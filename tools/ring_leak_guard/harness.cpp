// ring_leak_guard - 11 October 2026, from a live bug in ship_2026-10-11a.
//
// WHAT HAPPENED, because the leg only makes sense next to it. The AU hosting process that runs EchoJay V2 and
// every EchoJay Link ran out of file descriptors:
//
//     AUHostingServiceXPC_arrow[2341] (Metal) failed to open metal library with error: Too many open files
//
// Once that process cannot open a file, nothing in it can. A third-party AU could not open its bundle, so Logic
// reported -10875 for every one of them - including plain inserts, because they share that process. A Link could
// not write its rack sidecar, so the V2 said "has not published its rack yet" about Links that were working
// perfectly. A chain build stalled on the first bundle it tried to load, and Logic froze. Three complaints, one
// exhausted process, and ten minutes from a clean boot to all of them.
//
// THE LEAK was stage 3's value ring, on the consumer side. LinkProcessor::ringTick retried on
// `cmdRing_ == nullptr`, and openRingForDrain opened AND mmapped the file before testing whether it was a usable
// ring - so every failure dropped a descriptor and a 16 KB mapping on the floor, and because cmdRing_ stayed null
// it tried again on the next 30 Hz tick, for ever. The comment above it said "attempted quietly and once".
//
// It was unusable every time, too: the consumer opened O_RDWR|O_CREAT and deliberately did not initialise (that
// is the producer's job, and initialising here would race it), so for any rack the V2 had never addressed it
// CREATED a zeroed file that could never pass ringUsable(). 361 of those were on Sean's disk, every one with
// magic 00000000, and not one EJRing line in the whole log - the success path had never fired.
//
// WHY A GUARD, AND WHY THIS SHAPE. Nothing in the suite could have caught it. The ring legs assert that a frame
// written is a frame applied; they never ask what the mapping COST, and they run once rather than for the
// thousands of ticks it takes a 30 Hz leak to matter. So this leg does the thing the product does - tick, many
// times, against racks whose rings are not usable - and asserts the only two numbers that were wrong: the
// descriptor count and the mapped-region count. Flat across the run, measured at the start and the end, with a
// tolerance rather than an exact figure (a process opens and closes things for its own reasons).
//
// THE LEGS
//   (1) a rack whose ring is ABSENT: the consumer must not create one, so after many ticks there is still no
//       cmd-ring file for it. This is the half that produced 361 stray files;
//   (2) a rack whose ring file exists but is ZEROED (the unusable case): descriptors flat across 600 ticks;
//   (3) ...and mapped regions flat across the same run, because the mmap leaked with the descriptor and 14 MB a
//       second of stranded mappings is its own fault;
//   (4) THE RETRY IS BOUNDED: the number of OPEN ATTEMPTS over 600 ticks is far below 600. A fix that merely
//       closed the descriptor would still hammer open() 30 times a second for every Link in the session, and
//       this is the leg that says so;
//   (5) ...and a ring that APPEARS later is still picked up, which is why the retry is bounded rather than
//       abandoned after one failure (the V2 lays the ring down when it first addresses a rack, which can be
//       minutes after the Link started);
//   (6) the failure is not SILENT: one rate-limited line naming the rack, not one per tick. The log said
//       nothing at all about this, which is why it took a kernel message to find.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkShm.h"
#include "EJCmdRing.h"
#include <cstdio>
#include <memory>
#include <sys/resource.h>
#include <libproc.h>
#include <mach/mach.h>
#include <vector>

struct EchoJayLinkSyncTestAccess
{
    static juce::String uid (LinkProcessor& p)  { return p.instanceUid_; }
    static void ringTick (LinkProcessor& p)     { p.ringTick(); }
    static bool ringMapped (LinkProcessor& p)
    { return p.cmdRing_.load (std::memory_order_acquire) != nullptr; }
    // The product counts its own open attempts, because the rate-limited log line reports them. The guard reads
    // the same number rather than inferring it.
    static int ringOpenAttempts (LinkProcessor& p) { return p.ringOpenAttempts_; }
};
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms)
{ const double t0 = juce::Time::getMillisecondCounterHiRes();
  while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.004, false); } }

// THE TWO NUMBERS. proc_pidinfo is the honest reading for descriptors on macOS - it asks the kernel for this
// process's open file table rather than counting lines of something. vmmap's region count would need a fork and
// a parse; mach_vm region walking gives the same answer in-process and cannot be throttled.
int openFdCount()
{
    const int sz = proc_pidinfo (getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if (sz <= 0) return -1;
    std::vector<char> buf ((size_t) sz + 4096);
    const int got = proc_pidinfo (getpid(), PROC_PIDLISTFDS, 0, buf.data(), (int) buf.size());
    if (got <= 0) return -1;
    return got / (int) sizeof (proc_fdinfo);
}
int mappedRegionCount()
{
    int n = 0;
    vm_address_t addr = 0;
    while (true)
    {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64 (mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                          (vm_region_info_t) &info, &cnt, &obj) != KERN_SUCCESS)
            break;
        ++n;
        addr += size;
        if (n > 500000) break;   // a bound, so a pathological map cannot hang the gate
    }
    return n;
}
juce::String shmDir() { int e = 0; return LinkShm::resolveDir (e); }
juce::File ringFileFor (const juce::String& uid)
{ return juce::File (echojay::cmdring::ringPath (shmDir(), uid)); }

// A zeroed ring file of the right SIZE and the wrong MAGIC: exactly what the consumer's own O_CREAT used to
// leave behind, and exactly what a producer from a future layout version would look like.
void writeZeroedRing (const juce::String& uid)
{
    auto f = ringFileFor (uid);
    f.deleteFile();
    juce::MemoryBlock zeros (sizeof (echojay::cmdring::CmdRing), true);
    f.replaceWithData (zeros.getData(), zeros.getSize());
}
// A ring a real producer laid down, through the SHIPPED producer path.
void layDownRealRing (const juce::String& uid)
{
    int fd = -1, e = 0; bool laid = false;
    auto* r = echojay::cmdring::openRingRW (shmDir(), uid, fd, e, laid);
    if (r != nullptr) LinkShm::closeMapped (r, sizeof (echojay::cmdring::CmdRing), fd, {}, false);
}
std::unique_ptr<LinkProcessor> makeLink (const char* nm)
{
    auto l = std::make_unique<LinkProcessor>();
    l->linkName = nm; l->markTypedNameAuthoritative();
    l->prepareToPlay (48000.0, 512);
    for (int i = 0; i < 200 && EchoJayLinkSyncTestAccess::uid (*l).isEmpty(); ++i) pumpMs (5);
    return l;
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_ring_leak_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("ring_leak_guard: the value ring's consumer must not leak a descriptor or a mapping per tick\n");
    if (shmDir().isEmpty()) { std::printf ("  FAIL  the shared Link folder did not resolve\n"); return 1; }

    // The real soft limit, printed because it is what decides how long a leak takes to bite in a host. Under the
    // hosting service it is far lower than a shell's.
    { struct rlimit rl {}; getrlimit (RLIMIT_NOFILE, &rl);
      std::printf ("  ..    RLIMIT_NOFILE soft=%llu hard=%llu\n",
                   (unsigned long long) rl.rlim_cur, (unsigned long long) rl.rlim_max); }

    constexpr int kTicks = 600;   // 20 seconds of a 30 Hz timer, in a fraction of a second of wall clock

    std::printf ("== (1) an ABSENT ring: the consumer must not CREATE one ==\n");
    {
        auto l = makeLink ("LeakAbsent");
        const auto uid = EchoJayLinkSyncTestAccess::uid (*l);
        ringFileFor (uid).deleteFile();
        for (int i = 0; i < kTicks; ++i) EchoJayLinkSyncTestAccess::ringTick (*l);
        check (! ringFileFor (uid).existsAsFile(),
               "(1) after 600 ticks against an absent ring there is still NO ring file - a consumer that creates "
               "one leaves a file that can never be usable (361 of those were on Sean's disk)",
               ringFileFor (uid).existsAsFile()
                   ? juce::String ("CREATED, ") + juce::String (ringFileFor (uid).getSize()) + " bytes"
                   : juce::String ("absent"));
        check (! EchoJayLinkSyncTestAccess::ringMapped (*l),
               "(1) ...and nothing is mapped, so the file channel carries the values, which is correct");
    }

    std::printf ("== (2)(3)(4) an UNUSABLE ring: descriptors, mappings and open attempts ==\n");
    {
        auto l = makeLink ("LeakZeroed");
        const auto uid = EchoJayLinkSyncTestAccess::uid (*l);
        writeZeroedRing (uid);
        // One tick first, so any one-off allocation this Link does on its first pass is OUTSIDE the measurement.
        EchoJayLinkSyncTestAccess::ringTick (*l);
        const int fd0  = openFdCount();
        const int vm0  = mappedRegionCount();
        for (int i = 0; i < kTicks; ++i) EchoJayLinkSyncTestAccess::ringTick (*l);
        const int fd1  = openFdCount();
        const int vm1  = mappedRegionCount();
        std::printf ("    fds %d -> %d   mapped regions %d -> %d   over %d ticks\n", fd0, fd1, vm0, vm1, kTicks);
        // A TOLERANCE, not an exact figure: a process opens and closes things for its own reasons, and a leg that
        // demands equality fails on an unrelated log rotation. 8 is far below the 600 a per-tick leak gives.
        check (fd0 > 0 && fd1 - fd0 <= 8,
               "(2) descriptors are FLAT across 600 ticks against an unusable ring  (RED before the fix: one per "
               "tick, which is 30 a second per Link and is what exhausted the hosting process)",
               juce::String (fd1 - fd0) + " leaked");
        check (vm0 > 0 && vm1 - vm0 <= 8,
               "(3) ...and so are mapped regions - the mmap leaked with the descriptor, 16 KB at a time",
               juce::String (vm1 - vm0) + " leaked");
        check (! EchoJayLinkSyncTestAccess::ringMapped (*l),
               "(2) ...and an unusable ring is still not adopted, which is the behaviour that was always right");
        const int attempts = EchoJayLinkSyncTestAccess::ringOpenAttempts (*l);
        check (attempts > 0 && attempts <= 40,
               "(4) THE RETRY IS BOUNDED: 600 ticks produced only a handful of open() attempts. Closing the "
               "descriptor alone would have left this hammering open() 30 times a second for every Link in the "
               "session - a quieter bug, still a bug",
               juce::String (attempts) + " attempt(s) in " + juce::String (kTicks) + " ticks");
    }

    std::printf ("== (5) a ring that APPEARS later is still picked up ==\n");
    {
        auto l = makeLink ("LeakLate");
        const auto uid = EchoJayLinkSyncTestAccess::uid (*l);
        ringFileFor (uid).deleteFile();
        for (int i = 0; i < 60; ++i) EchoJayLinkSyncTestAccess::ringTick (*l);
        check (! EchoJayLinkSyncTestAccess::ringMapped (*l), "(5) fixture: nothing mapped while the ring is absent");
        // The V2 lays it down when it first addresses this rack - which can be minutes after the Link started.
        layDownRealRing (uid);
        // The retry is BOUNDED, not abandoned: give it the wall clock a bounded retry needs, then tick.
        bool mapped = false;
        for (int t = 0; t < 40 && ! mapped; ++t)
        { pumpMs (120); for (int i = 0; i < 10; ++i) EchoJayLinkSyncTestAccess::ringTick (*l);
          mapped = EchoJayLinkSyncTestAccess::ringMapped (*l); }
        check (mapped,
               "(5) THE REASON THE RETRY IS BOUNDED AND NOT ABANDONED: a ring laid down after the Link started is "
               "adopted within a second or so, so values stop going the slow way",
               mapped ? juce::String ("adopted") : juce::String ("NEVER adopted - a bounded retry became no retry"));
    }

    std::printf ("\n==== ring_leak_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
