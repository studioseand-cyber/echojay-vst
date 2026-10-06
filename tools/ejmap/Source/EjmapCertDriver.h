/*
  EjmapCertDriver.h

  EJ MAP AS THE DRIVER (certification plan step 5). The signed probe MEASURES; this
  file orchestrates it and DERIVES from what it prints (decision D2). Its first job is
  regenerating the pushed compressor-profile fixtures from live probe runs, and
  proving it by comparing field by field. Design requirements and the acceptance
  criterion are in docs/EJMAP_CERT_DRIVER.md; the parts that shape this code:

    - SIGN ONCE AND FAIL LOUDLY (doc section 1). The probe's signature is checked
      before the first product, under a deadline, and an unsigned or unverifiable
      probe aborts the run before any plugin is touched. With --sign-identity the
      driver signs ONCE first, also under a deadline, because a locked keychain
      makes codesign BLOCK rather than fail.
    - THE DRIVER OWNS EVERY TIMEOUT (doc section 2). Every probe run has a
      deadline enforced from outside, and the child is killed on expiry. The wait
      status is read with waitpid, never through juce::ChildProcess, whose
      getExitCode returns 0 for a child killed by a signal - a crash would read as a
      clean exit.
    - THE RETRY RULE (spec section 6, the Neve 33609 case). A run that is killed or
      dies abnormally is re-run ONCE, alone. A second failure is recorded with its
      reason. Two timeouts mark a hang. Every attempt is written to run.jsonl, and a
      retried product is named in the report.
    - UNLICENSED IS NOT BROKEN (doc section 3). PACE-wrapped products are HELD, not
      run, while step 2 is open (--include-pace overrides). A held product is
      reported as held, never as a failure.
    - COVERAGE FIRST (doc section 0). The report opens with what was reachable and
      what was not, and the out-of-reach products are listed by name.

  Two modes, both headless (Main.cpp runHeadlessCli):
    --cert-rederive <fixturesDir>
        Re-derives range, direction and unit from each fixture's OWN displayAt. No
        probe, no plugin, so it covers all 74. It proves the derivation code.
    --cert-defaults --fixtures <dir> --probe <EchoJayProbe> --out <dir>
                    [--timeout-s N] [--sign-identity ID --entitlements FILE] [--include-pace]
        Regenerates every reachable fixture through the probe and compares it.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <CoreFoundation/CoreFoundation.h>
#include "EchoJayAuRegistry.h"
#include "EjmapFixtureUnit.h"
#include "EjmapFixtureRange.h"
#include "EjmapCandidateRules.h"
#include "EjmapFixtureReadout.h"
#include "EjmapCertOutcome.h"
#include "EjmapSweep.h"
#include "EjmapPitch.h"
#include "EjmapProfileExport.h"
#include "EjmapLoop.h"
#include "EjmapSidechainCheck.h"
#include "EjmapGainCal.h"
#include "EjmapTiming.h"
#include "EjmapLimiter.h"
#include "EjmapEq.h"
#include "EjmapCertReview.h"
#include "EjmapSaturation.h"
#include "EjmapReverbDelay.h"
#include "EjmapDynamics.h"
#include "EjmapDeesser.h"
#include "EjmapMultiband.h"
#include "EjmapRoleEvidence.h"
#include "EjmapPhaseB.h"
#include "EjmapWindowWatch.h"
#include "EjmapWatchdog.h"
#include <sstream>
#include "EchoJayParamMaps.h"   // fingerprintForDescription: the join key, one function for all three sides

#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach/mach_time.h>
#include <functional>
#include <libproc.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <map>
#include <set>
#include <fstream>
#include <vector>
#include <iostream>

extern char** environ;

namespace ejmap::cert
{

//==============================================================================
// A CHILD PROCESS WITH A DEADLINE, AND ITS EXACT WAIT STATUS.
struct ChildResult
{
    // uiShown: a window belonging to the child's process tree came on screen, and
    // the driver killed the run at once. The probe never shows a window of its own
    // (LSBackgroundOnly, no editor), so any window in its tree is a plugin's or a
    // licence wrapper's - e.g. PACE's "Software Activation" (PACEEdenExperience),
    // a child of the probe, seen 28 Sep on Eiosis E2Deesser.
    enum class Kind { exited, signaled, timedOut, uiShown, spawnFailed, sleptTwice } kind = Kind::spawnFailed;
    int code = 0;             // exit code when exited
    int signal = 0;           // terminating signal when signaled
    juce::String out;         // stdout and stderr, interleaved
    juce::StringArray windowsInTree;   // owner names of on-screen windows the child's tree showed
    double ms = 0.0;                   // AWAKE time the child ran for (the clock every timeout is measured on)
    double sleptMs = 0.0;              // continuous minus awake time over the run: > 0 means the Mac slept during it

    bool cleanExit (int wanted = 0) const { return kind == Kind::exited && code == wanted; }
    juce::String describe() const
    {
        switch (kind)
        {
            case Kind::exited:      return "exit " + juce::String (code);
            case Kind::signaled:    return "killed by signal " + juce::String (signal)
                                           + (signal == SIGTERM ? " (SIGTERM: a caller's timeout, not a refusal)" : "");
            case Kind::timedOut:    return "timed out after " + juce::String (ms / 1000.0, 1) + " s (killed by the driver)";
            case Kind::uiShown:     return "SHOWED A WINDOW (" + windowsInTree.joinIntoString (", ")
                                           + ") after " + juce::String (ms / 1000.0, 1) + " s; killed by the driver";
            case Kind::spawnFailed: return "could not be started";
            case Kind::sleptTwice:  return "the Mac slept during both attempts: refused (a measurement across a wake is not trusted)";
        }
        return "?";
    }
};

//==============================================================================
// WINDOW WATCHING (docs/EJMAP_CERT_DRIVER.md section 3: a precondition before any
// PACE subject joins). An activation dialog blocks instantiation, so without this
// it can only ever surface as a timeout, indistinguishable from a hang.
using windowwatch::parentOf;
using windowwatch::inTreeOf;

inline juce::StringArray windowsOwnedByTree (pid_t root, bool onScreenOnly = true)
{
    juce::StringArray owners;
    const CGWindowListOption opt = onScreenOnly ? kCGWindowListOptionOnScreenOnly : kCGWindowListOptionAll;
    CFArrayRef list = CGWindowListCopyWindowInfo (opt, kCGNullWindowID);
    if (list == nullptr) return owners;
    for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
    {
        auto w = (CFDictionaryRef) CFArrayGetValueAtIndex (list, i);
        auto pidRef = (CFNumberRef) CFDictionaryGetValue (w, kCGWindowOwnerPID);
        int pid = 0;
        if (pidRef == nullptr || ! CFNumberGetValue (pidRef, kCFNumberIntType, &pid)) continue;
        if (! inTreeOf ((pid_t) pid, root)) continue;
        auto name = (CFStringRef) CFDictionaryGetValue (w, kCGWindowOwnerName);
        owners.addIfNotAlreadyThere ((name != nullptr ? juce::String::fromCFString (name) : juce::String ("?"))
                                     + " [pid " + juce::String (pid) + "]");
    }
    CFRelease (list);
    return owners;
}

struct WatchOptions { bool watchWindows = true; bool onScreenOnly = true; int pollMs = 250; };

//==============================================================================
// SLEEP (ruled 29 Sep; spec section 6: unattended runs happen on laptops).
//
// THE CLOCK. Every timeout is measured on AWAKE time, so a Mac that sleeps mid-run never turns slept time into a
// "hang". Measured 29 Sep on this Mac: mach_absolute_time (JUCE's getMillisecondCounterHiRes) read 518,794 s against
// 1,031,532 s of wall time since boot - it excludes the 5.9 days slept. runChild already timed out on that clock, so
// batch 2's sleeps could not fake a timeout; this makes it explicit, injectable and PINNED instead of a property of
// JUCE's implementation. A second clock that DOES count sleep (mach_continuous_time: not moved by NTP or a user
// changing the time, unlike wall time) is read beside it to measure how long the Mac slept during each process.
inline double continuousMs()
{
    static const double ratio = [] { mach_timebase_info_data_t tb; mach_timebase_info (&tb); return (double) tb.numer / (double) tb.denom / 1.0e6; }();
    return (double) mach_continuous_time() * ratio;
}
struct Clock
{
    std::function<double()> awakeMs     = [] { return juce::Time::getMillisecondCounterHiRes(); };
    std::function<double()> withSleepMs = [] { return continuousMs(); };
};

// A process that slept longer than this during its run is not trusted (ruled 29 Sep): a measurement across a wake
// can be corrupt AND plausible - RCompressor (s) position 5 read its ratio as 0 through a bridge that died across a
// dark wake, and rendered 2.1 M no-op blocks inside 500 ms. It is re-run ONCE; slept again, it is refused, never looped.
inline constexpr double kSleptMs = 1000.0;

// THE ASSERTION. Held for the whole of a run: no system sleep (honoured on AC power, including with the lid closed) and
// no idle sleep. It CANNOT stop a lid-close on battery - batch 2's sleep (29 Sep, 18:36) was exactly that, "Clamshell
// Sleep" on 9% battery, with idle sleep already prevented system-wide - which is why the awake clock above matters.
struct SleepGuard
{
    IOPMAssertionID system = 0, idle = 0;
    bool systemHeld = false, idleHeld = false;
    explicit SleepGuard (const juce::String& why)
    {
        const auto reason = CFStringCreateWithCString (kCFAllocatorDefault, why.toRawUTF8(), kCFStringEncodingUTF8);
        systemHeld = IOPMAssertionCreateWithName (kIOPMAssertionTypePreventSystemSleep, kIOPMAssertionLevelOn, reason, &system) == kIOReturnSuccess;
        idleHeld   = IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn, reason, &idle) == kIOReturnSuccess;
        CFRelease (reason);
    }
    ~SleepGuard()
    {
        if (systemHeld) IOPMAssertionRelease (system);
        if (idleHeld) IOPMAssertionRelease (idle);
    }
    juce::String describe() const
    {
        return juce::String ("sleep assertions: system ") + (systemHeld ? "held" : "REFUSED") + ", idle " + (idleHeld ? "held" : "REFUSED")
               + " (a lid-close on battery sleeps regardless; timeouts count awake time only)";
    }
};

// THE TIMEOUT ARITHMETIC, alone so it can be pinned: elapsed is AWAKE time.
inline bool timeoutPassed (double startAwakeMs, double nowAwakeMs, int timeoutMs) { return nowAwakeMs - startAwakeMs > timeoutMs; }

inline ChildResult runChild (const juce::StringArray& args, int timeoutMs, WatchOptions watch = {}, const Clock& clock = {})
{
    ChildResult r;
    int fds[2];
    if (args.isEmpty() || pipe (fds) != 0) return r;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init (&fa);
    posix_spawn_file_actions_adddup2 (&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2 (&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose (&fa, fds[0]);

    std::vector<std::string> store;
    for (const auto& a : args) store.push_back (a.toStdString());
    std::vector<char*> argv;
    for (auto& s : store) argv.push_back (s.data());
    argv.push_back (nullptr);

    pid_t pid = 0;
    const double t0 = clock.awakeMs(), w0 = clock.withSleepMs();
    const int rc = posix_spawn (&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy (&fa);
    close (fds[1]);
    if (rc != 0) { close (fds[0]); return r; }

    fcntl (fds[0], F_SETFL, O_NONBLOCK);
    juce::MemoryOutputStream collected;
    int status = 0;
    bool reaped = false, killed = false, uiKilled = false;
    double lastWindowPoll = 0.0;
    char buf[8192];
    auto drain = [&] { for (;;) { const ssize_t n = read (fds[0], buf, sizeof buf); if (n > 0) collected.write (buf, (size_t) n); else break; } };
    for (;;)
    {
        pollfd p { fds[0], POLLIN, 0 };
        poll (&p, 1, 20);
        drain();
        if (! reaped && waitpid (pid, &status, WNOHANG) == pid) reaped = true;
        if (reaped) { drain(); break; }
        const double now = clock.awakeMs();
        if (watch.watchWindows && now - lastWindowPoll >= watch.pollMs)
        {
            lastWindowPoll = now;
            const auto shown = windowsOwnedByTree (pid, watch.onScreenOnly);
            if (! shown.isEmpty())
            {
                r.windowsInTree = shown;
                // The whole tree goes: the wrapper's UI process is a child of the probe.
                for (int k = 0; k < 16; ++k)
                {
                    bool any = false;
                    int n = proc_listchildpids (pid, nullptr, 0);
                    std::vector<pid_t> kids ((size_t) juce::jmax (0, n) + 8);
                    n = proc_listchildpids (pid, kids.data(), (int) (kids.size() * sizeof (pid_t)));
                    for (int c = 0; c < n; ++c) if (kids[(size_t) c] > 1) { kill (kids[(size_t) c], SIGKILL); any = true; }
                    if (! any) break;
                }
                kill (pid, SIGKILL);
                waitpid (pid, &status, 0);
                uiKilled = reaped = true;
                drain();
                break;
            }
        }
        if (timeoutPassed (t0, now, timeoutMs))
        {
            kill (pid, SIGKILL);
            waitpid (pid, &status, 0);
            killed = reaped = true;
            drain();
            break;
        }
    }
    close (fds[0]);
    r.ms  = clock.awakeMs() - t0;
    r.sleptMs = juce::jmax (0.0, (clock.withSleepMs() - w0) - r.ms);
    r.out = collected.toString();
    if (uiKilled)                r.kind = ChildResult::Kind::uiShown;
    else if (killed)             r.kind = ChildResult::Kind::timedOut;
    else if (WIFEXITED (status))   { r.kind = ChildResult::Kind::exited;   r.code = WEXITSTATUS (status); }
    else if (WIFSIGNALED (status)) { r.kind = ChildResult::Kind::signaled; r.signal = WTERMSIG (status); }
    return r;
}

//==============================================================================
// THE SIGNATURE GATE. Sign once (optional) under a deadline, then verify.
// THE RETRY RULE, ONE IMPLEMENTATION for every probe the driver runs (--cert-defaults and --cert-sweep alike).
//   - a refusal (exit 3) and a window are ANSWERS: never retried;
//   - anything else unclean is re-run ONCE (the SIGTERM rule: a killed probe is not a refusal);
//   - a clean run during which the Mac SLEPT is re-run ONCE too (ruled 29 Sep), because its numbers may be corrupt and
//     still look right; slept AGAIN, it is refused (`sleptTwice`) - recorded, never looped.
// `onAttempt` sees every attempt, so every attempt's output is kept.
struct Attempted { ChildResult r; int attempts = 0; bool sleptTwice = false; };
inline bool isAnswer (const ChildResult& r) { return (r.kind == ChildResult::Kind::exited && r.code == 3) || r.kind == ChildResult::Kind::uiShown; }
inline Attempted runWithRetry (const juce::StringArray& args, int timeoutMs, const std::function<void (int, const ChildResult&)>& onAttempt,
                               const Clock& clock = {}, WatchOptions watch = {})
{
    Attempted a;
    for (int attempt = 1; attempt <= 2; ++attempt)
    {
        a.r = runChild (args, timeoutMs, watch, clock);
        a.attempts = attempt;
        if (onAttempt) onAttempt (attempt, a.r);
        const bool slept = a.r.sleptMs > kSleptMs;
        if (isAnswer (a.r) || (a.r.cleanExit() && ! slept)) return a;
        if (attempt == 2) a.sleptTwice = slept;
    }
    return a;
}

struct ProbeIdentity { bool ok = false; juce::String why, team, cdhash; };

inline ProbeIdentity checkProbe (const juce::File& probe, const juce::String& signIdentity,
                                 const juce::File& entitlements)
{
    ProbeIdentity id;
    if (! probe.existsAsFile()) { id.why = "no probe at " + probe.getFullPathName(); return id; }
    if (signIdentity.isNotEmpty())
    {
        auto s = runChild ({ "/usr/bin/codesign", "-f", "-s", signIdentity, "--timestamp", "--options", "runtime",
                             "--entitlements", entitlements.getFullPathName(), probe.getFullPathName() }, 20000);
        if (s.kind == ChildResult::Kind::timedOut)
        { id.why = "SIGNING BLOCKED: codesign did not return within 20 s. A keychain prompt is the likely cause "
                   "(locked keychain, or a key not set to Always Allow). Nothing was run."; return id; }
        if (! s.cleanExit()) { id.why = "signing failed (" + s.describe() + "): " + s.out.trim(); return id; }
    }
    auto v = runChild ({ "/usr/bin/codesign", "--verify", "--strict", probe.getFullPathName() }, 20000);
    if (! v.cleanExit()) { id.why = "the probe is not validly signed (" + v.describe() + "): " + v.out.trim()
                                    + ". PACE refuses an unsigned probe, so nothing was run."; return id; }
    auto d = runChild ({ "/usr/bin/codesign", "-dvvv", probe.getFullPathName() }, 20000);
    for (auto line : juce::StringArray::fromLines (d.out))
    {
        line = line.trim();
        if (line.startsWith ("TeamIdentifier=")) id.team = line.fromFirstOccurrenceOf ("=", false, false);
        if (line.startsWith ("CDHash="))         id.cdhash = line.fromFirstOccurrenceOf ("=", false, false);
    }
    if (id.team.isEmpty() || id.team == "not set")
    { id.why = "the probe verifies but has no Team Identifier (ad-hoc signed). PACE needs a Developer ID "
               "signature, so nothing was run."; return id; }
    id.ok = true;
    return id;
}

//==============================================================================
// PACE, by the probe line's own rule (merge/kathy-2026-09-06 Source/EJPaceCheck.h
// isPaceWrapped, at 971e4c1). Ported, not included: that header is not on this
// branch. If the rule changes there, it must change here.
inline bool isPaceWrapped (const juce::File& bundle)
{
    const auto contents = bundle.getChildFile ("Contents");
    if (contents.getChildFile ("__Pace_Eden.bundle").exists()
        || contents.getChildFile ("Resources").getChildFile ("__Pace_Eden.bundle").exists())
        return true;
    for (const auto& f : contents.getChildFile ("Frameworks").findChildFiles (juce::File::findFilesAndDirectories, false))
        if (f.getFileName().containsIgnoreCase ("pace") || f.getFileName().containsIgnoreCase ("eden"))
            return true;
    for (const auto& bin : contents.getChildFile ("MacOS").findChildFiles (juce::File::findFiles, false))
    {
        juce::MemoryBlock mb;
        if (! bin.loadFileAsData (mb)) continue;
        for (const char* needle : { "PACEAntiPiracy", "com.paceap", "PaceAP", "__Pace_Eden" })
        {
            const auto* data = static_cast<const char*> (mb.getData());
            const size_t n = mb.getSize(), k = std::strlen (needle);
            for (size_t i = 0; i + k <= n; ++i)
                if (data[i] == needle[0] && std::memcmp (data + i, needle, k) == 0)
                    return true;
        }
    }
    return false;
}

// "type,subtype,manufacturer" -> the .component bundle that registers it, from each
// bundle's own Info.plist. Built-in system components have no bundle here.
inline std::map<juce::String, juce::File> componentBundles()
{
    std::map<juce::String, juce::File> out;
    for (const auto& dir : { juce::File ("/Library/Audio/Plug-Ins/Components"),
                             juce::File ("~/Library/Audio/Plug-Ins/Components") })
        for (const auto& b : dir.findChildFiles (juce::File::findDirectories, false, "*.component"))
        {
            const auto path = b.getFullPathName();
            auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(),
                                                                (CFIndex) path.getNumBytesAsUTF8(), true);
            if (url == nullptr) continue;
            if (auto bundle = CFBundleCreate (nullptr, url))
            {
                if (auto comps = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (bundle, CFSTR ("AudioComponents")))
                    if (CFGetTypeID (comps) == CFArrayGetTypeID())
                        for (CFIndex i = 0; i < CFArrayGetCount (comps); ++i)
                            if (auto d = (CFDictionaryRef) CFArrayGetValueAtIndex (comps, i))
                                if (CFGetTypeID (d) == CFDictionaryGetTypeID())
                                {
                                    auto s = [d] (CFStringRef k) {
                                        auto v = (CFStringRef) CFDictionaryGetValue (d, k);
                                        return v != nullptr && CFGetTypeID (v) == CFStringGetTypeID()
                                                 ? juce::String::fromCFString (v) : juce::String(); };
                                    const auto key = s (CFSTR ("type")) + "," + s (CFSTR ("subtype")) + "," + s (CFSTR ("manufacturer"));
                                    out.emplace (key, b);
                                }
                CFRelease (bundle);
            }
            CFRelease (url);
        }
    return out;
}

//==============================================================================
// REACHABILITY of one fixture on this machine.
struct Subject
{
    juce::File fixtureFile;
    juce::var pushed;
    juce::String product, uid, version;
    juce::String category = "compressor";   // "compressor" | "pitch": which certification the subject gets (ONE STORE, ruled 1 Oct)
    enum class Reach { reachable, reachableNoVersion, heldPace, requiresHardware, versionMismatch, notInstalled, ambiguous, unfixtured } reach
        = Reach::notInstalled;
    juce::String detail;                 // installed version(s), the ambiguity, ...
    juce::String mapState;               // INFORMATION (ruled 2 Oct): "local map" | "server map state N" | "server map at a different build" | "none"
    juce::PluginDescription desc;        // resolved component, when installed
    bool licenceBound = false;           // PACE-wrapped, or its PACE state could not be checked
    // versionMismatch only: the ONE installed version, resolved for MEASUREMENT (see measurable()).
    bool installedUnique = false, hardware = false;
};

// THE VERSION GUARD BELONGS ON COMPARISON, NOT ON MEASUREMENT (ruled 29 Sep). A fixture made at another
// version cannot be REPRODUCED here, so --cert-defaults still skips it; but a fresh measurement at the
// installed version is valid and writes a NEW identity. Section 6 runs EJ Map on other people's Macs, where
// a version nobody has sampled is the normal case, not a workaround. Measurable when:
//   - reachable, with or without a recorded version; or
//   - installed at another version, exactly ONE version installed (several: never pick one), not a
//     hardware product, and not licence-bound unless PACE is included.
// NO PACE-MARKER HOLD (ruled 2 Oct): PACE-wrapped is not unlicensed; the window is the evidence. The batch carries
// forward what the SCAN observed (licence-stops.json: needs_licence, not loaded again) and the probe's own window
// watch guards every batch load (a licence can vanish between scan and batch). includePace is kept for the call
// sites' shape and does nothing.
inline bool measurable (const Subject& s, bool /*includePace*/)
{
    if (s.reach == Subject::Reach::reachable || s.reach == Subject::Reach::reachableNoVersion) return true;
    if (s.reach == Subject::Reach::unfixtured) return ! s.hardware;
    return s.reach == Subject::Reach::versionMismatch && s.installedUnique && ! s.hardware;
}

// PLUGINS THAT NEED EXTERNAL HARDWARE (logged 29 Sep, docs/EJMAP_CERT_DRIVER.md
// section 3): they may enumerate, but they render nothing without the hardware, so they
// are RECORDED, never measured and never scored. The only known members so far are McDSP's
// APB plugins, which need the Analog Processing Box (per Kathy). The rule is by
// manufacturer code and product name, not by guess. Add to it only on evidence.
//
// UAD-2 joins it (ruled 29 Sep): every UAD-2 product (manufacturer code "!UAD") opened its own dialog at the FIRST RENDER
// on this Mac, which has no UAD hardware attached; defaults sampling, which never renders, was clean. Consistent with
// needing the hardware, not proven - the window watch cannot read titles. All 215 UAD components installed here are
// com.uaudio.effects with "!UAD"; none is a UADx (native) build, which would not need hardware.
// The category is CONDITIONAL, not uncertifiable: attaching the hardware makes it one re-run.
inline juce::String externalHardwareNeeded (const juce::String& product, const juce::String& componentCode)
{
    if (componentCode.endsWith (",McDP") && product.startsWith ("APB ")) return "McDSP APB hardware";
    if (componentCode.endsWith (",!UAD")) return "UAD hardware (UAD-2)";
    return {};
}
inline bool requiresExternalHardware (const juce::String& product, const juce::String& componentCode)
{
    return externalHardwareNeeded (product, componentCode).isNotEmpty();
}

inline juce::String reachName (Subject::Reach r)
{
    switch (r)
    {
        case Subject::Reach::reachable:          return "reachable";
        case Subject::Reach::reachableNoVersion: return "reachable (fixture records no version)";
        case Subject::Reach::heldPace:           return "held: PACE-wrapped or unverifiable (run with --include-pace)";
        case Subject::Reach::requiresHardware:   return "not measured: requires external hardware";
        case Subject::Reach::versionMismatch:    return "out of reach: installed at another version";
        case Subject::Reach::notInstalled:       return "out of reach: not installed";
        case Subject::Reach::ambiguous:          return "out of reach: uid matches more than one component";
        case Subject::Reach::unfixtured:         return "discovered: mapped, no fixture at its installed version";
    }
    return "?";
}

// WHAT COUNTS AS A RECORD (ruled 30 Sep). A fixture leaves the worklist when it records what happened to the product:
// a threshold sweep, every candidate's sweep (no thresholdSweep by design, a human picks), or a REFUSAL - the product
// was stopped before a usable measurement and the fixture says at which stage and why. Before this rule only a
// thresholdSweep counted, so a candidates fixture went straight back on the list, and a product stopped in the defaults
// phase wrote nothing and was rediscovered on every batch, forever.
// ONE STORE (ruled 1 Oct): compressor and tuner records live in the same directory, discriminated by the record's own
// `schema` field - the store is where records live, the schema is what a record says. Sean's ej_comp_profile/1 is the
// server-side projection and is unchanged by this; these are the certification records it is built from.
using sweep::kSchemaCompressor;
using sweep::kSchemaTuner;
inline bool sweepRecorded (const juce::var& fixture)
{
    return fixture.getProperty ("thresholdSweep", {}).isObject()
        || fixture.getProperty ("thresholdCandidates", {}).isArray()
        || fixture.getProperty ("pitchCandidates", {}).isArray()
        || fixture.getProperty ("thresholdRefusal", {}).isObject();
}
inline bool refusalRecorded (const juce::var& fixture) { return fixture.getProperty ("thresholdRefusal", {}).isObject(); }

// TRANSIENT OR PERMANENT (ruled 30 Sep). A refusal at these stages is a fact about the PRODUCT and a re-run cannot change
// it: the plan found no threshold role, or the ratio never reads 4:1. Everything else is a fact about the RUN - a hang,
// a window (the iLok was out), the budget, a sleep, a silent reference - and a re-run after fixing it is the point.
// --retry-refused re-runs the transient ones; retrying the permanent ones indiscriminately would re-run the
// uncertifiable on every batch, the very jam the record exists to escape. --retry-refused-all overrides.
inline bool refusalIsPermanent (const juce::String& stage) { return stage == "plan" || stage == "ratio_none" || stage == "ara_only"; }

// A RETRIED REFUSAL MAY HAVE NO CONTROLS (found 30 Sep on SSL G3's first --retry-refused): a record written in the
// defaults phase is the discovered identity and nothing else. Planned from as a fixture, it reads "0 controls hold
// the threshold role" - a permanent refusal manufactured from an empty record. So defaults are sampled first for an
// unseen version, a discovered product, AND any subject whose record carries no controls.
inline bool needsDefaultsFirst (const Subject& s)
{
    return s.reach == Subject::Reach::versionMismatch || s.reach == Subject::Reach::unfixtured
        || ! s.pushed.getProperty ("controls", {}).isArray();
}
inline bool refusalPermanent (const juce::var& fixture)
{
    return refusalRecorded (fixture) && refusalIsPermanent (fixture.getProperty ("thresholdRefusal", {}).getProperty ("stage", "").toString());
}

inline std::vector<Subject> loadFixtures (const juce::File& dir)
{
    std::vector<Subject> out;
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.json");
    files.sort();
    for (const auto& f : files)
    {
        // The defaults SIDECAR (<stem>.defaults.json) is the sample a sweep was planned on, kept beside the fixture; it is
        // not a subject. Loaded as one it carried the same uid with no sweep, and re-entered the worklist every run.
        if (f.getFileName().endsWith (".defaults.json")) continue;
        Subject s;
        s.fixtureFile = f;
        s.pushed = juce::JSON::parse (f.loadFileAsString());
        if (! s.pushed.isObject()) continue;
        s.product = s.pushed.getProperty ("product", "").toString();
        s.uid     = s.pushed.getProperty ("uid", "").toString().toLowerCase();
        s.version = s.pushed.getProperty ("version", "").toString();
        if (s.pushed.getProperty ("category", "").toString() == "pitch" || s.pushed.getProperty ("schema", "").toString() == kSchemaTuner) s.category = "pitch";
        out.push_back (s);
    }
    return out;
}

// THE HOLD FAILS SAFE. First version: "no bundle found" meant "not PACE", and the two McDSP APB compressors ran while
// step 2 was open - PACE-wrapped (Eden bundle and PACE bytes both present), but registered without an AudioComponents
// key, so no plist scan finds them. A component whose PACE state cannot be CHECKED is held, and says so. Apple's own
// built-ins have no bundle here and are exempt. One rule, used by classify and by discovery.
inline bool paceHeld (const juce::PluginDescription& d, const std::map<juce::String, juce::File>& bundles, juce::String& why)
{
    const auto code = d.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false);
    auto it = bundles.find (code);
    if (it != bundles.end())
    {
        if (isPaceWrapped (it->second)) { why = "PACE-wrapped (" + it->second.getFileName() + ")"; return true; }
        return false;
    }
    if (code.endsWith (",appl")) return false;      // macOS built-in, no bundle to check
    why = "no bundle found for " + code + ", so its PACE state cannot be checked: held, not assumed clear";
    return true;
}

inline void classify (std::vector<Subject>& subjects, bool includePace)
{
    const auto bundles = componentBundles();
    std::multimap<juce::String, juce::PluginDescription> byUid;      // registry, resolved once
    const auto census = echojay::auregistry::buildCensus();
    for (const auto& t : census.targets)
    {
        auto d = echojay::auregistry::describeFromRegistry (t.identifier);
        if (d.fileOrIdentifier.isNotEmpty())
            byUid.emplace (juce::String::toHexString (d.uniqueId).toLowerCase(), d);
    }
    auto isPace = [&bundles] (const juce::PluginDescription& d, juce::String& why) { return paceHeld (d, bundles, why); };

    for (auto& s : subjects)
    {
        // The three fixtures whose `version` holds the component code instead of a
        // version: resolve that exact component (Apple's are outside the census).
        if (s.version.containsChar (','))
        {
            auto d = echojay::auregistry::describeFromRegistry ("AudioUnit:Effects/" + s.version);
            if (d.fileOrIdentifier.isEmpty() || d.name.isEmpty() || d.name == "<Unknown>")
            { s.reach = Subject::Reach::notInstalled; s.detail = "component " + s.version + " is not registered"; continue; }
            s.desc = d;
            if (requiresExternalHardware (s.product, s.version))
            { s.reach = Subject::Reach::requiresHardware; s.detail = "needs " + externalHardwareNeeded (s.product, s.version) + " present; recorded, not measured - attaching it makes this one re-run"; continue; }
            juce::String why;
            s.licenceBound = isPace (d, why);
            const bool held = ! includePace && s.licenceBound;
            s.detail = held ? why : "installed " + d.version + "; the fixture records no version to check it against";
            s.reach = held ? Subject::Reach::heldPace : Subject::Reach::reachableNoVersion;
            continue;
        }
        juce::Array<juce::PluginDescription> atVersion;
        juce::StringArray versions;
        for (auto [it, end] = byUid.equal_range (s.uid); it != end; ++it)
        {
            versions.addIfNotAlreadyThere (it->second.version);
            if (it->second.version == s.version) atVersion.add (it->second);
        }
        if (versions.isEmpty()) { s.reach = Subject::Reach::notInstalled; continue; }
        if (atVersion.isEmpty())
        {
            s.reach = Subject::Reach::versionMismatch;
            s.detail = "fixture " + s.version + ", installed " + versions.joinIntoString (" / ");
            juce::Array<juce::PluginDescription> installed;
            for (auto [it, end] = byUid.equal_range (s.uid); it != end; ++it) installed.add (it->second);
            if (installed.size() == 1)
            {
                s.desc = installed.getReference (0);
                s.installedUnique = true;
                s.hardware = requiresExternalHardware (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false));
                if (s.hardware) s.detail << "; needs " << externalHardwareNeeded (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)) << " present";
                juce::String why;
                s.licenceBound = isPace (s.desc, why);
            }
            continue;
        }
        if (atVersion.size() > 1)
        {
            s.reach = Subject::Reach::ambiguous;
            for (const auto& d : atVersion) s.detail << d.fileOrIdentifier << " ";
            continue;                                   // never pick one
        }
        s.desc = atVersion.getReference (0);
        if (requiresExternalHardware (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)))
        { s.reach = Subject::Reach::requiresHardware; s.detail = "needs " + externalHardwareNeeded (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)) + " present; recorded, not measured - attaching it makes this one re-run"; continue; }
        juce::String why;
        s.licenceBound = isPace (s.desc, why);
        const bool held = ! includePace && s.licenceBound;
        if (held) s.detail = why;
        s.reach = held ? Subject::Reach::heldPace : Subject::Reach::reachable;
    }
}

//==============================================================================
// THE PROBE'S OUTPUT, parsed. Lines are tab-separated; see au_instantiate_probe.cpp.
struct ListRow { juce::String name, label; int numSteps = 0; bool discrete = false, automatable = false, meta = false; };
struct TextAtRow { int index = -1; juce::String name, label, defText; double defNorm = 0, declared = 0;
                   std::map<juce::String, juce::String> at; };

inline std::map<int, ListRow> parseListParams (const juce::String& out)
{
    std::map<int, ListRow> rows;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() != 7 || ! f[0].containsOnly ("0123456789") || f[0].isEmpty()) continue;
        ListRow r { f[1], f[2], f[3].getIntValue(), f[4] == "1", f[5] == "1", f[6] == "1" };
        rows[f[0].getIntValue()] = r;
    }
    return rows;
}

inline std::vector<TextAtRow> parseTextAt (const juce::String& out)
{
    std::vector<TextAtRow> rows;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() >= 5 && f[0] == "param" && f[3] == "unit")
            rows.push_back ({ f[1].getIntValue(), f[2], f[4] });
        else if (f.size() >= 5 && f[0] == "def" && ! rows.empty())
        { rows.back().defNorm = f[1].getDoubleValue(); rows.back().defText = f[2]; rows.back().declared = f[4].getDoubleValue(); }
        else if (f.size() >= 2 && f[0] == "at" && ! rows.empty())
            rows.back().at[f[1]] = f.size() >= 3 ? f[2] : juce::String();
    }
    return rows;
}

// READOUT DETECTION: a control whose value differs between two FRESH instances is a readout (item 12).
// One function, used by --cert-defaults and by the sweep's defaults-first pass for an unseen version.
struct ReadoutHit { int index; juce::String name; double a, b; juce::String ta, tb; };
struct ReadoutScan { bool checked = false; juce::String note; std::vector<ReadoutHit> hits; std::set<int> positions; };
inline ReadoutScan scanReadouts (const std::vector<TextAtRow>& text, const ChildResult& second)
{
    ReadoutScan r;
    std::map<int, int> positionOf;                      // control index -> position in controls[]
    for (size_t i = 0; i < text.size(); ++i) positionOf[text[i].index] = (int) i;
    if (! second.cleanExit()) { r.note = "readout check incomplete: second instance " + second.describe(); return r; }
    const auto again = parseTextAt (second.out);
    std::map<int, const TextAtRow*> byIndex;
    for (const auto& t : again) byIndex[t.index] = &t;
    r.checked = true;
    for (const auto& t : text)
    {
        auto it = byIndex.find (t.index);
        if (it == byIndex.end()) { r.note = "second instance lacks control " + juce::String (t.index); r.checked = false; break; }
        if (it->second->defNorm != t.defNorm || it->second->defText != t.defText)
        {
            r.hits.push_back ({ t.index, t.name, t.defNorm, it->second->defNorm, t.defText, it->second->defText });
            r.positions.insert (positionOf[t.index]);
        }
    }
    return r;
}

//==============================================================================
// THE FIXTURE, composed from probe output through the committed derivations.
inline const char* kProfileNote  = "PROFILE ONLY - nothing certified, nothing written to the plugin.";
inline const char* kDefaultsNote = "the value the plugin instantiated with; declaredDefault is what the plugin calls "
                                   "its default - they are not always the same";

inline juce::var composeFixture (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                 int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date);
inline juce::var composeFixtureImpl (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                     int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date);
inline juce::var composeFixture (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                 int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date)
{
    auto f = composeFixtureImpl (s, list, textAt, listRc, textAtRc, probeLabel, date);
    if (auto* o = f.getDynamicObject()) o->setProperty ("schema", s.category == "pitch" ? kSchemaTuner : kSchemaCompressor);
    return f;
}
inline juce::var composeFixtureImpl (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                     int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date)
{
    // IDENTITY FROM THE RESOLVED COMPONENT, never copied from the fixture: copying
    // would make these fields match by construction and the comparison circular.
    // The three version-less fixtures carry the component code in `version`, so for
    // them the code is what the component is asked for.
    const auto uid = juce::String::toHexString (s.desc.uniqueId).toLowerCase();
    const auto version = s.reach == Subject::Reach::reachableNoVersion
                           ? s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)
                           : s.desc.version;
    auto* o = new juce::DynamicObject();
    o->setProperty ("identity", "AudioUnit|" + uid + "|" + version);
    o->setProperty ("product", s.desc.name);
    o->setProperty ("format", s.desc.pluginFormatName.isNotEmpty() ? s.desc.pluginFormatName : juce::String ("AudioUnit"));
    // the vendor as the host registers it (the export's plugin.manufacturer): the 103 store records got it stamped on
    // 1 Oct by hand; a record made since by discovery had none (CL 1B, 2 Oct)
    if (s.desc.manufacturerName.isNotEmpty()) o->setProperty ("manufacturer", s.desc.manufacturerName);
    if (s.mapState.isNotEmpty()) o->setProperty ("mapState", s.mapState);   // INFORMATION (ruled 2 Oct): certification does not wait on a map
    o->setProperty ("uid", uid);
    o->setProperty ("version", version);
    o->setProperty ("sampledAt", date);
    o->setProperty ("certified", false);
    o->setProperty ("note", kProfileNote);
    o->setProperty ("probe", probeLabel);
    o->setProperty ("listParamsRc", listRc);
    o->setProperty ("textAtRc", textAtRc);
    // THE JOIN KEY (ruled 1 Oct, the highest-stakes small fix): map_fp = SHA-256(format|uidHex|version|param_count) by the
    // ONE shared function EchoJay and EJ Map's mapper use, with param_count from the probe's --list-params rows - the
    // instance's getParameters().size() - and NEVER from controls.length: `controls` is built from the text-at rows and a
    // parameter the text pass skipped is not a control (NEOLD U2A: 13 rows, 11 controls; 5 of 92 fixtures hashed wrong
    // and would have failed the join SILENTLY). Reproduced against every local map: pin M1.
    o->setProperty ("param_count", (int) list.size());
    o->setProperty ("map_fp", echojay::fingerprintForDescription (s.desc, (int) list.size()));
    juce::Array<juce::var> controls;
    for (const auto& t : textAt)
    {
        auto* c = new juce::DynamicObject();
        const auto a0 = t.at.count ("0.000") ? t.at.at ("0.000") : juce::String();
        const auto a5 = t.at.count ("0.500") ? t.at.at ("0.500") : juce::String();
        const auto a1 = t.at.count ("1.000") ? t.at.at ("1.000") : juce::String();
        c->setProperty ("index", t.index);
        c->setProperty ("name", t.name);
        const auto unit = fixtureunit::unitFor (t.label, a0, a1);
        c->setProperty ("unit", unit.isEmpty() ? juce::var() : juce::var (unit));
        auto* at = new juce::DynamicObject();
        at->setProperty ("0.000", a0); at->setProperty ("0.500", a5); at->setProperty ("1.000", a1);
        c->setProperty ("displayAt", juce::var (at));
        auto lr = list.find (t.index);
        c->setProperty ("numSteps", lr != list.end() ? lr->second.numSteps : 0);
        c->setProperty ("discrete", lr != list.end() && lr->second.discrete);
        const auto dr = fixturerange::derive (a0, a5, a1, 0.01, t.defText, t.defNorm);   // the instantiate point folds in (ruled 4 Oct)
        c->setProperty ("range", dr.range);
        c->setProperty ("direction", dr.direction);
        auto* d = new juce::DynamicObject();
        d->setProperty ("normalised", t.defNorm);
        d->setProperty ("display", t.defText);
        d->setProperty ("declaredDefault", t.declared);
        d->setProperty ("note", kDefaultsNote);
        c->setProperty ("defaultOnInstantiate", juce::var (d));
        controls.add (juce::var (c));
    }
    o->setProperty ("controls", controls);
    o->setProperty ("defaultsSampledAt", date);
    o->setProperty ("defaultsProbe", probeLabel);
    return juce::var (o);
}

//==============================================================================
// FIELD-BY-FIELD COMPARISON. Numbers compare as doubles (JSON has one number type);
// everything else by kind and value. Returns one line per differing path.
inline void diffVar (const juce::var& a, const juce::var& b, const juce::String& path, juce::StringArray& out)
{
    auto isNum = [] (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); };
    if (isNum (a) && isNum (b)) { if ((double) a != (double) b) out.add (path + ": " + a.toString() + " vs " + b.toString()); return; }
    if (a.isVoid() || b.isVoid()) { if (! (a.isVoid() && b.isVoid())) out.add (path + ": " + (a.isVoid() ? "null" : a.toString())
                                                                             + " vs " + (b.isVoid() ? "null" : b.toString())); return; }
    if (a.isBool() || b.isBool()) { if (! (a.isBool() && b.isBool() && (bool) a == (bool) b)) out.add (path + ": " + a.toString() + " vs " + b.toString()); return; }
    if (a.isString() || b.isString()) { if (! (a.isString() && b.isString() && a.toString() == b.toString()))
                                            out.add (path + ": \"" + a.toString() + "\" vs \"" + b.toString() + "\""); return; }
    if (auto* aa = a.getArray())
    {
        auto* ba = b.getArray();
        if (ba == nullptr) { out.add (path + ": array vs non-array"); return; }
        if (aa->size() != ba->size()) out.add (path + ": " + juce::String (aa->size()) + " vs " + juce::String (ba->size()) + " entries");
        for (int i = 0; i < juce::jmin (aa->size(), ba->size()); ++i) diffVar ((*aa)[i], (*ba)[i], path + "[" + juce::String (i) + "]", out);
        return;
    }
    auto* ao = a.getDynamicObject(); auto* bo = b.getDynamicObject();
    if (ao == nullptr || bo == nullptr) { if (a.toString() != b.toString()) out.add (path + ": differ"); return; }
    for (auto& kv : ao->getProperties())
        if (! bo->hasProperty (kv.name)) out.add (path + "." + kv.name.toString() + ": only in the regenerated fixture");
        else diffVar (kv.value, bo->getProperty (kv.name), path + "." + kv.name.toString(), out);
    for (auto& kv : bo->getProperties())
        if (! ao->hasProperty (kv.name)) out.add (path + "." + kv.name.toString() + ": only in the pushed fixture");
}

inline juce::StringArray compareFixtures (const juce::var& regenerated, const juce::var& pushed)
{
    static const juce::StringArray provenance { "sampledAt", "probe", "defaultsSampledAt", "defaultsProbe" };
    // A deep copy, NOT a JSON round trip: re-printing a double could move it and
    // manufacture a difference that no measurement made.
    auto strip = [] (const juce::var& v) {
        auto copy = v.clone();
        if (auto* o = copy.getDynamicObject()) for (const auto& k : provenance) o->removeProperty (k);
        return copy; };
    juce::StringArray out;
    diffVar (strip (regenerated), strip (pushed), "", out);
    return out;
}

//==============================================================================
// MODE 1: --cert-rederive. Every control of every fixture, re-derived from its own
// displayAt. range and direction must match exactly. unit can only be checked one
// way: a control whose fixture has NO unit must derive none from its text (a label
// can supply a unit, never remove one). The rest are counted, not claimed.
inline int runRederive (const juce::File& dir)
{
    auto subjects = loadFixtures (dir);
    int controls = 0, rangeBad = 0, dirBad = 0, unitContradicts = 0, unitFromText = 0, unitNeedsLabel = 0, unitNone = 0;
    juce::StringArray bad;
    for (const auto& s : subjects)
        if (auto* arr = s.pushed.getProperty ("controls", juce::var()).getArray())
            for (const auto& c : *arr)
            {
                ++controls;
                const auto da = c.getProperty ("displayAt", juce::var());
                const auto a0 = da.getProperty ("0.000", "").toString(), a5 = da.getProperty ("0.500", "").toString(),
                           a1 = da.getProperty ("1.000", "").toString();
                const auto di = c.getProperty ("defaultOnInstantiate", juce::var());
                const auto d = fixturerange::derive (a0, a5, a1, 0.01, di.getProperty ("display", "").toString(), (double) di.getProperty ("normalised", -1.0));
                juce::StringArray rd; diffVar (d.range, c.getProperty ("range", juce::var()), "range", rd);
                if (! rd.isEmpty()) { ++rangeBad; bad.add (s.product + " / " + c.getProperty ("name", "").toString() + ": " + rd.joinIntoString ("; ")); }
                if (d.direction != c.getProperty ("direction", "").toString())
                { ++dirBad; bad.add (s.product + " / " + c.getProperty ("name", "").toString() + ": direction " + d.direction
                                     + " vs " + c.getProperty ("direction", "").toString()); }
                const auto fromText = fixtureunit::unitFor ({}, a0, a1);
                const auto u = c.getProperty ("unit", juce::var());
                if (u.isVoid())                    { ++unitNone; if (fromText.isNotEmpty()) { ++unitContradicts;
                                                        bad.add (s.product + " / " + c.getProperty ("name", "").toString()
                                                                 + ": text gives unit '" + fromText + "' but the fixture has none"); } }
                else if (u.toString() == fromText) ++unitFromText;
                else                               ++unitNeedsLabel;
            }
    std::cout << "CERT REDERIVE: " << subjects.size() << " fixtures, " << controls << " controls, from each fixture's own displayAt\n"
              << "  range      " << (controls - rangeBad) << " / " << controls << " reproduced\n"
              << "  direction  " << (controls - dirBad) << " / " << controls << " reproduced\n"
              << "  unit       " << unitNone << " with no unit, of which " << unitContradicts << " contradicted by the text;\n"
              << "             " << unitFromText << " explained by the text, " << unitNeedsLabel
              << " that must come from a parameter label (not checkable without the plugin)\n";
    for (const auto& b : bad) std::cout << "  DIFF " << b << "\n";
    std::cout << (rangeBad + dirBad + unitContradicts == 0 ? "CERT REDERIVE: GREEN" : "CERT REDERIVE: RED") << std::endl;
    return rangeBad + dirBad + unitContradicts == 0 ? 0 : 1;
}

//==============================================================================
// MODE 2: --cert-defaults. Regenerate every reachable fixture and compare it.
struct Options
{
    juce::File fixtures, probe, out, entitlements;
    juce::String signIdentity;
    int timeoutMs = 90000;
    bool includePace = false;
};

inline int runCertDefaults (const Options& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    if (! opt.fixtures.isDirectory()) { say ("CERT: no fixtures directory at " + opt.fixtures.getFullPathName()); return 2; }
    if (! opt.out.createDirectory()) { say ("CERT: cannot create " + opt.out.getFullPathName()); return 2; }

    const SleepGuard sleepGuard ("EJ Map certification defaults");
    std::cout << sleepGuard.describe() << std::endl;
    // 1. THE SIGNATURE, once, before anything is touched.
    const auto id = checkProbe (opt.probe, opt.signIdentity, opt.entitlements);
    if (! id.ok) { say ("CERT: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const juce::String probeLabel = "signed EchoJayProbe (feat/ejmap-cert), team " + id.team + ", cdhash " + id.cdhash;
    const juce::String date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");

    // 2. THE WORKLIST and its reachability.
    auto subjects = loadFixtures (opt.fixtures);
    classify (subjects, opt.includePace);

    auto ledger = opt.out.getChildFile ("run.jsonl");
    ledger.deleteFile();
    auto logAttempt = [&] (const Subject& s, const juce::String& mode, int attempt, const ChildResult& r)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("product", s.product); o->setProperty ("fixture", s.fixtureFile.getFileName());
        o->setProperty ("mode", mode); o->setProperty ("attempt", attempt);
        o->setProperty ("outcome", r.describe()); o->setProperty ("ms", r.ms);
        o->setProperty ("slept_ms", std::round (r.sleptMs));
        ledger.appendText (juce::JSON::toString (juce::var (o), true) + "\n");
    };
    // THE RETRY RULE: anything but a clean exit is re-run ONCE, alone. The second
    // result stands. Two timeouts mark a hang.
    auto runProbe = [&] (const Subject& s, const juce::String& mode, const juce::StringArray& extra, bool& retried)
    {
        juce::StringArray args { opt.probe.getFullPathName(), s.desc.name, s.desc.fileOrIdentifier,
                                 juce::String::toHexString (s.desc.uniqueId) };
        args.addArray (extra);
        // The probe's raw output is kept for every attempt, so a difference can be
        // traced to what the plugin printed without re-running it.
        auto keep = [&] (int attempt, const ChildResult& r) {
            opt.out.getChildFile ("raw").getChildFile (s.fixtureFile.getFileNameWithoutExtension() + "."
                + mode.retainCharacters ("abcdefghijklmnopqrstuvwxyz-") + "." + juce::String (attempt) + ".txt")
               .replaceWithText (r.out); };
        opt.out.getChildFile ("raw").createDirectory();
        // Not retried: a refusal (exit 3) is the plugin's answer, and a window is a
        // licence or activation event - a retry would only put it on the desktop again.
        // Re-run once: anything else unclean, or a clean run the Mac slept through.
        const auto a = runWithRetry (args, opt.timeoutMs, [&] (int attempt, const ChildResult& r) { logAttempt (s, mode, attempt, r); keep (attempt, r); });
        if (a.attempts == 2) retried = true;
        auto r = a.r;
        if (a.sleptTwice) r.kind = ChildResult::Kind::sleptTwice;
        return r;
    };

    // READOUT DETECTION (schema change, item 12: shape AGREED 28 Sep with a null default -
    // a meter has no instantiate value, so null is the truthful encoding - but NOT yet
    // emitted by composeFixture; that is its own change).
    // A control whose value differs between two FRESH instances is a readout, and its
    // defaultOnInstantiate is meaningless: one sample of something moving. Found on
    // Shadow Hills Class A's "VU Meter R" (five instances, five values). It is a
    // derivation from two measurements, so it lives here and the probe is unchanged.
    // Sufficient, not necessary: a meter that happens to read the same twice (a gain
    // reduction meter sitting at 0 in silence) is NOT caught.
    using Readout = ReadoutHit;
    struct Row { const Subject* s; juce::String outcome; juce::StringArray diffs, diffsOnReadouts, emissionFails;
                 bool reproduced = false, retried = false, readoutChecked = false, emitted = false, unlicensed = false;
                 int captured = 0, onList = 0; std::vector<Readout> readouts; juce::String readoutNote; };
    std::vector<Row> rows;
    for (const auto& s : subjects)
    {
        if (s.reach != Subject::Reach::reachable && s.reach != Subject::Reach::reachableNoVersion) continue;
        Row row { &s };
        std::cout << "  probing " << s.product << " ..." << std::flush;
        auto lp = runProbe (s, "--list-params", { "--list-params" }, row.retried);
        auto ta = lp.cleanExit() ? runProbe (s, "--text-at all", { "--text-at", "all" }, row.retried) : ChildResult();
        // UNLICENSED IS NOT BROKEN (doc section 3, spec section 6). A licence-bound product
        // that refuses, hangs or shows a window is recorded as unlicensed_on_host, with the
        // shape it failed in. It is not a pass and not a defect, and it is counted apart.
        // Licence-bound by BUNDLE, or by BEHAVIOUR: PACE's UI appearing in the probe's
        // tree outranks a bundle scan that found nothing (kHs Compressor, 29 Sep).
        auto failedWith = [&] (const juce::String& f, const ChildResult& r) {
            const bool byBehaviour = ! s.licenceBound && certoutcome::isPaceUi (r.windowsInTree);
            row.unlicensed = certoutcome::classifyFailure (s.licenceBound, r.windowsInTree)
                               == certoutcome::Failure::unlicensedOnHost;
            if (! row.unlicensed) return "NOT REPRODUCED: " + f;
            return juce::String (byBehaviour ? "UNLICENSED ON HOST (licence-bound BY BEHAVIOUR: PACE UI appeared, "
                                                 "though its bundle carries no PACE markers; "
                                             : "UNLICENSED ON HOST (licence-bound; ") + f + ")"; };
        if (! lp.cleanExit()) row.outcome = failedWith ("--list-params " + lp.describe(), lp);
        else if (! ta.cleanExit()) row.outcome = failedWith ("--text-at all " + ta.describe(), ta);
        else
        {
            const auto list = parseListParams (lp.out);
            const auto text = parseTextAt (ta.out);
            for (const auto& kv : list) if (kv.second.automatable && ! kv.second.meta) ++row.onList;
            row.captured = (int) text.size();

            // THE SECOND INSTANCE. One extra instantiation per product.
            auto tb = runProbe (s, "--text-at all (second instance)", { "--text-at", "all" }, row.retried);
            const auto scan = scanReadouts (text, tb);
            row.readoutChecked = scan.checked;
            if (scan.note.isNotEmpty()) row.readoutNote = scan.note;
            row.readouts = scan.hits;
            const auto& readoutPositions = scan.positions;

            // TWO FORMS, TWO QUESTIONS. `regen` is the MEASURED form, composed exactly as
            // before item 12; it is what the reproduction score compares, so that score stays
            // comparable to the 14 of 15 measured before the schema change and never contains
            // the new fields. `emittedFixture` is item 12's shape; it is what gets written, and
            // it is checked separately by checkEmission. Never merge the two.
            const auto regen = composeFixture (s, list, text, lp.code, ta.code, probeLabel, date);
            std::vector<fixturereadout::Moved> moved;
            for (const auto& ro : row.readouts) moved.push_back ({ ro.index, ro.a, ro.b, ro.ta, ro.tb });
            const auto emittedFixture = fixturereadout::applySchema (regen, moved, row.readoutChecked);
            opt.out.getChildFile (s.fixtureFile.getFileName()).replaceWithText (juce::JSON::toString (emittedFixture, false));
            row.emissionFails = fixturereadout::checkEmission (emittedFixture, regen, moved, row.readoutChecked);
            row.emitted = row.readoutChecked && row.emissionFails.isEmpty();
            for (const auto& d : compareFixtures (regen, s.pushed))
            {
                // A difference in a DETECTED readout's defaultOnInstantiate is the known,
                // recorded condition, reported apart from real differences.
                const int pos = d.startsWith (".controls[") ? d.fromFirstOccurrenceOf ("[", false, false).getIntValue() : -1;
                const bool onReadoutDefault = pos >= 0 && readoutPositions.count (pos) > 0
                                              && d.contains ("].defaultOnInstantiate.");
                (onReadoutDefault ? row.diffsOnReadouts : row.diffs).add (d);
            }
            row.reproduced = row.diffs.isEmpty() && row.diffsOnReadouts.isEmpty();
            row.outcome = row.reproduced ? "REPRODUCED"
                        : row.diffs.isEmpty() ? "DIFFERS ONLY on " + juce::String (row.diffsOnReadouts.size())
                                                + " detected-readout default(s)"
                                              : "DIFFERS in " + juce::String (row.diffs.size()) + " field(s)";
        }
        std::cout << " " << row.outcome << std::endl;
        rows.push_back (row);
    }

    // The readouts, in a SIDECAR. Not written into any fixture until the schema field
    // is agreed (it is item 12 on the mismatch list).
    {
        juce::Array<juce::var> side;
        for (const auto& r : rows)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("fixture", r.s->fixtureFile.getFileName());
            o->setProperty ("product", r.s->product);
            o->setProperty ("readoutCheck", r.readoutChecked ? juce::var ("instantiate_twice") : juce::var());
            if (r.readoutNote.isNotEmpty()) o->setProperty ("note", r.readoutNote);
            juce::Array<juce::var> list;
            for (const auto& ro : r.readouts)
            {
                auto* x = new juce::DynamicObject();
                x->setProperty ("index", ro.index); x->setProperty ("name", ro.name);
                x->setProperty ("samples", juce::Array<juce::var> { ro.a, ro.b });
                x->setProperty ("displays", juce::Array<juce::var> { ro.ta, ro.tb });
                list.add (juce::var (x));
            }
            o->setProperty ("readouts", list);
            side.add (juce::var (o));
        }
        opt.out.getChildFile ("readouts.json").replaceWithText (juce::JSON::toString (juce::var (side), false));
    }

    // 3. THE REPORT. Coverage first, always.
    std::map<Subject::Reach, juce::StringArray> byReach;
    for (const auto& s : subjects) byReach[s.reach].add (s.product + (s.detail.isNotEmpty() ? "  (" + s.detail + ")" : ""));
    const int reachable = (int) rows.size();
    int reproduced = 0, differs = 0, onlyReadouts = 0, failed = 0, unlicensed = 0, readoutControls = 0, readoutChecked = 0, emitted = 0;
    for (const auto& r : rows)
    {
        if (r.reproduced) ++reproduced;
        else if (! r.diffs.isEmpty()) ++differs;
        else if (! r.diffsOnReadouts.isEmpty()) ++onlyReadouts;
        else if (r.unlicensed) ++unlicensed;
        else ++failed;
        readoutControls += (int) r.readouts.size();
        if (r.readoutChecked) ++readoutChecked;
        if (r.emitted) ++emitted;
    }

    juce::String rep;
    rep << "EJ MAP CERT DRIVER - regenerate compressor-profile fixtures - " << juce::Time::getCurrentTime().toISO8601 (true) << "\n"
        << "probe     " << probeLabel << "\n"
        << "fixtures  " << opt.fixtures.getFullPathName() << "\n\n"
        << "COVERAGE (read this first)\n"
        << "  " << juce::String ("fixtures").paddedRight (' ', 58) << (int) subjects.size() << "\n";
    for (auto r : { Subject::Reach::reachable, Subject::Reach::reachableNoVersion, Subject::Reach::heldPace,
                    Subject::Reach::requiresHardware, Subject::Reach::versionMismatch, Subject::Reach::notInstalled,
                    Subject::Reach::ambiguous })
        rep << "  " << reachName (r).paddedRight (' ', 58) << byReach[r].size() << "\n";
    // TWO LINES, NEVER ONE: they answer different questions.
    rep << "\nREPRODUCTION (the MEASURED form vs the pushed fixture; item-12 fields are not in it): reproduced "
        << reproduced << " of " << reachable << " reachable, of " << (int) subjects.size() << " fixtures"
        << "  |  differ only on detected readouts " << onlyReadouts << "  |  differ " << differs
        << "  |  unlicensed_on_host " << unlicensed << "  |  not reproduced (named) " << failed << "\n"
        << "SCHEMA EMISSION (item 12, checked separately, C1-C5): emitted the readout fields correctly on "
        << emitted << " of " << reachable << " reachable\n"
        << "READOUTS: checked on " << readoutChecked << " of " << reachable << " (two fresh instances each); "
        << readoutControls << " control(s) moved between instances (also in readouts.json)\n\n"
        << "PER REACHABLE PRODUCT (reproduction | emission)\n";
    for (const auto& r : rows)
    {
        rep << "  " << r.s->product.paddedRight (' ', 38) << r.outcome
            << "  |  emission " << (r.emitted ? juce::String ("OK")
                                              : r.readoutChecked ? "FAILED" : "not checked: " + r.readoutNote)
            << (r.onList > 0 ? "   controls " + juce::String (r.captured) + " / " + juce::String (r.onList) : juce::String())
            << (r.retried ? "   RETRIED" : "") << "\n";
        for (const auto& f : r.emissionFails) rep << "      EMISSION " << f << "\n";
        for (const auto& d : r.diffs) rep << "      " << d << "\n";
        for (const auto& d : r.diffsOnReadouts) rep << "      " << d << "   (on a detected readout)\n";
        for (const auto& ro : r.readouts)
            rep << "      readout [" << ro.index << "] " << ro.name << ": " << juce::String (ro.a, 6) << " then "
                << juce::String (ro.b, 6) << " on instantiate\n";
        if (r.readoutNote.isNotEmpty()) rep << "      " << r.readoutNote << "\n";
    }
    rep << "\nOUT OF REACH OR HELD, BY NAME\n";
    for (auto r : { Subject::Reach::heldPace, Subject::Reach::requiresHardware, Subject::Reach::versionMismatch,
                    Subject::Reach::notInstalled, Subject::Reach::ambiguous })
        if (! byReach[r].isEmpty())
        {
            rep << "  " << reachName (r) << " (" << byReach[r].size() << ")\n";
            for (const auto& n : byReach[r]) rep << "      " << n << "\n";
        }
    rep << "\nThe acceptance criterion is docs/EJMAP_CERT_DRIVER.md section 0. A product that did not answer counts as NOT\n"
           "reproduced and is never a pass. Every attempt is in run.jsonl beside this report.\n";
    opt.out.getChildFile ("report.txt").replaceWithText (rep);
    std::cout << "\n" << rep << std::endl;
    return (differs == 0 && onlyReadouts == 0 && failed == 0 && emitted == reachable) ? 0 : 1;
}

//==============================================================================
// DISCOVERY (ruled 29 Sep). THE DRIVER USED TO READ ITS WORKLIST FROM ITS OWN OUTPUT: every mode took its subjects from
// a directory of fixtures, so a product nobody had certified could never enter it - on a fresh Mac, nothing ever would.
// A fixture is the RECORD of what is certified, not the list of what to certify; a hand-written stub fixture unblocked
// everything, which is the proof. The worklist is now keyed on MAPS, as the coverage state machine already says:
//
//   a CANDIDATE is installed, MAPPED at its installed build, in the compressor category, with no fixture at its
//   installed identity and version.
//
//   MAPPED means EJ Map's ledger says so: a local map for that identity (maps/*.json), or the server's map state for it
//   (map-state.json: 1 local only, 2 submitted by this machine, 3 submitted by another). The second matters most: on a
//   fresh Mac almost everything is mapped by SOMEONE ELSE and has no local map at all (elysia mpressor, 29 Sep). A map
//   for a DIFFERENT build (4) is not mapped here.
//   THE CATEGORY is the local map's own, else categories.json's for that identity (indexed by format|uid, its
//   mark_keys). Both come from the server's categorisation, a documented PREREQUISITE (docs section 10): on a fresh Mac
//   the categories and the map state arrive in the same connected EJ Map run, so this one round-trip opens both stops.
//   "pitch" (tuners) is listed, not swept: tuner certification (section 5) is not built.
//
// Everything here reads the ledger; nothing writes it.
struct DiscoveryInputs
{
    std::map<juce::String, int> mapState;                   // identity key -> MapState (0 unmapped .. 5 unknown)
    std::map<juce::String, juce::String> localMapCategory;  // identity key -> category, from a local map
    std::map<juce::String, juce::String> categoryByUid;     // "AudioUnit|uid" -> category, from categories.json
    // "AudioUnit|uid" -> disposition, from categories.json, when it is anything but "sweep" - and its `why`. THE MAPPER'S
    // ESCAPE HATCH REACHES CERT (ruled 30 Sep): the runbook excludes a plugin that hangs by writing operator_excluded here,
    // and the certification batch must not open what the mapping sweep was told to leave alone.
    std::map<juce::String, juce::String> dispositionByUid;
    juce::StringArray notes;                                // what was read, for the report
};

// ONLY AN EXCLUSION STOPS CERTIFICATION (re-ruled 2 Oct; the 30 Sep ruling was too broad). operator_excluded and the
// hang / crash family are the mapper's escape hatch and are honoured. A MAPPING verdict - no_dial_set, review,
// not_a_processor - says nothing about whether a product can be measured: the CATEGORY decides (compressor, pitch).
// Measured on the fresh-ledger test: 202 products held by disposition against 7 before, every tuner among them.
// THE PRINCIPLE (ruled 2 Oct, afternoon): certification may be gated ONLY by facts about the plugin (installed,
// category, the operator's exclusion) and by evidence observed at load (a licence window, a hang, a crash) - NEVER by a
// verdict the mapping workflow produced for its own purposes. A disposition is a verdict; the operator's exclusion is
// a fact; load evidence lives in the ledger (quarantine, licence stops), not in a disposition word.
inline bool exclusionDisposition (const juce::String& disp)
{
    return disp.trim().toLowerCase() == "operator_excluded";
}

inline DiscoveryInputs loadDiscoveryInputs (const juce::File& ledgerRoot)
{
    DiscoveryInputs in;
    if (! ledgerRoot.isDirectory()) { in.notes.add ("no EJ Map ledger at " + ledgerRoot.getFullPathName()); return in; }
    int maps = 0;
    for (const auto& f : ledgerRoot.getChildFile ("maps").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto m = juce::JSON::parse (f.loadFileAsString());
        const auto id = m.getProperty ("identity", {});
        if (! id.isObject()) continue;
        const auto key = id.getProperty ("format", "").toString() + "|" + id.getProperty ("uid", "").toString().toLowerCase()
                         + "|" + id.getProperty ("version", "").toString();
        in.localMapCategory[key] = m.getProperty ("category", "").toString();
        ++maps;
    }
    const auto ms = juce::JSON::parse (ledgerRoot.getChildFile ("map-state.json").loadFileAsString());
    if (auto* ids = ms.getProperty ("identities", {}).getDynamicObject())
        for (const auto& p : ids->getProperties())
            in.mapState[p.name.toString()] = (int) p.value.getProperty ("state", 5);
    const auto cats = juce::JSON::parse (ledgerRoot.getChildFile ("categories.json").loadFileAsString());
    if (auto* prods = cats.getProperty ("products", {}).getDynamicObject())
        for (const auto& p : prods->getProperties())
            if (const auto* mk = p.value.getProperty ("mark_keys", {}).getArray())
                for (const auto& k : *mk)
                {
                    const auto uidKey = k.toString().toLowerCase().replace ("audiounit|", "AudioUnit|").replace ("vst3|", "VST3|");
                    in.categoryByUid[uidKey] = p.value.getProperty ("category", "").toString();
                    const auto disp = p.value.getProperty ("disposition", "").toString().trim();
                    if (exclusionDisposition (disp))
                        in.dispositionByUid[uidKey] = disp + (p.value.hasProperty ("why") ? " (" + p.value.getProperty ("why", "").toString() + ")" : juce::String());
                }
    in.notes.add (juce::String (maps) + " local map(s), " + juce::String ((int) in.mapState.size()) + " map-state row(s)"
                  + (ms.getProperty ("fetched_at", "").toString().isNotEmpty() ? " (fetched " + ms.getProperty ("fetched_at", "").toString() + ")" : juce::String (" (never fetched)"))
                  + ", " + juce::String ((int) in.categoryByUid.size()) + " categorised identities"
                  + (in.dispositionByUid.empty() ? juce::String() : ", " + juce::String ((int) in.dispositionByUid.size()) + " with an exclusion disposition (operator_excluded / hang / crash: honoured; mapping verdicts are not)"));
    return in;
}

struct InstalledRecord { juce::PluginDescription desc; juce::String identityKey, uidKey; };

inline std::vector<InstalledRecord> installedAudioUnits()
{
    std::vector<InstalledRecord> out;
    for (const auto& t : echojay::auregistry::buildCensus().targets)
    {
        auto d = echojay::auregistry::describeFromRegistry (t.identifier);
        if (d.fileOrIdentifier.isEmpty()) continue;
        out.push_back ({ d, echojay::identityKeyForDescription (d), "AudioUnit|" + juce::String::toHexString (d.uniqueId).toLowerCase() });
    }
    return out;
}

// QUARANTINED AT SCAN (ruled 2 Oct): the ledger's quarantine, each bundle resolved to the AudioUnits it registers and
// their category (loop::quarantinedAtScan), so the census and the batch can name what never reached them.
inline std::vector<loop::QuarantinedBundle> quarantinedBundles (const juce::File& ledgerRoot)
{
    // the quarantine and the licence stops, one list: both are bundles the census never sees
    auto q = juce::JSON::parse (ledgerRoot.getChildFile ("quarantine.json").loadFileAsString());
    juce::Array<juce::var> both; if (const auto* a = q.getArray()) both = *a;
    const auto ls = juce::JSON::parse (ledgerRoot.getChildFile ("licence-stops.json").loadFileAsString());
    if (const auto* a = ls.getArray()) for (const auto& e : *a) both.add (e);
    q = juce::var (both);
    const auto cats = juce::JSON::parse (ledgerRoot.getChildFile ("categories.json").loadFileAsString());
    std::map<juce::String, juce::StringArray> auNamesByBundle; std::map<juce::String, juce::String> uidByName;
    const auto bundles = componentBundles();
    for (const auto& r : installedAudioUnits())
    {
        // "AudioUnit:Effects/aufx,clST,SfTb" -> "aufx,clST,SfTb" -> the bundle that registers it
        const auto key = r.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false);
        if (auto it = bundles.find (key); it != bundles.end()) auNamesByBundle[it->second.getFullPathName()].addIfNotAlreadyThere (r.desc.name);
        uidByName[r.desc.name] = juce::String::toHexString (r.desc.uniqueId).toLowerCase();
    }
    return loop::quarantinedAtScan (q, cats, auNamesByBundle, uidByName);
}

// THE PURE CORE: which installed products are candidates, and why the rest are not. `fixtureKeys` holds "uid|version"
// (lowercase uid) for every fixture in the store.
struct Candidate { InstalledRecord inst; juce::String category, mappedBy; };
// NO MAP YET (ruled 2 Oct, afternoon): an installed compressor or tuner with no map at its installed build is
// DISCOVERED like any other (certification does not need a map); the list is INFORMATION for the census - the mapping
// sweep can still map them, and the server joins by map_fp whenever it does.
struct UnmappedProduct { juce::String name, version, category, uidKey; };
struct Discovery { std::vector<Candidate> candidates; std::map<juce::String, int> excluded; juce::StringArray tuners, excludedByDisposition; std::vector<UnmappedProduct> unmapped; int noMapYet = 0; };

inline Discovery discoverCandidates (const DiscoveryInputs& in, const std::vector<InstalledRecord>& installed,
                                     const std::set<juce::String>& fixtureKeys)
{
    Discovery d;
    for (const auto& r : installed)
    {
        // CERTIFICATION DOES NOT NEED A MAP (ruled 2 Oct, reversing 29 Sep): the join is by map_fp, which the record
        // computes exactly as EchoJay does, and it can happen whenever the map arrives. Discovery = INSTALLED + CATEGORY
        // (compressor | pitch) + NOT EXCLUDED. The map state is INFORMATION on the record: local, server, server at a
        // different build, or none. Certification no longer waits on mapping (the coverage state machine changed here).
        const auto local = in.localMapCategory.find (r.identityKey);
        const auto st = in.mapState.find (r.identityKey);
        const bool serverMapped = st != in.mapState.end() && st->second >= 1 && st->second <= 3;
        const juce::String mappedBy = local != in.localMapCategory.end() ? juce::String ("local map")
                                    : serverMapped ? "server map state " + juce::String (st->second)
                                    : (st != in.mapState.end() && st->second == 4) ? juce::String ("server map at a different build")
                                    : juce::String ("none");
        if (mappedBy == "none") ++d.noMapYet;
        juce::String category;
        if (auto c = in.categoryByUid.find (r.uidKey); c != in.categoryByUid.end()) category = c->second;   // the catalogue's category is the fact
        if (category.isEmpty() && local != in.localMapCategory.end()) category = local->second;             // a local map's, when the catalogue has none
        if (mappedBy == "none" && (category == "compressor" || category == "pitch")) d.unmapped.push_back ({ r.desc.name, r.desc.version, category, r.uidKey });
        const auto fxKey = juce::String::toHexString (r.desc.uniqueId).toLowerCase() + "|" + r.desc.version;
        if (fixtureKeys.count (fxKey)) { ++d.excluded["fixture present at this version"]; continue; }
        if (auto disp = in.dispositionByUid.find (r.uidKey); disp != in.dispositionByUid.end())
        { ++d.excluded["disposition " + disp->second.upToFirstOccurrenceOf (" (", false, false) + " in categories.json"]; d.excludedByDisposition.add (r.desc.name + ": " + disp->second); continue; }
        // TUNERS ARE CANDIDATES (ruled 1 Oct, one store): category pitch gets the tuner certification, in the same worklist.
        if (category == "pitch") d.tuners.add (r.desc.name);
        else if (category != "compressor") { ++d.excluded[category.isEmpty() ? juce::String ("no category") : "category " + category]; continue; }
        d.candidates.push_back ({ r, category, mappedBy });
    }
    return d;
}

// THE WORKLIST: the fixture store's subjects (what has a record), then every discovered candidate (what has only a map),
// in COVERAGE ORDER - a record at the installed version that has not been swept first (the sweep alone is missing),
// then those that need defaults first (another version's record, or none). A fixture that already records a sweep is
// certified and leaves the list.
// THE STORE SIDE OF THE WORKLIST, pure: a fixture that records a sweep, every candidate's sweep, or a refusal stays out;
// the rest go on. `retryRefused` puts the refusals back (the iLok is in now, the hardware is attached): the record
// stays in the fixture until the re-run replaces it.
struct StorePartition { std::vector<Subject> toSweep; int recorded = 0, refused = 0, permanent = 0; };
inline bool refusalAtWindow (const juce::var& fixture) { return refusalRecorded (fixture) && fixture.getProperty ("thresholdRefusal", {}).getProperty ("stage", "").toString() == "window"; }
// INERT = LICENCE (ruled 5 Oct): a record whose sweep (or every candidate's) is inert is a licence row, and --retry-licence
// re-sweeps it like a window refusal - the one way back once the product is activated
inline bool inertRecorded (const juce::var& fixture)
{
    if (fixture.getProperty ("thresholdSweep", {}).getProperty ("result", "").toString() == "inert") return true;
    const auto* cs = fixture.getProperty ("thresholdCandidates", {}).getArray();
    if (cs == nullptr || cs->isEmpty()) return false;
    for (const auto& c : *cs) if (c.getProperty ("thresholdSweep", {}).getProperty ("result", "").toString() != "inert") return false;
    return true;
}
inline StorePartition partitionStore (const std::vector<Subject>& fromStore, bool retryRefused, bool retryAll = false, bool licenceOnly = false, const juce::StringArray* force = nullptr)
{
    StorePartition p;
    for (const auto& s : fromStore)
    {
        const bool refusal = refusalRecorded (s.pushed), permanent = refusalPermanent (s.pushed);
        if (refusal) ++p.refused;
        if (permanent) ++p.permanent;
        // --retry-licence (ruled 2 Oct): only the refusals a licence window caused come back
        // THE FOLLOW-UP'S OWN RE-SWEEP SET (ruled 4 Oct): a product whose plan under this build differs from the plan it was
        // swept under is forced back onto the worklist, record or refusal alike (EjmapLoop.h planDiffers)
        const bool retry = (retryRefused && refusal && (licenceOnly ? refusalAtWindow (s.pushed) : (retryAll || ! permanent)))
                        || (retryRefused && licenceOnly && inertRecorded (s.pushed))                                    // inert = licence (5 Oct)
                        || (force != nullptr && force->contains (s.product));
        if (sweepRecorded (s.pushed) && ! retry) ++p.recorded;
        else p.toSweep.push_back (s);
    }
    return p;
}

inline std::vector<Subject> buildWorklist (const juce::File& fixturesDir, const juce::File& ledgerRoot, bool includePace,
                                           juce::StringArray& report, bool retryRefused = false, bool retryAll = false,
                                           std::vector<UnmappedProduct>* unmappedOut = nullptr, bool licenceOnly = false, const juce::StringArray* force = nullptr)
{
    std::vector<Subject> fromStore;
    if (fixturesDir.isDirectory()) fromStore = loadFixtures (fixturesDir);
    classify (fromStore, includePace);
    std::set<juce::String> fixtureKeys, storeUids;
    for (const auto& s : fromStore)
    {
        fixtureKeys.insert (s.uid + "|" + s.version);
        storeUids.insert (s.uid);
    }
    const auto part = partitionStore (fromStore, retryRefused, retryAll, licenceOnly, force);
    std::vector<Subject> out = part.toSweep;
    const int certified = part.recorded;

    const auto in = loadDiscoveryInputs (ledgerRoot);
    const auto installed = installedAudioUnits();
    const auto disc = discoverCandidates (in, installed, fixtureKeys);
    const auto bundles = componentBundles();
    int added = 0;
    for (const auto& c : disc.candidates)
    {
        const auto uid = juce::String::toHexString (c.inst.desc.uniqueId).toLowerCase();
        if (storeUids.count (uid)) continue;                 // the store's own subject for this product already covers it
        Subject s;
        s.product = c.inst.desc.name; s.uid = uid; s.version = c.inst.desc.version;
        auto* o = new juce::DynamicObject();
        o->setProperty ("product", s.product); o->setProperty ("uid", uid); o->setProperty ("version", s.version);
        o->setProperty ("format", "AudioUnit");
        o->setProperty ("discovered", "map: " + c.mappedBy + ", category " + c.category + ", no fixture: defaults are sampled first");
        o->setProperty ("category", c.category);
        s.pushed = juce::var (o);
        s.category = c.category == "pitch" ? "pitch" : "compressor";
        s.desc = c.inst.desc;
        s.reach = Subject::Reach::unfixtured;
        s.installedUnique = true;
        s.hardware = requiresExternalHardware (s.product, c.inst.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false));
        juce::String why;
        s.licenceBound = paceHeld (c.inst.desc, bundles, why);
        s.mapState = c.mappedBy;
        s.detail = "map: " + c.mappedBy + (s.hardware ? "; needs " + externalHardwareNeeded (s.product, c.inst.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)) + " present" : juce::String());
        out.push_back (s);
        ++added;
    }
    // COVERAGE ORDER: the sweep alone is missing, then defaults-then-sweep; by name within each.
    auto rank = [] (const Subject& s) { return (s.reach == Subject::Reach::versionMismatch || s.reach == Subject::Reach::unfixtured) ? 1 : 0; };
    std::stable_sort (out.begin(), out.end(), [&] (const Subject& a, const Subject& b) {
        return rank (a) != rank (b) ? rank (a) < rank (b) : a.product.compareIgnoreCase (b.product) < 0; });

    report.add ("worklist: " + juce::String ((int) fromStore.size()) + " fixture(s) in the store (" + juce::String (certified)
                + " already record a sweep, candidates or a refusal; " + juce::String (part.refused) + " refusal(s), "
                + juce::String (part.permanent) + " of them permanent"
                + (retryRefused ? juce::String (retryAll ? "; ALL RE-RUN this time (--retry-refused-all)" : "; the transient ones RE-RUN this time (--retry-refused)") : juce::String()) + "), "
                + juce::String (added) + " discovered from the ledger at " + ledgerRoot.getFullPathName());
    report.addArray (in.notes);
    juce::StringArray ex;
    for (const auto& [why, n] : disc.excluded) ex.add (juce::String (n) + " " + why);
    report.add ("installed AUs not discovered: " + ex.joinIntoString (", "));
    if (! disc.excludedByDisposition.isEmpty())
        report.add ("left alone by categories.json disposition: " + disc.excludedByDisposition.joinIntoString ("; "));
    if (! disc.tuners.isEmpty()) report.add ("pitch category, on the worklist for tuner certification: " + disc.tuners.joinIntoString (", "));
    {
        juce::StringArray un; for (const auto& u : disc.unmapped) un.add (u.name + " " + u.version + " [" + u.category + "]");
        report.add ("NO MAP YET (information, not a gate - certified anyway; the mapping sweep can map them, the server joins by map_fp): "
                    + juce::String ((int) disc.unmapped.size()) + (un.isEmpty() ? juce::String() : ": " + un.joinIntoString (", ")));
    }
    if (unmappedOut != nullptr) *unmappedOut = disc.unmapped;
    return out;
}

// The EJ Map ledger EJ Map itself uses when none is given.
inline juce::File defaultEjmapLedger() { return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap"); }

//==============================================================================
// THE THRESHOLD SWEEP, one product (spec section 4; --cert-sweep). The probe measures, EJ Map
// plans and derives (EjmapSweep.h). Same gates as --cert-defaults: the signature once, the
// reachability classification, the retry rule, the window watch, and every probe output kept.
// ONE PROCESS PER POSITION (ruled 29 Sep): a reference-only process at the default threshold
// (spec 4.7's unlicensed test only), then one process per position, levels quiet to loud.
// The fixture is written to <out>/fixtures only - never into the server tree, never over a
// pushed fixture - with LF line ends. processes.json records every process, so the fixture
// can be re-derived from the raw files alone (RoundTripTest does exactly that).
// THE FOLLOW-UP'S PROJECTION (5 Oct, R1): what a derive-only run of runToneCheckAll decided it WOULD do - re-sweeps with
// their reasons, the sidechain-reading set, the inert checks. Filled through SweepOptions::projection when a caller
// (the zip review) asks; the decision code is the follow-up's own, not a second copy of it.
struct FollowUpProjection
{
    struct Item { juce::String product, why; bool afterRederive = false; };
    std::vector<Item> resweeps, sidechain; juce::StringArray inert;
};
struct SweepOptions
{
    juce::File fixtures, probe, out, ledger = defaultEjmapLedger();
    juce::String product, hostVersion, armLabel;
    std::vector<std::pair<int, float>> extraSets;    // a DIAGNOSTIC arm: a non-swept control moved on purpose
    int timeoutMs = 120000;                          // per process
    bool includePace = false, resetPerHold = false, retryRefused = false, retryAll = false;
    bool profile = false;                            // the profile sweep (31 levels, 2.5 s, quiet reference everywhere)
    juce::StringArray slice;                         // the dress rehearsal only: product names the batch is limited to (empty = all)
    bool retryLicence = false;                       // --retry-licence: re-check the needs-licence set (the licence is back)
    bool deriveOnly = false;                         // --derive-only (tone-check mode): re-derive, apply the rules and export, load NOTHING - the projection for a zipped-back folder
    juce::StringArray resweepProducts;               // the follow-up's own re-sweep set (planDiffers): forced back onto the worklist
    juce::String mapState;                           // INFORMATION for the record (ruled 2 Oct): set by the batch from the subject; the tuner path has no Subject of its own
    FollowUpProjection* projection = nullptr;        // the zip review's capture of a derive-only decision pass (never set by a batch)
};

// WHERE CERTIFICATION LANDS BY DEFAULT (ruled 30 Sep). The runbook hands a machine's work over as `zip -rq
// ~/Library/ejmap`, so anything written outside it never comes home: the sweep used to require --out with no default,
// and an operator's choice of directory decided whether the work survived. Now:
//   --out       defaults to ~/Library/ejmap/cert/            (traces, processes, reports, run.jsonl)
//   --fixtures  defaults to <out>/fixtures/                  THE STORE IS THE OUTPUT DIRECTORY
//   --probe     defaults to EchoJayProbe beside the running ejmap executable (inside ejmap.app once it ships there)
// The second line is the one that matters: the worklist prunes against --fixtures and the sweep writes to
// <out>/fixtures, so "a second run skips what the first certified" was only ever true when the two pointed at the
// same place. With the defaults they do. Explicit flags keep working for repo-store runs.
inline constexpr int kUncleanBudget = 2;     // unclean processes (after their once-retry) before a product is refused
inline juce::File defaultCertRoot() { return defaultEjmapLedger().getChildFile ("cert"); }
inline juce::File defaultProbeBeside (const juce::File& executable) { return executable.getSiblingFile ("EchoJayProbe"); }
template <typename Opts>
inline void resolveCertPaths (Opts& o, const juce::File& executable)
{
    if (o.out == juce::File())      o.out = defaultCertRoot();
    if (o.fixtures == juce::File()) o.fixtures = o.out.getChildFile ("fixtures");
    if (o.probe == juce::File())    o.probe = defaultProbeBeside (executable);
}

// The architecture the plugin RUNS in (ruling item 4): the bundle's own slices against this
// host's. A bundle with no slice for the host runs bridged, out of process, under Rosetta.
inline juce::String bundleArchs (const juce::File& bundle)
{
    auto plist = juce::parseXML (bundle.getChildFile ("Contents/Info.plist"));
    juce::String exeName;
    if (plist != nullptr)
        if (auto* dict = plist->getChildByName ("dict"))
            for (auto* k = dict->getFirstChildElement(); k != nullptr; k = k->getNextElement())
                if (k->hasTagName ("key") && k->getAllSubText() == "CFBundleExecutable" && k->getNextElement() != nullptr)
                    exeName = k->getNextElement()->getAllSubText();
    if (exeName.isEmpty()) return {};
    auto r = runChild ({ "/usr/bin/lipo", "-archs", bundle.getChildFile ("Contents/MacOS").getChildFile (exeName).getFullPathName() }, 20000);
    return r.cleanExit() ? r.out.trim() : juce::String();
}

inline juce::String fixtureFileName (const juce::var& f)
{
    return "AudioUnit_" + f.getProperty ("uid", "").toString() + "_" + f.getProperty ("version", "").toString() + ".json";
}

// DERIVE, COMPOSE, WRITE AND REPORT: shared by a live sweep and by a re-derivation from its traces, so the two can
// never disagree about what a trace means (decision D2: re-compute, never re-measure).
struct SweepRunInfo { juce::String headline, probeLabel, armLine; int processes = 0, retried = 0, sleptProcesses = 0; double sleptMs = 0; bool newIdentity = false; };

struct Derivation { sweep::Derived d; sweep::DisplayCheck dc; juce::var sweepVar; juce::String report; bool written = false; };

inline Derivation deriveOne (const sweep::Plan& plan, const sweep::Measured& m, const sweep::Provenance& pv, const SweepRunInfo& info,
                             const juce::String& fixtureName)
{
    Derivation out;
    const auto levels = plan.testLevels();
    const auto d = sweep::derive (m, levels, plan.ratioIndex, plan.quietReference);
    const auto dc = sweep::displayCheck (d, plan.thrUnit);
    const bool written = ! d.unlicensedSuspect;
    out.d = d; out.dc = dc; out.written = written;
    if (written) out.sweepVar = sweep::composeThresholdSweep (d, dc, plan, pv);

    int slicesTotal = 0, instack = 0, unl = 0, rer = 0, landed = 0, failedProc = 0; double confMax = 0;
    for (const auto& p : m.positions)
    {
        if (p.processFailed) { ++failedProc; continue; }
        if (p.unlanded) { ++unl; continue; }
        ++landed; slicesTotal += p.slices; if (p.instackMatch) ++instack; confMax = juce::jmax (confMax, p.confirmMs);
        if (p.rerendered) ++rer;
    }
    auto num = [] (std::optional<double> v, int dp = 2) { return v ? juce::String (*v, dp) : juce::String ("null"); };
    juce::String rep;
    rep << info.headline << "\n";
    if (info.probeLabel.isNotEmpty()) rep << "probe: " << info.probeLabel << "\n";
    rep << "host: " << pv.host << (pv.bridged ? " (BRIDGED)" : " (native)") << "\n";
    if (info.armLine.isNotEmpty()) rep << info.armLine << "\n";
    rep << "threshold: [" << plan.thr << "] " << plan.thrName << " unit '" << plan.thrUnit << "' (class " << plan.cls
        << (plan.thrFlags.isEmpty() ? "" : ", flags " + plan.thrFlags.joinIntoString (",")) << ") | positions " << (int) plan.norms.size()
        << (plan.stepped ? " (every step)" : " (16 evenly spaced)") << ", one process each, levels quiet to loud\n";
    if (plan.pickNote.isNotEmpty()) rep << "pick: " << plan.pickNote << "\n";
    if (! plan.linkStates.isEmpty()) rep << "links as instantiated (recorded, not written): " << plan.linkStates.joinIntoString (", ") << "\n";
    rep << "ratio: " << (plan.ratioIndex >= 0 ? "[" + juce::String (plan.ratioIndex) + "] ran at '" + d.ratioText + "' -> "
                                                  + (d.ratio ? juce::String (*d.ratio) : juce::String ("not numeric"))
                                                  + (plan.ratioRaise ? " (RAISED from '" + d.ratioInstantiated + "', value READ BACK after the write)" : juce::String())
                                                : plan.ratioNote)
        << " | auto make-up disabled: " << (plan.autoMakeupDisabled ? "yes" : "no") << "\n"
        << "default-threshold reference ('" << m.refText << "', spec 4.7 check only):";
    for (double L : levels) { const auto k = sweep::levelKey (L); if (m.refDb.count (k)) rep << "  " << k << " out-in " << juce::String (m.refDb.at (k) - m.inRmsDb.at (k), 2); }
    if (! d.defaultGain.empty()) rep << "  (default gain is information only)";
    if (d.quietReference)
    {
        rep << "\nlinear reference: each position's own gain at its quiet rung's upper level, checked against the lower (must differ by 6 dB within "
            << juce::String (sweep::kQuietTolDb, 1) << "):";
        for (size_t i = 0; i < d.quietCheckDb.size(); ++i)
            rep << " " << (d.quietCheckDb[i] ? juce::String (*d.quietCheckDb[i], 2) : juce::String ("--"));
        { int desc = 0; juce::StringArray at; for (size_t i = 0; i < d.quietRungDb.size(); ++i) if (d.quietRungDb[i] && d.quietRungDb[i]->first != sweep::kQuietLadder.front().first) { ++desc; at.add (juce::String ((int) i) + "@" + juce::String ((int) d.quietRungDb[i]->second)); }
          rep << "\nreference ladder: " << desc << " position(s) below the first rung" << (desc > 0 ? " (" + at.joinIntoString (" ") + ")" : juce::String()); }
    }
    else
    {
        rep << "\nlinear reference (soft end";
        if (d.softEnd) { rep << ", position " << *d.softEnd << "):"; for (double L : levels) rep << "  " << sweep::levelKey (L) << " " << juce::String (d.linearGain.at (sweep::levelKey (L)), 2); }
        else rep << "): none";
        if (d.softEndSpreadDb) rep << "  | spread " << juce::String (*d.softEndSpreadDb, 2) << " dB (unusable beyond " << juce::String (sweep::kLinearDb, 1) << ")";
    }
    rep << "\n";
    {
        std::map<juce::String, int> counts;
        for (const auto& x : d.landedBy) ++counts[x.isEmpty() ? "no_process" : x];
        rep << "writes landed by:";
        for (const auto& [k, v] : counts) rep << " " << k << " " << v;
        rep << "\n";
    }
    if (info.processes > 0) rep << "processes: " << info.processes << " run, " << info.retried << " retried, " << failedProc << " position(s) failed"
                                << (info.sleptProcesses > 0 ? ", " + juce::String (info.sleptProcesses) + " SLEPT DURING THEIR RUN (" + juce::String (info.sleptMs / 1000.0, 1)
                                                                + " s asleep; their readings are recorded as measured, not re-run)" : juce::String()) << "\n";
    rep << "write verify: " << landed << " landed, " << unl << " write_unlanded (skipped), in-stack read already matched on " << instack
        << " of " << landed << ", pump slices total " << slicesTotal << ", longest confirm " << juce::String (confMax, 1) << " ms, re-rendered " << rer << "\n";
    { juce::StringArray a; for (int i : d.holdDoubled) a.add (juce::String (i)); rep << "hold doubled at positions: " << (a.isEmpty() ? "none" : a.joinIntoString (",")) << "\n"; }
    { juce::StringArray a; for (int i : d.stillMoving) a.add (juce::String (i)); rep << "still moving AFTER doubling (reading not used): " << (a.isEmpty() ? "none" : a.joinIntoString (",")) << "\n"; }
    rep << "\npos   norm      display        red@-24  red@-12  red@-6    T(peak)   T-display\n";
    for (size_t i = 0; i < d.norms.size(); ++i)
    {
        auto cell = [&] (double L) {
            auto it = d.reduction.find (sweep::levelKey (L));
            std::optional<double> g = it != d.reduction.end() && i < it->second.size() ? it->second[i] : std::nullopt;
            return (g ? juce::String (*g, 2) : juce::String ("--")).paddedLeft (' ', 7); };
        const juce::var tv = i < d.tEquivalent.size() ? d.tEquivalent[i] : juce::var();
        juce::String t = tv.isVoid() ? "--" : tv.isObject() ? juce::JSON::toString (tv, true) : juce::String ((double) tv, 1);
        juce::String off;
        for (const auto& o : dc.offsets) if (o.first == (int) i) off = juce::String (o.second, 2);
        rep << juce::String ((int) i).paddedLeft (' ', 3) << "  " << juce::String (d.norms[i], 4).paddedRight (' ', 8) << "  "
            << d.texts[i].paddedRight (' ', 13) << cell (-24) << "  " << cell (-12) << "  " << cell (-6) << "   "
            << t.paddedLeft (' ', 9) << "   " << off.paddedLeft (' ', 7) << "\n";
    }
    rep << "\n1 MAP:             " << d.result << (d.reason.isNotEmpty() ? " (" + d.reason + ")" : juce::String())
        << " | sense " << (d.sense.isEmpty() ? "--" : d.sense) << " | engage";
    for (double L : levels) { auto e = d.engage.count (sweep::levelKey (L)) ? d.engage.at (sweep::levelKey (L)) : std::nullopt; rep << " " << sweep::levelKey (L) << ":" << (e ? juce::String (*e) : juce::String ("none")); }
    rep << "\n2 engage drift:    " << num (dc.engageDriftDb);
    for (const auto& [k, v] : dc.engage) rep << "  " << k << ":" << num (v);
    rep << (dc.engage.empty() ? "  (not a dB display)" : "") << "   (primary; displayLinear unset until the population decides)"
        << "\n   offset IQR:      " << (dc.positions > 0 ? juce::String (dc.iqrDb, 2) + " dB over " + juce::String (dc.positions) + " positions (min "
                                                         + juce::String (dc.minDb, 2) + ", max " + juce::String (dc.maxDb, 2) + ")" : juce::String ("null"))
        << "\n3 displayOffsetDb: " << num (dc.offsetDb) << (dc.offsetDb ? juce::String (std::abs (*dc.offsetDb) <= sweep::kDisplayDb ? "  (inside spec 7's 2 dB bar)" : "  (outside spec 7's 2 dB bar)") : juce::String())
        << "\n" << (written ? "fixture: " + fixtureName : "NOT LICENSED suspected (silent, non-finite or not the input's tone at default): " + d.referenceNote + "no thresholdSweep written") << "\n";
    out.report = rep;
    return out;
}

// A REFUSAL IS A RECORD (ruled 30 Sep). A product stopped before a usable measurement - its defaults would not sample,
// its plan found no threshold, its ratio search failed, a window appeared, its unclean-process budget ran out, or its
// reference was not the input's tone - used to write NOTHING, so discovery offered it again on every batch: four
// minutes of timeouts per batch per hanger, for ever. It now writes the fixture it has (the store's, or the discovered
// identity with no controls yet) with `thresholdRefusal` naming the stage and the reason, and NO thresholdSweep. The
// worklist treats that as recorded; --retry-refused, a new installed version, or deleting the file puts it back.
inline juce::File writeRefusalRecord (const juce::File& fixturesDir, const juce::var& base, const juce::String& stage, const juce::String& reason,
                                      int processes, int unclean, const juce::String& probeLabel, const juce::String& host)
{
    auto f = sweep::stripPrivate (juce::JSON::parse (juce::JSON::toString (base)));
    if (auto* o = f.getDynamicObject())
    {
        o->removeProperty ("thresholdSweep"); o->removeProperty ("thresholdCandidates"); o->removeProperty ("thresholdReview");
        auto* r = new juce::DynamicObject();
        r->setProperty ("stage", stage);
        r->setProperty ("retry", refusalIsPermanent (stage) ? "permanent: a fact about the product; --retry-refused skips it (--retry-refused-all does not)"
                                                             : "transient: a fact about the run; --retry-refused re-runs it once the reason is fixed");
        r->setProperty ("reason", reason);
        r->setProperty ("recordedAt", juce::Time::getCurrentTime().toISO8601 (false));
        r->setProperty ("host", host);
        r->setProperty ("probe", probeLabel);
        r->setProperty ("processes", processes);
        r->setProperty ("uncleanProcesses", unclean);
        r->setProperty ("note", "stopped before a usable measurement: no thresholdSweep. The worklist skips a recorded refusal; "
                                "re-run it with --retry-refused (after fixing what the reason names), or by deleting this file, "
                                "or it re-runs on its own at a new installed version");
        o->setProperty ("thresholdRefusal", juce::var (r));
    }
    sweep::stampSchema (f, kSchemaCompressor);
    fixturesDir.createDirectory();
    auto out = fixturesDir.getChildFile (fixtureFileName (base));
    out.replaceWithText (juce::JSON::toString (f) + "\n", false, false, "\n");
    return out;
}

// Returns whether a thresholdSweep was written; false is the licence-suspect case (silent, non-finite or off-tone at
// default), which the caller records as a refusal.
inline bool composeAndReport (const juce::var& base, const sweep::Plan& plan, const sweep::Measured& m, const sweep::Provenance& pv,
                              const juce::File& fixtureOut, const juce::File& reportOut, const SweepRunInfo& info, juce::String* whyNot = nullptr,
                              const sweep::Derived* repeat = nullptr)
{
    auto one = deriveOne (plan, m, pv, info, fixtureOut.getFileName());
    if (one.written) sweep::attachRepeatQuality (one.sweepVar, one.d, repeat);
    if (one.written)
        fixtureOut.replaceWithText (juce::JSON::toString (sweep::composeFixture (base, one.sweepVar)) + "\n", false, false, "\n");
    else if (whyNot != nullptr)
        *whyNot = "not licensed suspected (silent, non-finite or not the input's tone at default): " + one.d.referenceNote;
    reportOut.replaceWithText (one.report, false, false, "\n");
    std::cout << one.report << std::flush;
    return one.written;
}

// A LICENCE IS A PROPERTY OF THE PRODUCT, NOT OF A CANDIDATE (ruled 30 Sep, the third misfire of a licence inference
// after the PACE bundle markers and the 3 dB reference test). If ANY candidate produced the input's tone, the plugin is
// licensed, and a candidate whose reference was silent or off-tone at default is saying something about THAT STAGE (a
// gate closed on the tone, a processor that mutes) - never about licensing. Such a candidate keeps the result its data
// gave it and records the silent reference beside it. Only a product silent on every candidate stays licence-suspect.
inline void resolveLicenceAtProductLevel (std::vector<std::pair<sweep::Plan, Derivation>>& cands, const sweep::Provenance& pv)
{
    bool producedAudio = false;
    for (const auto& [q, one] : cands) producedAudio = producedAudio || ! one.d.unlicensedSuspect;
    if (! producedAudio) return;
    for (auto& [q, one] : cands)
    {
        if (! one.d.unlicensedSuspect) continue;
        one.written = true;
        one.sweepVar = sweep::composeThresholdSweep (one.d, one.dc, q, pv);
        if (auto* o = one.sweepVar.getDynamicObject())
            o->setProperty ("silentOrOffToneAtDefault", "this candidate's reference at default was not the input's tone (" + one.d.referenceNote.trim()
                                                        + "); another candidate of the same product produced it, so this is a fact about the stage, not the licence");
        one.report = one.report.replace ("NOT LICENSED suspected (silent, non-finite or not the input's tone at default): ",
                                         "reference silent or off-tone at default, NOT a licence question (another candidate produced the tone): ")
                               .replace ("no thresholdSweep written", "sweep written with its data's result '" + one.d.result + "'");
    }
}

// SEVERAL CANDIDATES (ruled 30 Sep): one fixture carrying every candidate's sweep, labelled, and NO thresholdSweep - a
// human reads the curves and picks. The fixture says so in `thresholdReview`.
// BOTH OUTPUT CHANNELS, FROM A CANDIDATE'S OWN SWEEP CAPTURES (ruled 4 Oct): the probe prints every hold's per-channel level
// ("ch a,b"); the worst |a - b| over every reading above silence is the channel agreement the pair rules need. Reads the
// traces named in processes.json whose tag starts with the candidate's prefix (c<idx>.pos..), repeats included.
inline std::optional<std::pair<double, int>> channelAgreementFromTraces (const juce::File& processesJson, const juce::File& rawDir, const juce::String& prefix)
{
    const auto procs = juce::JSON::parse (processesJson.loadFileAsString());
    const auto* a = procs.getArray(); if (a == nullptr) return std::nullopt;
    double worst = 0.0; int n = 0;
    for (const auto& p : *a)
    {
        const auto tag = p.getProperty ("tag", "").toString();
        if (! tag.startsWith (prefix + "pos") && ! tag.startsWith ("r2." + prefix + "pos")) continue;
        for (const auto& line : juce::StringArray::fromLines (rawDir.getChildFile (p.getProperty ("file", "").toString()).loadFileAsString()))
        {
            if (! line.startsWith ("hold\t")) continue;
            const auto f = juce::StringArray::fromTokens (line, "\t", "");
            const int k = f.indexOf ("ch"); if (k < 0 || k + 1 >= f.size()) continue;
            const auto chs = juce::StringArray::fromTokens (f[k + 1], ",", ""); if (chs.size() < 2) continue;
            const double c0 = chs[0].getDoubleValue(), c1 = chs[1].getDoubleValue();
            if (c0 < sweep::kSilentDb || c1 < sweep::kSilentDb) continue;
            worst = juce::jmax (worst, std::abs (c0 - c1)); ++n;
        }
    }
    if (n == 0) return std::nullopt;
    return std::make_pair (worst, n);
}

// THE MEASURED CANDIDATE RULES (items 1, 10, 11, 12; EjmapCandidateRules.h), tried when Rule 1 did not decide
inline juce::var decideByMeasurement (const std::vector<std::pair<sweep::Plan, Derivation>>& cands, const juce::File& processesJson, const juce::File& rawDir)
{
    std::vector<candidaterules::CandidateFacts> facts;
    for (const auto& [q, one] : cands)
    {
        candidaterules::CandidateFacts f; f.index = q.thr; f.name = q.thrName; f.certified = one.written && one.d.result == "certified"; f.pairWritten = q.pairIndex; f.pairWrittenName = q.pairName;
        for (size_t i = 0; i < one.d.inAtGr.size() && i < one.d.norms.size(); ++i) { const auto it = one.d.inAtGr[i].at.find (2); if (it != one.d.inAtGr[i].at.end() && (it->second.isDouble() || it->second.isInt())) f.curve2[std::round ((double) one.d.norms[i] * 1e4) / 1e4] = (double) it->second; }
        if (processesJson.existsAsFile()) if (const auto ch = channelAgreementFromTraces (processesJson, rawDir, "c" + juce::String (q.thr) + ".")) { f.channelWorstDb = ch->first; f.channelReadings = ch->second; }
        facts.push_back (f);
    }
    const auto d = candidaterules::decide (facts);
    if (! d.decided) { if (d.whyNot.isNotEmpty()) std::cout << "  no measured rule: " << d.whyNot << std::endl; return {}; }
    auto* rd = new juce::DynamicObject();
    rd->setProperty ("rule", d.rule); rd->setProperty ("ruleText", d.note);
    auto* pk = new juce::DynamicObject(); pk->setProperty ("index", d.pick); pk->setProperty ("name", d.pickName); rd->setProperty ("pick", juce::var (pk));
    juce::Array<juce::var> others; for (const auto& o : d.others) others.add (o); rd->setProperty (d.rule == "master_over_trims" ? "trims" : "twin", others);
    if (d.pairWith >= 0) { auto* pw = new juce::DynamicObject(); pw->setProperty ("index", d.pairWith); pw->setProperty ("name", d.pairWithName); rd->setProperty ("pair_with", juce::var (pw)); }
    std::cout << "  " << d.rule << ": " << d.note << std::endl;
    return juce::var (rd);
}

inline void composeCandidatesAndReport (const juce::var& base, const sweep::Plan& plan, const std::vector<std::pair<sweep::Plan, Derivation>>& cands,
                                        const juce::File& fixtureOut, const juce::File& reportOut, const juce::var& ruleDecided = {})
{
    auto f = sweep::stripPrivate (juce::JSON::parse (juce::JSON::toString (base)));
    if (ruleDecided.isObject())
    {
        f.getDynamicObject()->setProperty ("ruleDecided", ruleDecided);
        auto* pk = new juce::DynamicObject(); pk->setProperty ("index", ruleDecided.getProperty ("pick", {}).getProperty ("index", -1)); pk->setProperty ("name", ruleDecided.getProperty ("pick", {}).getProperty ("name", ""));
        const auto rule = ruleDecided.getProperty ("rule", "").toString();
        pk->setProperty ("note", rule.startsWith ("R1") ? juce::String ("decided by Rule 1 (ruleDecided): the comp-worded candidate certified alone") : "decided by the measured rule '" + rule + "' (ruleDecided): " + ruleDecided.getProperty ("ruleText", "").toString());
        f.getDynamicObject()->setProperty ("pickedCandidate", juce::var (pk));
    }
    juce::Array<juce::var> arr;
    juce::String rep;
    for (const auto& [q, one] : cands)
    {
        auto* c = new juce::DynamicObject();
        c->setProperty ("index", q.thr); c->setProperty ("name", q.thrName);
        juce::Array<juce::var> fl; for (auto& x : q.thrFlags) fl.add (x); c->setProperty ("flags", fl);
        c->setProperty ("thresholdSweep", one.written ? sweep::stripPrivate (one.sweepVar) : juce::var());
        // WHY A CANDIDATE IS LICENCE-SUSPECT is on the candidate (ruled 4 Oct): the reference note - silent at every level,
        // non-finite, or not the tone - so the row can say "licence suspected: <reason>" rather than count the candidates
        if (! one.written) c->setProperty ("licenceSuspectReason", one.d.referenceNote.trim().trimCharactersAtEnd (";"));
        // A FLAT CANDIDATE IS EVIDENCE, NOT A DEFECT (ruled 30 Sep): 997 Hz excites one band, so a band that does not
        // cover it shows nothing. Said on the candidate, so thresholdReview does not read as a dead end. A PASS-THROUGH
        // flat is NOT that evidence: the product did nothing as instantiated, so the tone says nothing about its bands.
        if (one.d.result == "flat" && one.d.passThroughAtDefaults)
            c->setProperty ("reading", "pass_through_at_defaults: output equals input at every reading, so the product does nothing at its instantiate "
                                       "defaults; not evidence about band coverage or this control (a precondition gap, not a band gap)");
        else if (one.d.result == "flat")
            c->setProperty ("reading", "no response at 997 Hz: this candidate's band does not cover the test tone, or the control is not a threshold at it; "
                                       "uncertified for want of an in-band tone (a per-band tone is a later feature)");
        else if (one.d.result == "certified")
            c->setProperty ("reading", "responds at 997 Hz: a curve to read");
        arr.add (juce::var (c));
        rep << one.report << "\n";
    }
    // ONE FIELD, NOT A SCAN (ruled 30 Sep): the responding candidates are NAMED here, index and name, and every candidate's
    // verdict sits beside them. Two consumers deriving the same thing from the array is how the Waves untick bug happened;
    // the server half reads thresholdReview.responding and nothing else.
    auto ref = [] (const sweep::Plan& q) { auto* o = new juce::DynamicObject(); o->setProperty ("index", q.thr); o->setProperty ("name", q.thrName); return juce::var (o); };
    juce::Array<juce::var> responding, verdicts, passThrough;
    int curves = 0, flats = 0, passCount = 0;
    for (const auto& [q, one] : cands)
    {
        const juce::String verdict = ! one.written ? "licence_suspect" : one.d.passThroughAtDefaults ? "pass_through_at_defaults" : one.d.result;
        auto v = ref (q); v.getDynamicObject()->setProperty ("result", verdict); verdicts.add (v);
        if (one.d.result == "certified") { ++curves; responding.add (ref (q)); }
        else if (one.d.result == "flat" && one.d.passThroughAtDefaults) { ++passCount; passThrough.add (ref (q)); }
        else if (one.d.result == "flat") ++flats;
    }
    if (auto* o = f.getDynamicObject())
    {
        auto* rv = new juce::DynamicObject();
        rv->setProperty ("class", plan.cls);
        rv->setProperty ("candidates", (int) cands.size());
        rv->setProperty ("note", "several controls hold the threshold role; each was swept with the others at their instantiate defaults. "
                                 "On a multiband, the band containing the 997 Hz test tone can certify and the rest are uncertified for want of an in-band tone. "
                                 "There is no thresholdSweep and no dB-equivalent map until a human picks from the curves; the map comes after the pick, when that band's own ratio can be read. "
                                 "`responding` names the candidate(s) that respond; `passThroughAtDefaults` names those where the product did nothing as instantiated (not band evidence); `verdicts` holds every candidate's result");
        rv->setProperty ("curves", curves);
        rv->setProperty ("flats", flats);                       // flats that ARE band-coverage evidence: pass-through is counted apart
        rv->setProperty ("responding", responding);             // THE FIELD: which candidate(s) respond at 997 Hz, by index and name
        auto* pt = new juce::DynamicObject();
        pt->setProperty ("count", passCount); pt->setProperty ("candidates", passThrough);
        rv->setProperty ("passThroughAtDefaults", juce::var (pt));
        rv->setProperty ("verdicts", verdicts);                 // every candidate's result in one place, same index/name keys
        o->setProperty ("thresholdReview", juce::var (rv));
        o->setProperty ("thresholdCandidates", arr);
        sweep::refusalEndedBySweep (f);
    }
    sweep::stampSchema (f, kSchemaCompressor);
    fixtureOut.replaceWithText (juce::JSON::toString (f) + "\n", false, false, "\n");
    reportOut.replaceWithText (rep, false, false, "\n");
    std::cout << rep << std::flush;
}

// THE INERT CHECK'S PROCESSES (ruled 4 Oct; the candidates and the verdict in EjmapSweep.h): a control reading with the
// plan's writes alone, then one process per candidate with that one write added, each at one position and -6 dBFS peak,
// 1.5 s hold. The runner is the caller's (the batch's runProbe with its ledger, or the follow-up's runChild).
struct InertRun { bool ran = false, inert = false; juce::String reason; std::vector<sweep::Plan::InertTry> tried; double controlDb = -999.0; };
inline InertRun runInertCheck (const juce::var& fixture, int thr, float norm, const juce::StringArray& sets,
                               const std::function<ChildResult (const juce::String& tag, const juce::StringArray& args)>& run)
{
    InertRun ir;
    auto args = [&] (const juce::StringArray& extra) {
        juce::StringArray a { "--sweep", "thr=" + juce::String (thr), "norms=" + juce::String (norm, 6), "levels=-6", "hz=997", "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" };
        juce::StringArray all = sets; all.addArray (extra); if (! all.isEmpty()) a.add ("set=" + all.joinIntoString (",")); return a; };
    auto reading = [] (const ChildResult& r) -> std::optional<double> {
        if (! r.cleanExit()) return {}; const auto t = sidechaincheck::parseTrace (r.out); if (! t.ok || ! t.holdDb.count (-6.0)) return {}; return t.holdDb.at (-6.0); };
    ir.tried = sweep::inertCandidates (fixture, thr);
    if (ir.tried.empty()) { ir.reason = "inert check: no power switch or gain-type control to try"; return ir; }
    const auto ctl = run ("inert-ctl", args ({}));
    const auto c = reading (ctl);
    if (! c) { ir.reason = "inert check: the control process gave no reading (" + ctl.describe() + ")"; return ir; }
    ir.controlDb = *c; ir.ran = true;
    for (auto& t : ir.tried)
    {
        const auto r = run ("inert-" + juce::String (t.index), args ({ juce::String (t.index) + ":" + juce::String (t.norm, 6) }));
        if (const auto v = reading (r)) { t.ran = true; t.afterDb = *v; } else t.note = r.describe();
        if (r.kind == ChildResult::Kind::uiShown) break;
    }
    ir.inert = sweep::inertVerdict (ir.controlDb, ir.tried, ir.reason);
    return ir;
}

inline int runCertSweep (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const SleepGuard sleepGuard ("EJ Map certification sweep");
    if (! opt.out.createDirectory()) { say ("SWEEP: cannot create " + opt.out.getFullPathName()); return 2; }
    const auto id = checkProbe (opt.probe, {}, {});
    if (! id.ok) { say ("SWEEP: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const juce::String probeLabel = "signed EchoJayProbe (feat/ejmap-cert), team " + id.team + ", cdhash " + id.cdhash;
    const juce::String date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");

    juce::StringArray wl;
    auto subjects = buildWorklist (opt.fixtures, opt.ledger, opt.includePace, wl, opt.retryRefused, opt.retryAll, nullptr, false, &opt.resweepProducts);
    const Subject* sp = nullptr;
    for (const auto& x : subjects) if (x.product == opt.product) { sp = &x; break; }
    if (sp == nullptr) { say ("SWEEP: '" + opt.product + "' is not on the worklist (" + wl.joinIntoString ("; ") + ")"); return 2; }
    const Subject& s = *sp;
    if (! measurable (s, opt.includePace))
    {
        juce::String why = reachName (s.reach) + (s.detail.isNotEmpty() ? " (" + s.detail + ")" : juce::String());
        if (s.reach == Subject::Reach::versionMismatch)
            why << (! s.installedUnique ? "; more than one version installed, and one is never picked"
                    : s.hardware ? "; needs its hardware" : "; licence-bound (run with --include-pace)");
        say ("SWEEP: " + s.product + " is not measurable here: " + why);
        return 4;
    }

    const juce::String armTag = opt.armLabel.isNotEmpty() ? ".arm-" + opt.armLabel : juce::String();
    auto raw = opt.out.getChildFile ("raw");
    raw.createDirectory();
    auto ledger = opt.out.getChildFile ("run.jsonl");
    juce::Array<juce::var> processes;
    bool windowSeen = false;
    juce::StringArray windows;
    int unclean = 0;
    juce::StringArray uncleanWhat;
    auto overBudget = [&] { return unclean >= kUncleanBudget; };
    juce::var base = s.pushed;               // the store's fixture, or the discovered identity; replaced by the sampled defaults below
    // The trace stem for THIS product, known before its defaults are: a refusal's processes.json lands under it too.
    const auto stem0 = "AudioUnit_" + juce::String::toHexString (s.desc.uniqueId).toLowerCase() + "_" + s.desc.version + ".sweep" + armTag;
    // THE RETRY RULE, as --cert-defaults: anything but a clean exit is re-run ONCE; a refusal (exit 3) and a
    // window are answers, not retried. Every attempt's output is kept.
    auto runProbe = [&] (const juce::String& stem, const juce::String& tag, const juce::StringArray& extra, float norm) -> ChildResult
    {
        juce::StringArray args { opt.probe.getFullPathName(), s.desc.name, s.desc.fileOrIdentifier,
                                 juce::String::toHexString (s.desc.uniqueId) };
        args.addArray (extra);
        const auto a = runWithRetry (args, opt.timeoutMs, [&] (int attempt, const ChildResult& r)
        {
            const auto file = stem + "." + tag + "." + juce::String (attempt) + ".txt";
            raw.getChildFile (file).replaceWithText (r.out, false, false, "\n");
            auto* o = new juce::DynamicObject();
            o->setProperty ("tag", tag); o->setProperty ("attempt", attempt); o->setProperty ("file", file);
            // A process that slept is recorded as NOT clean: a re-derivation must not trust it either.
            const bool slept = r.sleptMs > kSleptMs;
            o->setProperty ("outcome", slept && r.cleanExit() ? "SLEPT " + juce::String (r.sleptMs / 1000.0, 1) + " s during its run: not trusted"
                                                              : r.describe());
            o->setProperty ("clean", r.cleanExit() && ! slept); o->setProperty ("norm", norm);
            o->setProperty ("ms", r.ms);
            o->setProperty ("slept_ms", std::round (r.sleptMs));
            processes.add (juce::var (o));
            ledger.appendText (juce::JSON::toString (juce::var (o), true) + "\n");
            if (r.kind == ChildResult::Kind::uiShown) { windowSeen = true; windows.addArray (r.windowsInTree); }
        });
        auto r = a.r;
        if (a.sleptTwice) r.kind = ChildResult::Kind::sleptTwice;   // refused: unclean, and it says why
        if (! r.cleanExit()) { ++unclean; uncleanWhat.add (tag + ": " + r.describe()); }
        return r;
    };
    auto licenceLine = [&] (const juce::String& what, const ChildResult& r) {
        const auto f = certoutcome::classifyFailure (s.licenceBound, r.windowsInTree);
        return (f == certoutcome::Failure::unlicensedOnHost ? "UNLICENSED ON HOST: " : "ERROR: ") + what + " " + r.describe(); };
    // THE PER-PRODUCT BUDGET (ruled 30 Sep): the mapper's sweep bounds a plugin at one 90 s process; a certification is
    // ~17 processes per candidate, each with a once-retry, so a plugin that hangs on every load cost ~68 minutes before
    // its fixture existed. After kUncleanBudget processes end unclean (after their retry) the product stops, and the
    // refusal is written THEN. The defaults phase stops on its first unclean process, as before - within the budget.
    auto refuse = [&] (int rc, const juce::String& stage, const juce::String& reason) {
        say ("SWEEP: " + s.product + " - " + reason);
        opt.out.getChildFile (stem0 + ".processes.json").replaceWithText (juce::JSON::toString (juce::var (processes)) + "\n", false, false, "\n");
        const auto rec = writeRefusalRecord (opt.out.getChildFile ("fixtures"), base, stage, reason, processes.size(), unclean,
                                             probeLabel, "EJ Map " + opt.hostVersion);
        say ("SWEEP: refusal recorded at stage '" + stage + "' (" + (refusalIsPermanent (stage) ? "permanent" : "transient") + ") -> " + rec.getFileName()
             + " (skipped by the worklist until " + (refusalIsPermanent (stage) ? "--retry-refused-all" : "--retry-refused") + " or a new version)");
        return rc; };

    // AN UNSEEN VERSION: sample defaults first, then sweep, in one pass (ruled 29 Sep). The identity is the
    // INSTALLED component's, composed by the same composeFixture as --cert-defaults.
    bool newIdentity = false;
    if (needsDefaultsFirst (s))
    {
        const auto dstem = "AudioUnit_" + juce::String::toHexString (s.desc.uniqueId).toLowerCase() + "_" + s.desc.version + ".defaults";
        auto lp = runProbe (dstem, "list-params", { "--list-params" }, -1.0f);
        if (! lp.cleanExit()) return refuse (1, "defaults", licenceLine ("defaults --list-params", lp));
        auto ta = runProbe (dstem, "text-at", { "--text-at", "all" }, -1.0f);
        if (! ta.cleanExit()) return refuse (1, "defaults", licenceLine ("defaults --text-at all", ta));
        auto tb = runProbe (dstem, "text-at-2", { "--text-at", "all" }, -1.0f);
        const auto text = parseTextAt (ta.out);
        const auto scan = scanReadouts (text, tb);
        std::vector<fixturereadout::Moved> moved;
        for (const auto& h : scan.hits) moved.push_back ({ h.index, h.a, h.b, h.ta, h.tb });
        base = fixturereadout::applySchema (composeFixture (s, parseListParams (lp.out), text, lp.code, ta.code, probeLabel, date),
                                            moved, scan.checked);
        newIdentity = true;
    }

    auto plan = sweep::planFromFixture (base);
    if (! plan.ok) return refuse (4, "plan", plan.why);
    if (opt.profile) plan.makeProfile();
    for (auto x : opt.extraSets) plan.sets.push_back (x);
    const auto stem = fixtureFileName (base).upToLastOccurrenceOf (".json", false, false) + ".sweep" + armTag;

    // THE RATIO RAISE (spec 4.2): read the ratio's own texts on a grid, pick the smallest READ value at or above 4:1,
    // and write it in every process. The value used for R is read back after that write, in each process.
    if (plan.ratioRaise)
    {
        const auto rc = sweep::findControl (base, plan.ratioIndex);
        std::vector<float> grid;
        if (sweep::isSteppedControl (rc)) { const int n = (int) rc.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) grid.push_back ((float) k / (float) (n - 1)); }
        else for (int k = 0; k <= 64; ++k) grid.push_back ((float) k / 64.0f);
        juce::StringArray gs;
        for (float g : grid) gs.add (juce::String (g, 6));
        const auto r = runProbe (stem, "ratio-search", { "--text-at-norms", juce::String (plan.ratioIndex), gs.joinIntoString (",") }, -1.0f);
        if (windowSeen || ! r.cleanExit()) return refuse (1, "ratio_search", licenceLine ("ratio search", r));
        std::vector<sweep::GridPoint> pts;
        for (const auto& line : juce::StringArray::fromLines (r.out))
        {
            const auto f = juce::StringArray::fromTokens (line, "\t", "");
            if (f.size() >= 3 && f[0] == "at" && f[2] == "landed") pts.push_back ({ (float) f[1].getDoubleValue(), f[f.indexOf ("text") + 1] });
        }
        juce::String chosenText;
        const auto norm = sweep::chooseRatioRaise (pts, chosenText);
        if (! norm) return refuse (4, "ratio_none", "ratio instantiates at '" + plan.ratioDefaultText + "' and no grid position reads 4:1 or more; not swept");
        plan.sets.push_back ({ plan.ratioIndex, *norm }); plan.setRoles[plan.ratioIndex] = "ratio_raise";
        std::cout << "  ratio raise: [" << plan.ratioIndex << "] '" << plan.ratioDefaultText << "' -> norm " << *norm << " (grid read '" << chosenText << "')" << std::endl;
    }

    // THE NEUTRAL SET, profile sweeps only: mix / make-up by role, drive by name; each chosen on the control's own text grid
    // and written as a precondition. What could not be chosen is said. Never the threshold, the ratio, or an engage switch.
    juce::StringArray neutralNotes;
    if (opt.profile)
    {
        std::vector<roles::NamedControl> named;
        if (const auto* cs = base.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs) named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(), false });
        const auto cl = roles::classify (named, roles::Category::compressor);
        std::vector<std::pair<int, juce::String>> wanted;
        for (const auto& r : cl.controls)
        {
            if (r.index == plan.thr || r.index == plan.ratioIndex) continue;
            if (r.role == "mix") wanted.push_back ({ r.index, "mix_wet" });
            else if (r.role == "makeup") wanted.push_back ({ r.index, "makeup_zero" });
            else if (r.role.isEmpty() && sweep::driveNamed (r.name)) wanted.push_back ({ r.index, "drive_cleanest" });
        }
        for (const auto& [idx, role] : wanted)
        {
            if (windowSeen || overBudget()) break;
            bool already = false; for (auto [i, v] : plan.sets) already = already || i == idx;
            if (already) continue;
            const auto ctl = sweep::findControl (base, idx);
            std::vector<float> grid;
            if (sweep::isSteppedControl (ctl)) { const int n = (int) ctl.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) grid.push_back ((float) k / (float) juce::jmax (1, n - 1)); }
            else for (int k = 0; k <= 32; ++k) grid.push_back ((float) k / 32.0f);
            juce::StringArray gs; for (float g : grid) gs.add (juce::String (g, 6));
            const auto r = runProbe (stem, "neutral-" + juce::String (idx), { "--text-at-norms", juce::String (idx), gs.joinIntoString (",") }, -1.0f);
            if (! r.cleanExit()) { neutralNotes.add (role + " [" + juce::String (idx) + "]: grid read failed (" + r.describe() + "), not set"); continue; }
            std::vector<sweep::GridPoint> pts;
            for (const auto& line : juce::StringArray::fromLines (r.out))
            {
                const auto f = juce::StringArray::fromTokens (line, "\t", "");
                if (f.size() >= 3 && f[0] == "at" && f[2] == "landed") pts.push_back ({ (float) f[1].getDoubleValue(), f[f.indexOf ("text") + 1] });
            }
            juce::String chosen;
            const auto norm = sweep::chooseNeutral (pts, role, chosen);
            if (! norm) { neutralNotes.add (role + " [" + juce::String (idx) + "] " + ctl.getProperty ("name", "").toString() + ": no numeric text on its grid, not set"); continue; }
            plan.sets.push_back ({ idx, *norm }); plan.setRoles[idx] = role;
            std::cout << "  neutral: [" << idx << "] " << ctl.getProperty ("name", "").toString() << " -> norm " << *norm << " ('" << chosen << "') for " << role << std::endl;
        }
    }
    juce::StringArray sets;
    for (auto [i, v] : plan.sets) sets.add (juce::String (i) + ":" + juce::String (v, 6));
    // ONE SWEEP of one threshold control: a reference process, then one process per position. Called once for a single
    // threshold, once per candidate when several hold the role (tags prefixed "c<index>." so the traces stay apart).
    std::map<juce::String, std::vector<sweep::ProcessOut>> repeatRuns;   // v1.3: the second measurement of every position, by prefix
    juce::String lastSweepPrefix;                                         // the tag prefix of the last full sweep (what repeatRuns is keyed on)
    auto sweepFor = [&] (const sweep::Plan& q, const juce::String& tagPrefix, sweep::ProcessOut& refOut, std::vector<sweep::ProcessOut>& posOut)
    {
        juce::StringArray levelList;
        for (double L : q.probeLevels()) levelList.add (juce::String ((int) L));
        auto sweepArgs = [&] (const juce::String& norms, const juce::String& ref) {
            juce::StringArray a { "--sweep", "thr=" + juce::String (q.thr), "norms=" + norms, "levels=" + levelList.joinIntoString (","), "hz=997",
                                  "hold=" + juce::String (q.holdS, 2), "discard=" + juce::String (q.discardS, 2), "win=" + juce::String (q.winS, 2), "ref=" + ref, "moving_db=0.1",
                                  juce::String ("reset=") + (opt.resetPerHold ? "1" : "0") };
            juce::StringArray all = sets;                                     // the plan's preconditions (ratio raise, auto make-up off)
            for (const auto& w : q.engage) all.add (juce::String (w.index) + ":" + juce::String (w.norm, 6));   // plus the engage writes
            if (q.pairIndex >= 0 && norms.isNotEmpty()) all.add (juce::String (q.pairIndex) + ":" + norms);          // the dual-mono twin, WITH the amount (ruled 6 Oct)
            if (! all.isEmpty()) a.add ("set=" + all.joinIntoString (","));
            return a; };
        lastSweepPrefix = tagPrefix;
        std::cout << "  sweeping " << s.product << (tagPrefix.isNotEmpty() ? " candidate [" + juce::String (q.thr) + "] " + q.thrName : juce::String())
                  << (newIdentity ? " (unseen version " + s.desc.version + ": defaults sampled first)" : juce::String())
                  << " - 1 reference + " << (int) q.norms.size() << " position processes" << std::endl;
        if (! windowSeen && ! overBudget())
        {
            const auto r = runProbe (stem, tagPrefix + "ref", sweepArgs ("", "2.0"), -1.0f);
            refOut = { r.out, r.cleanExit(), r.describe(), -1.0f };
        }
        posOut.clear();
        for (size_t k = 0; k < q.norms.size() && ! windowSeen && ! overBudget(); ++k)
        {
            const auto tag = tagPrefix + "pos" + juce::String ((int) k).paddedLeft ('0', 2);
            const auto r = runProbe (stem, tag, sweepArgs (juce::String (q.norms[k], 6), "0"), q.norms[k]);
            posOut.push_back ({ r.out, r.cleanExit(), r.describe(), q.norms[k] });
        }
        // v1.4 THE HOLD-DOUBLED REPEAT: every position measured again in a fresh process with the hold at 5 s (same 300 ms
        // read at its end), tagged "r2." + prefix; the disagreement on any in_at_gr point is quality.point_error_db, the
        // trust gate - a point that moves had not settled. Only the profile sweep (repeats = 2) does this.
        const bool repeatIt = q.repeats > 1 && ! windowSeen && ! overBudget()
                              && sweep::repeatWorthwhile (sweep::derive (sweep::mergeProcesses (refOut, posOut), q.testLevels(), q.ratioIndex, q.quietReference));
        if (q.repeats > 1 && ! repeatIt) std::cout << "  hold-doubled repeat skipped: the first pass is pass-through (identical at every position)" << std::endl;
        if (repeatIt)
        {
            std::vector<sweep::ProcessOut> again;
            const sweep::Plan slow = q.holdDoubled();
            auto slowArgs = [&] (const juce::String& norms) {
                juce::StringArray a { "--sweep", "thr=" + juce::String (slow.thr), "norms=" + norms, "levels=" + levelList.joinIntoString (","), "hz=997",
                                      "hold=" + juce::String (slow.holdS, 2), "discard=" + juce::String (slow.discardS, 2), "win=" + juce::String (slow.winS, 2), "ref=0", "moving_db=0.1",
                                      juce::String ("reset=") + (opt.resetPerHold ? "1" : "0") };
                juce::StringArray all = sets; for (const auto& w : q.engage) all.add (juce::String (w.index) + ":" + juce::String (w.norm, 6));
                if (! all.isEmpty()) a.add ("set=" + all.joinIntoString (","));
                return a; };
            for (size_t k = 0; k < q.norms.size() && ! windowSeen && ! overBudget(); ++k)
            {
                const auto tag = "r2." + tagPrefix + "pos" + juce::String ((int) k).paddedLeft ('0', 2);
                const auto r = runProbe (stem, tag, slowArgs (juce::String (q.norms[k], 6)), q.norms[k]);
                again.push_back ({ r.out, r.cleanExit(), r.describe(), q.norms[k] });
            }
            repeatRuns[tagPrefix] = again;
        }
    };
    sweep::ProcessOut refOut;
    std::vector<sweep::ProcessOut> posOut;
    std::vector<std::pair<sweep::Plan, std::pair<sweep::ProcessOut, std::vector<sweep::ProcessOut>>>> candRuns;
    std::optional<sweep::Plan::Candidate> ruleOneDecided;        // RULE 1: the comp-worded candidate that certified alone
    // THE QUIET-LEVEL FALLBACK (ruled 30 Sep): a sweep whose soft end has no linear anchor is re-swept at once with the
    // per-position quiet reference (levels -54/-48 added), tagged "q." + the candidate's prefix so both runs stay in the
    // traces. The plan that comes back is the one to derive from. Same processes, same probe, one more pass.
    // THE ENGAGE SEARCH (spec section 3, built 1 Oct): a candidate that reads pass-through at the instantiate defaults
    // is tried with each engage candidate in turn - a QUICK probe of three positions with the write - and the first
    // that shows gain reduction is verified (GR with the write, pass-through without: the sweep just taken). The full
    // sweep is then re-run with the write as a precondition, tagged "e<idx>." + prefix. Every refused candidate is
    // recorded. If none shows GR the product stays pass-through, and the fixture lists what was tried.
    auto engageSearch = [&] (sweep::Plan q, const juce::String& prefix) -> sweep::Plan
    {
        const auto cands = sweep::engageCandidates (base, q.thrName);
        std::cout << "  pass-through at defaults: trying " << (int) cands.size() << " engage candidate(s)" << std::endl;
        for (const auto& w : cands)
        {
            if (windowSeen || overBudget()) break;
            sweep::Plan t = q; t.engage = { w };
            sweep::ProcessOut tr; std::vector<sweep::ProcessOut> tp;
            sweep::Plan quick = t; quick.norms = { q.norms.front(), q.norms[q.norms.size() / 2], q.norms.back() };
            if (quick.profile) { quick.profile = false; quick.quietReference = false; quick.holdS = 1.5; quick.discardS = 0.75; quick.winS = 0.25; }   // the quick probe stays quick
            sweepFor (quick, "eq" + juce::String (w.index) + "." + prefix, tr, tp);
            const auto d = sweep::derive (sweep::mergeProcesses (tr, tp), quick.testLevels(), q.ratioIndex, quick.quietReference);
            const bool hit = sweep::showsResponse (d);
            q.engageTried.add (w.name + " -> " + juce::String (w.norm, 1) + " (from '" + w.fromDisplay + "'): " + (hit ? "GAIN REDUCTION" : d.result + (d.reason.isNotEmpty() ? " - " + d.reason : juce::String())));
            std::cout << "    " << q.engageTried[q.engageTried.size() - 1] << std::endl;
            if (hit) { q.engage = { w }; return q; }
        }
        return q;
    };
    auto sweepWithFallback = [&] (sweep::Plan q, const juce::String& prefix, sweep::ProcessOut& r, std::vector<sweep::ProcessOut>& ps) -> sweep::Plan
    {
        sweepFor (q, prefix, r, ps);
        if (windowSeen || overBudget()) return q;
        auto first = sweep::derive (sweep::mergeProcesses (r, ps), q.testLevels(), q.ratioIndex, q.quietReference);
        // THE ENGAGE SEARCH RUNS IN PROFILE MODE TOO (2 Oct): until today the quiet-reference early return above this
        // block skipped it for every profile sweep, so a product flat at its defaults got no search and no record of one.
        if (sweep::engageSignature (first) && q.engage.empty())
        {
            q = engageSearch (q, prefix);
            if (q.engage.empty())                                                // stays pass-through; the tried list rides the fixture
            {
                // THE INERT CHECK (4 Oct): does ANY control move the output? V76U73 here read byte-identical output with
                // Mode, Gain, Makeup +24, Trim +18 and Power Off - "flat" was the wrong word for a product whose processing never runs
                if (! windowSeen && ! overBudget())
                {
                    juce::StringArray planSets; for (auto [i, v] : q.sets) planSets.add (juce::String (i) + ":" + juce::String (v, 6));
                    const auto ir = runInertCheck (base, q.thr, q.norms.front(), planSets, [&] (const juce::String& tag, const juce::StringArray& a) { return runProbe (stem, prefix + tag, a, -1.0f); });
                    q.inertTried = ir.tried; q.inertControlDb = ir.controlDb; q.inert = ir.inert; q.inertReason = ir.reason;
                    std::cout << "  inert check: " << ir.reason << std::endl;
                }
                return q;
            }
            std::cout << "  engage verified: " << q.engage.front().name << " -> " << q.engage.front().norm << "; full sweep with the write" << std::endl;
            sweepFor (q, "e" + juce::String (q.engage.front().index) + "." + prefix, r, ps);
            if (windowSeen || overBudget()) return q;
            first = sweep::derive (sweep::mergeProcesses (r, ps), q.testLevels(), q.ratioIndex, q.quietReference);
        }
        if (q.quietReference) return q;                                          // already on the quiet reference: no fallback to make
        if (! sweep::needsQuietFallback (first)) return q;
        std::cout << "  soft end has no linear anchor (" << first.reason << "): re-sweeping with the quiet-level reference" << std::endl;
        q.quietReference = true;
        q.referenceFallbackNote = "quiet-level reference used because the soft end had no linear anchor: " + first.reason;
        sweepFor (q, "q." + (q.engage.empty() ? juce::String() : "e" + juce::String (q.engage.front().index) + ".") + prefix, r, ps);
        return q;
    };
    // GRID REFINEMENT (2 Oct, ruled): after the sweep (and its engage / quiet fallback) a profile plan's 2 dB points are
    // read; wherever adjacent ones differ by more than kRefineGapDb, positions are added between them (sweep::refineNorms)
    // and measured with the SAME procedure - same preconditions and engage writes, quiet ladder, and the hold-doubled
    // repeat - under the same tag prefix with the next indices (pos16, pos17 ...), so re-derivation reads them as
    // positions like any other. Rounds until no gap is over the bar, or the round / position caps.
    auto refineGrid = [&] (sweep::Plan& q, const juce::String& prefix, sweep::ProcessOut& r, std::vector<sweep::ProcessOut>& ps)
    {
        if (! q.profile) return;
        juce::StringArray levelList; for (double L : q.probeLevels()) levelList.add (juce::String ((int) L));
        auto args = [&] (const sweep::Plan& pl, float norm) {
            juce::StringArray a { "--sweep", "thr=" + juce::String (pl.thr), "norms=" + juce::String (norm, 6), "levels=" + levelList.joinIntoString (","), "hz=997",
                                  "hold=" + juce::String (pl.holdS, 2), "discard=" + juce::String (pl.discardS, 2), "win=" + juce::String (pl.winS, 2), "ref=0", "moving_db=0.1",
                                  juce::String ("reset=") + (opt.resetPerHold ? "1" : "0") };
            juce::StringArray all = sets; for (const auto& w : q.engage) all.add (juce::String (w.index) + ":" + juce::String (w.norm, 6));
            if (pl.pairIndex >= 0) all.add (juce::String (pl.pairIndex) + ":" + juce::String (norm, 6));
            if (! all.isEmpty()) a.add ("set=" + all.joinIntoString (","));
            return a; };
        for (int round = 1; round <= sweep::kRefineRounds && ! windowSeen && ! overBudget(); ++round)
        {
            const auto d = sweep::derive (sweep::mergeProcesses (r, ps), q.testLevels(), q.ratioIndex, q.quietReference);
            std::vector<juce::var> two; for (const auto& g : d.inAtGr) two.push_back (g.at.count (2) ? g.at.at (2) : juce::var());
            // THE 1 dB CURVE TOO (4 Oct, DSM V3): where the 2 dB point is not reached the 1 dB gaps still count - the export needs nine
            // 1 dB positions, and a unit whose top range never reaches 2 dB (DSM V3: 8 of 19) can only get them from the 1 dB gaps
            std::vector<juce::var> one; for (const auto& g : d.inAtGr) one.push_back (g.at.count (1) ? g.at.at (1) : juce::var());
            auto added = sweep::refineNorms (d.norms, two, sweep::kRefineGapDb, sweep::kRefinePositionsMax);
            for (float n : sweep::refineNorms (d.norms, one, sweep::kRefineGapDb, sweep::kRefinePositionsMax)) { bool dup = false; for (float x : added) dup = dup || std::abs (x - n) < 1e-6f; if (! dup) added.push_back (n); }
            if (added.empty()) break;
            std::cout << "  grid refinement round " << round << ": " << (int) added.size() << " position(s) added where adjacent 2 dB points differ by more than "
                      << sweep::kRefineGapDb << " dB" << std::endl;
            q.refineRounds = round;
            const sweep::Plan slow = q.holdDoubled();
            for (float n : added)
            {
                if (windowSeen || overBudget()) break;
                const auto k = (int) q.norms.size();
                q.norms.push_back (n); q.refinedNorms.push_back (n);
                const auto tag = prefix + "pos" + juce::String (k).paddedLeft ('0', 2);
                const auto pr = runProbe (stem, tag, args (q, n), n);
                ps.push_back ({ pr.out, pr.cleanExit(), pr.describe(), n });
                if (q.repeats > 1)
                {
                    const auto rr = runProbe (stem, "r2." + tag, args (slow, n), n);
                    repeatRuns[prefix].push_back ({ rr.out, rr.cleanExit(), rr.describe(), n });
                }
            }
        }
    };
    if (plan.candidates.empty())
    {
        plan = sweepWithFallback (plan, "", refOut, posOut);
        refineGrid (plan, lastSweepPrefix, refOut, posOut);
    }
    else
    {
        // RULE 1 AT PLAN TIME (ruled 2 Oct, evening): the comp-worded candidate first, alone; the rest only on a miss.
        const auto r1 = sweep::ruleOnePick (plan.candidates);
        std::vector<sweep::Plan::Candidate> order;
        if (r1) order.push_back (plan.candidates[(size_t) *r1]);
        for (int i = 0; i < (int) plan.candidates.size(); ++i) if (! r1 || i != *r1) order.push_back (plan.candidates[(size_t) i]);
        if (r1) std::cout << "  RULE 1: '" << plan.candidates[(size_t) *r1].name << "' carries the compressor stage word alone - swept first; the rest only if it does not certify" << std::endl;
        for (size_t k = 0; k < order.size(); ++k)
        {
            const auto& c = order[k];
            if (windowSeen || overBudget()) break;
            auto q = plan.forCandidate (c);
            sweep::applyCandidateControl (q, base);
            // THE PAIR WRITE (ruled 6 Oct): a record decided dual_mono_pair (or review-picked "A + B as a pair") sweeps its pick with the
            // twin written at every position; the store's fixture carries the decision
            if (const auto rd = s.pushed.getProperty ("ruleDecided", {}); rd.getProperty ("pair_with", {}).isObject() && (int) rd.getProperty ("pick", {}).getProperty ("index", -1) == c.index)
            { q.pairIndex = (int) rd.getProperty ("pair_with", {}).getProperty ("index", -1); q.pairName = rd.getProperty ("pair_with", {}).getProperty ("name", "").toString();
              std::cout << "  PAIR WRITE: [" << q.pairIndex << "] " << q.pairName << " is written with [" << q.thr << "] " << q.thrName << " at every position (dual-mono pair)" << std::endl; }
            sweep::ProcessOut cr; std::vector<sweep::ProcessOut> cp;
            q = sweepWithFallback (q, "c" + juce::String (c.index) + ".", cr, cp);
            refineGrid (q, lastSweepPrefix, cr, cp);
            candRuns.push_back ({ q, { cr, cp } });
            if (r1 && k == 0)
            {
                const auto first = sweep::derive (sweep::mergeProcesses (cr, cp), q.testLevels(), q.ratioIndex, q.quietReference);
                if (first.result == "certified") { ruleOneDecided = c; std::cout << "  RULE 1: '" << c.name << "' certifies - the amount control; the other candidates stay at their instantiate values" << std::endl; break; }
                std::cout << "  RULE 1: '" << c.name << "' did not certify (" << first.result << ") - falling back to the full candidate table" << std::endl;
            }
        }
    }
    opt.out.getChildFile (stem + ".processes.json").replaceWithText (juce::JSON::toString (juce::var (processes)) + "\n", false, false, "\n");
    auto fixturesDir = opt.out.getChildFile ("fixtures");
    fixturesDir.createDirectory();
    if (newIdentity)   // the defaults the sweep was planned on, kept whatever happens next
        fixturesDir.getChildFile (fixtureFileName (base).upToLastOccurrenceOf (".json", false, false) + ".defaults.json")
                   .replaceWithText (juce::JSON::toString (base) + "\n", false, false, "\n");
    if (windowSeen)
    {
        // A WINDOW IS RECORDED; ONLY PACE'S IS CALLED A LICENCE FACT (EjmapCertOutcome.h). Said with the stage the probe
        // had reached, so "at the first render" (UAD-2 on this Mac, 29 Sep) reads differently from "on load".
        juce::String stageSeen = "unknown";
        for (int i = processes.size(); --i >= 0;)
            if (processes[i].getProperty ("outcome", "").toString().startsWith ("SHOWED A WINDOW"))
            {
                for (const auto& line : juce::StringArray::fromLines (raw.getChildFile (processes[i].getProperty ("file", "").toString()).loadFileAsString()))
                    if (line.startsWith ("stage\t")) stageSeen = line.fromFirstOccurrenceOf ("\t", false, false);
                stageSeen << " (process " << processes[i].getProperty ("tag", "").toString() << ")";
                break;
            }
        const bool licence = certoutcome::classifyFailure (s.licenceBound, windows) == certoutcome::Failure::unlicensedOnHost;
        return refuse (licence ? 1 : 5, "window", juce::String (licence ? "UNLICENSED ON HOST" : "STOPPED: A WINDOW (recorded, not called a licence fact)")
             + ": a window appeared in the probe's tree (" + windows.joinIntoString (", ") + ") at stage " + stageSeen
             + "; the product was stopped at once, nothing derived");
    }
    if (overBudget())
        return refuse (6, "budget", "stopped after " + juce::String (unclean) + " unclean processes (budget " + juce::String (kUncleanBudget)
                       + ", each already re-run once): " + uncleanWhat.joinIntoString ("; ") + "; nothing derived");

    const auto m = plan.candidates.empty() ? sweep::mergeProcesses (refOut, posOut)
                                           : sweep::mergeProcesses (candRuns.front().second.first, candRuns.front().second.second);
    juce::String pluginArch = m.arch;
    bool bridged = false;
    const auto bundles = componentBundles();
    if (auto it = bundles.find (s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)); it != bundles.end())
    {
        const auto archs = juce::StringArray::fromTokens (bundleArchs (it->second), " ", "");
        if (! archs.isEmpty() && ! archs.contains (m.arch)) { bridged = true; pluginArch = archs.contains ("x86_64") ? "x86_64" : archs[0]; }
    }
    sweep::Provenance pv;
    pv.measuredAt = juce::Time::getCurrentTime().toISO8601 (false);
    pv.host = "EJ Map " + opt.hostVersion + " / " + pluginArch + " / 48k";
    pv.bridged = bridged;
    SweepRunInfo info;
    if (! opt.extraSets.empty())
    {
        auto* arm = new juce::DynamicObject();
        arm->setProperty ("label", opt.armLabel);
        arm->setProperty ("note", "DIAGNOSTIC ARM - a non-swept control was moved on purpose; this is not the product's certification");
        juce::Array<juce::var> xs;
        for (auto [i, v] : opt.extraSets)
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("index", i); x->setProperty ("normalised", v);
            if (m.params.count (i)) x->setProperty ("name", m.params.at (i).first);
            if (m.setTexts.count (i)) x->setProperty ("readBack", m.setTexts.at (i));
            xs.add (juce::var (x));
        }
        arm->setProperty ("sets", xs);
        pv.diagnosticArm = juce::var (arm);
        info.armLine = "DIAGNOSTIC ARM '" + opt.armLabel + "' (not the product's certification)";
    }
    const auto outName = fixtureFileName (base).upToLastOccurrenceOf (".json", false, false) + armTag + ".json";
    info.headline = "EJ Map threshold sweep - " + s.product + " -> " + outName
                    + (s.reach == Subject::Reach::unfixtured ? "  (DISCOVERED: " + s.detail + "; no fixture existed)"
                       : newIdentity ? "  (NEW IDENTITY: installed " + s.desc.version + ", no fixture at that version)" : juce::String());
    info.probeLabel = probeLabel;
    info.processes = processes.size();
    for (const auto& pr : processes)
    {
        if ((int) pr.getProperty ("attempt", 1) == 2) ++info.retried;
        const double sl = (double) pr.getProperty ("slept_ms", 0.0);
        if (sl > 1000.0) { ++info.sleptProcesses; info.sleptMs += sl; }   // over a second: a sleep, not clock jitter
    }
    info.newIdentity = newIdentity;
    // v1.3: the repeat's derivation, when the sweep ran twice (quality lives on the sweep var at composition).
    auto repeatFor = [&] (const sweep::Plan& q, const juce::String& prefix) -> std::optional<sweep::Derived> {
        auto it = repeatRuns.find (prefix); if (it == repeatRuns.end() || it->second.empty()) return std::nullopt;
        return sweep::derive (sweep::mergeProcesses (refOut.out.isEmpty() ? sweep::ProcessOut { juce::String(), true, "none", -1.0f } : refOut, it->second), q.testLevels(), q.ratioIndex, q.quietReference); };
    if (plan.candidates.empty())
    {
        juce::String whyNot;
        // THE REPEAT IS KEYED ON THE LAST FULL SWEEP'S PREFIX (found 4 Oct on API-2500 (m)/(s) in Sean's run): an engage-search sweep
        // ran under "e<idx>." and its hold-doubled repeat was stored under that prefix, but this lookup asked for "" (or "q.") and
        // found nothing - the record said "no repeat" while 37 r2.e6.* captures sat in the traces. lastSweepPrefix is the key.
        const auto rpt = repeatFor (plan, repeatRuns.count (lastSweepPrefix) ? lastSweepPrefix : (plan.quietReference && ! plan.referenceFallbackNote.isEmpty() && repeatRuns.count ("q.") ? juce::String ("q.") : juce::String()));
        if (composeAndReport (base, plan, m, pv, fixturesDir.getChildFile (outName), opt.out.getChildFile (stem + ".report.txt"), info, &whyNot, rpt ? &*rpt : nullptr))
            return 0;
        return refuse (1, "reference", whyNot);
    }
    std::vector<std::pair<sweep::Plan, Derivation>> cands;
    for (const auto& [q, run] : candRuns)
    {
        SweepRunInfo ci = info;
        ci.headline = "CANDIDATE [" + juce::String (q.thr) + "] " + q.thrName + " (flags " + q.thrFlags.joinIntoString (",") + ") - " + s.product
                      + " (class " + plan.cls + ", " + juce::String ((int) plan.candidates.size()) + " candidates; the others at their instantiate defaults)";
        auto one = deriveOne (q, sweep::mergeProcesses (run.first, run.second), pv, ci, outName);
        juce::String pre = "c" + juce::String (q.thr) + ".";
        if (! q.engage.empty()) pre = "e" + juce::String (q.engage.front().index) + "." + pre;
        if (q.quietReference && ! q.referenceFallbackNote.isEmpty() && repeatRuns.count ("q." + pre)) pre = "q." + pre;
        if (auto it = repeatRuns.find (pre); it != repeatRuns.end() && one.written)
        {
            const auto d2 = sweep::derive (sweep::mergeProcesses (run.first, it->second), q.testLevels(), q.ratioIndex, q.quietReference);
            sweep::attachRepeatQuality (one.sweepVar, one.d, &d2);
        }
        else if (one.written) sweep::attachRepeatQuality (one.sweepVar, one.d, nullptr);
        cands.push_back ({ q, one });
    }
    resolveLicenceAtProductLevel (cands, pv);
    juce::var ruleDecided;
    if (ruleOneDecided && ! cands.empty() && cands.front().second.d.result == "certified")
    {
        // THE RECORD SAYS WHAT RULE 1 DECIDED: the pick, its engage write, every other candidate at its instantiate value,
        // and whether the defaults reference shows gain reduction with every candidate at its instantiate value - a stage
        // active at the defaults is IN the exported curve (MDynamics: Processor 1 at -20 dB); which one is not measured
        // individually under the plan-time rule, so the candidates left at a level are named.
        auto* rd = new juce::DynamicObject();
        rd->setProperty ("rule", "R1: exactly one candidate carries the compressor stage word (whole token: comp / compressor / compression) and certifies alone; the others stay at their instantiate values");
        auto* pk = new juce::DynamicObject(); pk->setProperty ("index", ruleOneDecided->index); pk->setProperty ("name", ruleOneDecided->name); rd->setProperty ("pick", juce::var (pk));
        rd->setProperty ("engage", cands.front().second.sweepVar.getProperty ("engageWrites", {}).getProperty ("writes", juce::Array<juce::var>()));
        juce::Array<juce::var> others, atLevel;
        for (const auto& c : plan.candidates)
        {
            if (c.index == ruleOneDecided->index) continue;
            const auto doi = sweep::findControl (base, c.index).getProperty ("defaultOnInstantiate", {});
            auto* o = new juce::DynamicObject(); o->setProperty ("index", c.index); o->setProperty ("name", c.name); o->setProperty ("set", doi.getProperty ("display", "")); o->setProperty ("norm", doi.getProperty ("normalised", juce::var()));
            others.add (juce::var (o));
            const auto txt = doi.getProperty ("display", "").toString().trim().toLowerCase();
            if (txt.isNotEmpty() && txt != "off" && ! txt.startsWith ("-inf") && txt.retainCharacters ("0123456789").isNotEmpty() && txt.getDoubleValue() != 0.0) atLevel.add (c.name + " at " + doi.getProperty ("display", "").toString());
        }
        rd->setProperty ("othersAtInstantiate", others);
        const auto& dg = cands.front().second.d.defaultGain;
        double gmin = 1e9, gmax = -1e9; for (const auto& [k, g] : dg) { gmin = juce::jmin (gmin, g); gmax = juce::jmax (gmax, g); }
        const bool active = ! dg.empty() && gmax - gmin > sweep::kSenseDb;
        rd->setProperty ("defaultsGr_db", dg.empty() ? juce::var() : juce::var (std::round ((gmax - gmin) * 100.0) / 100.0));
        rd->setProperty ("activeAtDefaults", active);
        rd->setProperty ("candidatesLeftAtLevel", atLevel);
        // THE SOURCE OF EVERY WORD (ruled 2 Oct, evening): each stage's own engage switch at instantiate, else "no engage
        // control, not verified"; and the defaults reference's GR as product-level evidence - never a stage reading flat.
        rd->setProperty ("stagesAtDefaults", sweep::stagesAtDefaultsLine (base, plan.candidates, ruleOneDecided->index));
        rd->setProperty ("note", juce::String ("defaults reference (every control at its instantiate value): GR ") + (dg.empty() ? juce::String ("not measured") : juce::String (gmax - gmin, 2) + " dB across the levels")
                                 + (active ? " - above the 1 dB sense bar: a stage is active at the defaults and is IN this curve (which one: not measured individually)" : " - below the 1 dB sense bar")
                                 + "; per stage, from each stage's engage switch at instantiate, never from a stage reading flat");
        ruleDecided = juce::var (rd);
    }
    if (! ruleDecided.isObject()) ruleDecided = decideByMeasurement (cands, opt.out.getChildFile (stem + ".sweep.processes.json"), opt.out.getChildFile ("raw"));
    composeCandidatesAndReport (base, plan, cands, fixturesDir.getChildFile (outName), opt.out.getChildFile (stem + ".report.txt"), ruleDecided);
    return 0;
}

// RE-DERIVE A SWEEP FROM ITS TRACES (--cert-sweep-rederive): nothing is measured. The plan comes from the fixture's
// own controls, the provenance (when, where, bridged, the diagnostic arm) from its existing thresholdSweep, and the
// readings from processes.json and the raw files. A rule change is applied to every past sweep this way.
// WHICH RUN TO RE-DERIVE FROM (1 Oct): the traces may hold the first pass (prefix), an engaged full sweep ("e<idx>." +
// prefix), a quiet-level fallback ("q." + ...), or both. The latest in that order is the run the fixture was composed
// from, and the plan is restored to match: engage writes from the fixture's own `engage` record.
struct TraceRun { juce::String prefix; bool quiet = false; int engageIndex = -1; };
// The repeat pass has positions only (no reference of its own): load them by tag prefix.
inline bool loadRepeatPositions (const juce::File& processesJson, const juce::File& rawDir, const juce::String& prefix, std::vector<sweep::ProcessOut>& positions)
{
    const auto list = juce::JSON::parse (processesJson.loadFileAsString());
    const auto* a = list.getArray(); if (a == nullptr) return false;
    std::map<juce::String, juce::var> last;
    for (const auto& p : *a) last[p.getProperty ("tag", "").toString()] = p;
    positions.clear();
    for (const auto& [tag, p] : last)
        if (tag.startsWith (prefix + "pos"))
            positions.push_back ({ rawDir.getChildFile (p.getProperty ("file", "").toString()).loadFileAsString(), (bool) p.getProperty ("clean", false),
                                   p.getProperty ("outcome", "").toString(), (float) (double) p.getProperty ("norm", -1.0) });
    return ! positions.empty();
}

inline TraceRun resolveTraceRun (const juce::File& processesJson, const juce::String& prefix)
{
    std::set<juce::String> tags;
    const auto list = juce::JSON::parse (processesJson.loadFileAsString());   // held: getArray() on a temporary dangles
    if (const auto* a = list.getArray())
        for (const auto& p : *a) tags.insert (p.getProperty ("tag", "").toString());
    int eidx = -1;
    for (const auto& t : tags)
        if (t.startsWith ("e") && t.endsWith ("." + prefix + "ref") && ! t.startsWith ("eq"))
            eidx = t.substring (1).upToFirstOccurrenceOf (".", false, false).getIntValue();
    TraceRun r;
    const juce::String e = eidx >= 0 ? "e" + juce::String (eidx) + "." : juce::String();
    if (tags.count ("q." + e + prefix + "ref")) { r.prefix = "q." + e + prefix; r.quiet = true; }
    else r.prefix = e + prefix;
    r.engageIndex = eidx;
    return r;
}
inline void restoreEngage (sweep::Plan& q, const juce::var& oldSweep)
{
    const auto eg = oldSweep.getProperty ("engageWrites", {});
    if (const auto* ws = eg.getProperty ("writes", {}).getArray())
        for (const auto& w : *ws)
            q.engage.push_back ({ (int) w.getProperty ("index", -1), w.getProperty ("control", "").toString(),
                                  (float) (double) w.getProperty ("norm", 0.0), w.getProperty ("from", "").toString() });
    if (const auto* tr = eg.getProperty ("tried", {}).getArray())
        for (const auto& t : *tr) q.engageTried.add (t.toString());
}
// THE WRITES THE SWEEP RAN AT (6 Oct, Sean's b0258a7b run): the plan's preconditions (a ratio raise, a make-up zero, a
// mix at wet) were decided at sweep time by the two-sweep test, which a re-derive cannot repeat - so the re-derived plan
// wrote NONE, the export fell back to the instantiate norm, and Zip's tone check wrote its ratio back at 1:1 (49 records
// lost their writes, 7 tone checks failed on it). Restored from the old sweep's own list; where that is already empty (a
// record the 5 Oct follow-up rewrote), from the position traces' `set` lines, which hold every write and its norm.
inline void restoreWrites (sweep::Plan& q, const juce::var& oldSweep, const sweep::Measured& m)
{
    // a candidate's plan already holds the other candidates at their instantiate values: those stay, the rest is added
    std::set<int> have; for (const auto& [idx, n] : q.sets) have.insert (idx);
    if (const auto* pre = oldSweep.getProperty ("preconditions", {}).getArray(); pre != nullptr && ! pre->isEmpty())
    {
        for (const auto& x : *pre)
        {
            const int idx = (int) x.getProperty ("index", -1); if (idx < 0 || have.count (idx)) continue;
            q.sets.push_back ({ idx, (float) (double) x.getProperty ("norm", 0.0) }); have.insert (idx);
            if (x.hasProperty ("role")) q.setRoles[idx] = x.getProperty ("role", "").toString();
        }
    }
    // then the traces, which hold every write that ran (a record the 5 Oct follow-up rewrote keeps only the plan's own
    // auto-make-up write: Spherix's twelve ratio raises were in its traces alone); the engage writes are set lines too and
    // have their own record (restoreEngage): they and the swept control stay out of the preconditions
    std::set<int> engaged; for (const auto& e : q.engage) engaged.insert (e.index);
    for (const auto& [idx, norm] : m.setNorms)
        if (! engaged.count (idx) && ! have.count (idx) && idx != q.thr) { q.sets.push_back ({ idx, norm }); q.setRoles[idx] = "from_trace"; have.insert (idx); }
}
// The grid refinement rides the traces as ordinary positions; what the fixture restores is the record of it.
inline void restoreRefinement (sweep::Plan& q, const juce::var& oldSweep)
{
    const auto g = oldSweep.getProperty ("gridRefinement", {});
    if (! g.isObject()) return;
    q.refineRounds = (int) g.getProperty ("rounds", 0);
    if (const auto* an = g.getProperty ("added_norms", {}).getArray()) for (const auto& n : *an) q.refinedNorms.push_back ((float) (double) n);
}

inline int runSweepRederive (const juce::File& fixtureIn, const juce::File& processesJson, const juce::File& rawDir, const juce::File& fixtureOut)
{
    const auto fx = juce::JSON::parse (fixtureIn.loadFileAsString());
    auto old = fx.getProperty ("thresholdSweep", {});
    // Provenance comes from the first candidate that HAS a sweep: the first may be a licence-suspect with none
    // (MDynamicsMBLarge, whose Band 1 gate is silent at default).
    if (! old.isObject() && fx.getProperty ("thresholdCandidates", {}).isArray())
        for (const auto& c : *fx.getProperty ("thresholdCandidates", {}).getArray())
            if (c.getProperty ("thresholdSweep", {}).isObject()) { old = c.getProperty ("thresholdSweep", {}); break; }
    if (! old.isObject()) { std::cout << "REDERIVE: " << fixtureIn.getFileName() << " has no thresholdSweep" << std::endl; return 2; }
    auto plan = sweep::planFromFixture (fx);
    if (! plan.ok) { std::cout << "REDERIVE: " << plan.why << std::endl; return 4; }
    // The raise, as recorded: the fixture says it ran raised; the value itself is read back from the traces.
    if (old.getProperty ("ratioDuring", {}).hasProperty ("raisedFrom")) plan.ratioRaise = true;
    // A profile sweep is recognised by its 31-level grid, and re-derived as one.
    if (old.getProperty ("tone", {}).getProperty ("levels_dbfs", {}).size() > 3) plan.makeProfile();
    sweep::Provenance pv;
    pv.measuredAt = old.getProperty ("measuredAt", "").toString();
    pv.host = old.getProperty ("host", "").toString();
    pv.bridged = (bool) old.getProperty ("bridged", false);
    pv.diagnosticArm = old.getProperty ("diagnosticArm", {});
    auto base = juce::JSON::parse (juce::JSON::toString (fx));
    if (auto* o = base.getDynamicObject()) { o->removeProperty ("thresholdSweep"); o->removeProperty ("thresholdCandidates"); o->removeProperty ("thresholdReview"); }
    SweepRunInfo info;
    info.headline = "RE-DERIVED from traces (nothing measured) - " + fx.getProperty ("product", "").toString() + " -> " + fixtureOut.getFileName();
    if (! pv.diagnosticArm.isVoid()) info.armLine = "DIAGNOSTIC ARM '" + pv.diagnosticArm.getProperty ("label", "").toString() + "' (not the product's certification)";
    // A "q."-prefixed run in the traces is the quiet-level fallback (ruled 30 Sep) and is the run to derive from.
    auto fallbackNote = [&] (const juce::var& sweepVar) {
        const auto n = sweepVar.getProperty ("linearReference", {}).getProperty ("fallback", "").toString();
        return n.isNotEmpty() ? n : juce::String ("quiet-level reference used because the soft end had no linear anchor (re-derived from the q. traces)"); };
    if (plan.candidates.empty())
    {
        sweep::ProcessOut ref; std::vector<sweep::ProcessOut> pos;
        const auto run = resolveTraceRun (processesJson, "");
        if (run.quiet) { plan.quietReference = true; plan.referenceFallbackNote = fallbackNote (old); }
        if (run.engageIndex >= 0) restoreEngage (plan, old);
        restoreRefinement (plan, old);
        if (! sweep::loadProcesses (processesJson, rawDir, ref, pos, run.prefix)) { std::cout << "REDERIVE: cannot load the traces" << std::endl; return 2; }
        sweep::ProcessOut ref2; std::vector<sweep::ProcessOut> pos2; std::optional<sweep::Derived> rpt;
        if (sweep::loadProcesses (processesJson, rawDir, ref2, pos2, "r2." + run.prefix) || (loadRepeatPositions (processesJson, rawDir, "r2." + run.prefix, pos2)))
            rpt = sweep::derive (sweep::mergeProcesses (ref, pos2), plan.testLevels(), plan.ratioIndex, plan.quietReference);
        const auto merged = sweep::mergeProcesses (ref, pos);
        restoreWrites (plan, old, merged);
        composeAndReport (base, plan, merged, pv, fixtureOut, fixtureOut.getSiblingFile (fixtureOut.getFileNameWithoutExtension() + ".report.txt"), info, nullptr, rpt ? &*rpt : nullptr);
        return 0;
    }
    std::vector<std::pair<sweep::Plan, Derivation>> cands;
    for (const auto& c : plan.candidates)
    {
        auto q = plan.forCandidate (c);
        sweep::applyCandidateControl (q, base);
        sweep::ProcessOut ref; std::vector<sweep::ProcessOut> pos;
        juce::var oldC;
        if (const auto* ocs = fx.getProperty ("thresholdCandidates", {}).getArray()) { for (const auto& x : *ocs) if ((int) x.getProperty ("index", -1) == c.index) oldC = x.getProperty ("thresholdSweep", {}); }
        // a COLLAPSED candidates record (6 Oct): the single sweep on the record is the picked candidate's own view - its detector, writes and
        // refinement carry into that candidate; the other candidates come from their traces alone
        else if ((int) fx.getProperty ("pickedCandidate", {}).getProperty ("index", -1) == c.index) oldC = fx.getProperty ("thresholdSweep", {});
        const auto run = resolveTraceRun (processesJson, "c" + juce::String (c.index) + ".");
        if (const auto pw = oldC.getProperty ("pairWrite", {}); pw.isObject()) { q.pairIndex = (int) pw.getProperty ("index", -1); q.pairName = pw.getProperty ("name", "").toString(); }
        if (run.quiet) { q.quietReference = true; q.referenceFallbackNote = fallbackNote (oldC); }
        if (run.engageIndex >= 0 || oldC.hasProperty ("engageWrites")) restoreEngage (q, oldC);
        restoreRefinement (q, oldC);
        if (! sweep::loadProcesses (processesJson, rawDir, ref, pos, run.prefix)) { std::cout << "REDERIVE: no traces for candidate " << c.index << std::endl; return 2; }
        std::vector<sweep::ProcessOut> pos2; std::optional<sweep::Derived> rpt;
        if (loadRepeatPositions (processesJson, rawDir, "r2." + run.prefix, pos2))
            rpt = sweep::derive (sweep::mergeProcesses (ref, pos2), q.testLevels(), q.ratioIndex, q.quietReference);
        SweepRunInfo ci = info;
        ci.headline = "CANDIDATE [" + juce::String (q.thr) + "] " + q.thrName + " - re-derived";
        const auto mergedC = sweep::mergeProcesses (ref, pos);
        restoreWrites (q, oldC, mergedC);
        auto one = deriveOne (q, mergedC, pv, ci, fixtureOut.getFileName());
        if (one.written) sweep::attachRepeatQuality (one.sweepVar, one.d, rpt ? &*rpt : nullptr);
        cands.push_back ({ q, one });
    }
    resolveLicenceAtProductLevel (cands, pv);
    composeCandidatesAndReport (base, plan, cands, fixtureOut, fixtureOut.getSiblingFile (fixtureOut.getFileNameWithoutExtension() + ".report.txt"),
                                fx.getProperty ("ruleDecided", {}).isObject() ? fx.getProperty ("ruleDecided", {}) : decideByMeasurement (cands, processesJson, rawDir));
    return 0;
}

inline int runCertTuner (const SweepOptions& opt);   // defined below (tuner certification, one store)

// EVERY RUNNABLE PRODUCT (--cert-sweep-all): the census's runnable list, minus what is named in --skip, one
// runCertSweep each. A product's failure is reported and the batch goes on.
// ONE LOOP DOES EVERYTHING (ruled 2 Oct, docs/STRANGER_MAC_TEST.md, rules in EjmapLoop.h). The batch opens with the
// iLok's presence and the census written to <out>/census.txt; every discovered product ends in exactly one state in
// <out>/outcomes.json; a certified profile-grade record goes on through the detector, the export and the tone check
// INSIDE the batch, the tone check embedded in the profile under <out>/profiles/; a product the batch could not
// measure is a row too (held, with which reason). The closing "finish pass" walks every record in the store that has
// no row yet (a batch stopped and resumed, or records from an earlier binary), so a second invocation completes what
// the first left and changes nothing else.
inline constexpr int kToneWindowExit = 5;    // the probe showed a window during the tone check: needs_licence, not a failed check
inline constexpr int kToneLicenceKnownExit = 6;   // the session already knew the product needs a licence that is not present: not loaded
inline constexpr int kToneVersionExit = 7;        // section 11: the installed version is not the record's (or is unknown): not loaded
inline constexpr double kToneLevelByRule = -999.0; // Lrms sentinel: the test L per level comes from profile::toneLevelFor
inline int runDetector (const SweepOptions& opt, const juce::File& recordFile, const juce::String& candidate);
inline int runToneCheck (const SweepOptions& opt, const juce::File& profileFile, const juce::File& recordFile, double Lrms, double g, const juce::String& candidate);
inline int runSweepCensus (const juce::File& fixturesDir, const juce::File& ledgerRoot, bool includePace, bool retryRefused, bool retryAll);
inline juce::String nowStamp() { return juce::Time::getCurrentTime().formatted ("%Y%m%dT%H%M%S"); }
inline juce::String iLokPresence()
{
    const auto r = runChild ({ "/usr/sbin/system_profiler", "SPUSBDataType" }, 30000);
    bool inBlock = false;
    for (auto line : juce::StringArray::fromLines (r.out))
    {
        line = line.trim();
        if (line.startsWith ("iLok")) inBlock = true;
        else if (inBlock && line.startsWith ("Location ID:")) return "present (" + line.fromFirstOccurrenceOf (":", false, false).trim() + ")";
        else if (inBlock && line.endsWith (":") && ! line.contains (" ")) inBlock = false;
    }
    return r.cleanExit() ? "ABSENT" : "unknown (system_profiler " + r.describe() + ")";
}
inline juce::File latestRecordFor (const juce::File& fixturesDir, const juce::String& product)
{
    juce::File best; juce::Time bestT;
    for (const auto& f : fixturesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (f.getFileName().endsWith (".defaults.json")) continue;
        if (juce::JSON::parse (f.loadFileAsString()).getProperty ("product", "").toString() != product) continue;
        if (! best.exists() || f.getLastModificationTime() > bestT) { best = f; bestT = f.getLastModificationTime(); }
    }
    return best;
}
// The after-chain for one record, through the loop's rules: detector (once), export, tone check, the tone check
// embedded in the profile. Returns the row.
inline juce::var finishRecord (const SweepOptions& opt, const juce::File& recordFile, const juce::String& category)
{
    auto record = juce::JSON::parse (recordFile.loadFileAsString());
    const auto identity = record.getProperty ("identity", recordFile.getFileNameWithoutExtension()).toString();
    const auto product = record.getProperty ("product", "").toString();
    { const auto what = loop::applyReviewPick (record, juce::JSON::parse (opt.out.getChildFile ("review_picks.json").loadFileAsString())); if (what.isNotEmpty()) { std::cout << "  " << what << std::endl; if (what.startsWith ("review pick applied")) recordFile.replaceWithText (juce::JSON::toString (record) + "\n", false, false, "\n"); } }
    auto o = loop::outcomeForRecord (record);
    juce::String profilePath, tonePath;
    if (o.exportPending)
    {
        auto profilesDir = opt.out.getChildFile ("profiles"); profilesDir.createDirectory();
        const auto stem = juce::File::createLegalFileName (product).replaceCharacter (' ', '_') + "_" + record.getProperty ("version", "").toString();
        const auto profileFile = profilesDir.getChildFile (stem + ".json");
        // RULE 1: a candidates record with a pick is read through its single view; the detector and the tone check take the pick by name
        const juce::String pick = record.getProperty ("thresholdSweep", {}).isObject() ? juce::String() : record.getProperty ("pickedCandidate", {}).getProperty ("name", "").toString();
        auto single = [&] (const juce::var& r) { if (pick.isEmpty()) return r; juce::String why; auto v = profile::candidateAsSingle (r, pick, why); return v.isVoid() ? r : v; };
        if (! opt.deriveOnly && ! single (record).getProperty ("thresholdSweep", {}).getProperty ("detector", {}).isObject())
        {
            runDetector (opt, recordFile, pick);                                              // writes detector into the record (the pick's sweep), or says why not
            record = juce::JSON::parse (recordFile.loadFileAsString());
        }
        const auto e = profile::exportCompProfile (single (record));
        bool toneRan = false; juce::String toneWhy;
        if (opt.deriveOnly && ! e.ok && e.refused.contains ("detector_f not measured"))
        {
            loop::Outcome w; w.state = "needs_review"; w.reason = "would export in the follow-up: the pick needs its detector measured (one load) and the tone check (derive-only ran neither)";
            return loop::makeRow (identity, product, category, w, recordFile.getFullPathName(), {}, {}, nowStamp());
        }
        if (e.ok)
        {
            profileFile.replaceWithText (juce::JSON::toString (e.profile) + "\n", false, false, "\n");
            profilePath = profileFile.getFullPathName();
            const auto toneFile = profileFile.getSiblingFile (profileFile.getFileNameWithoutExtension() + ".tonecheck.json");
            toneFile.deleteFile();                                                             // a result file is this call's or nobody's
            if (opt.deriveOnly)
            {
                loop::Outcome w; w.state = "needs_review"; w.reason = "export written (derive-only: the tone check was not run, nothing loaded); would be tone-checked by the follow-up";
                return loop::makeRow (identity, product, category, w, recordFile.getFullPathName(), profilePath, {}, nowStamp());
            }
            const int trc = runToneCheck (opt, profileFile, recordFile, kToneLevelByRule, 2.0, pick);
            // THE TONE CHECK RAN when it wrote its result; exit 1 is a FAILED check (a result on the profile), not a check
            // that could not run (exits 2-4 write nothing). Lindell 254E, 11:17: a failed check was read as "could not run".
            if (toneFile.existsAsFile())
            {
                toneRan = true; tonePath = toneFile.getFullPathName();
                auto prof = juce::JSON::parse (profileFile.loadFileAsString());                  // the tone check rides the profile (criterion A5)
                if (auto* po = prof.getDynamicObject()) { po->setProperty ("tone_check", juce::JSON::parse (toneFile.loadFileAsString())); profileFile.replaceWithText (juce::JSON::toString (prof) + "\n", false, false, "\n"); }
            }
            else toneWhy = "tone check exit " + juce::String (trc) + (toneFile.existsAsFile() ? juce::String() : " (no result file)");
            if (trc == kToneVersionExit)
            {
                // SECTION 11: a record from another version (an imported trace set, or a plugin updated since the sweep) is never tone-checked against this install
                loop::Outcome w; w.state = "needs_review"; w.reason = "section 11: the installed version is not the record's " + record.getProperty ("version", "").toString() + " (or is unknown) - the record is not this install's; not loaded, no tone check, the profile is not used across versions";
                return loop::makeRow (identity, product, category, w, recordFile.getFullPathName(), profilePath, {}, nowStamp());
            }
            if (trc == kToneLicenceKnownExit)
            {
                loop::Outcome w; w.state = "needs_licence"; w.reason = "needs a licence the session knows is not present (the scan's stop or an earlier window): not loaded for the tone check; the export stands; --retry-licence re-checks it";
                return loop::makeRow (identity, product, category, w, recordFile.getFullPathName(), profilePath, {}, nowStamp());
            }
            if (trc == kToneWindowExit)
            {
                // A WINDOW DURING THE TONE CHECK is the same evidence as at the scan or the sweep: needs_licence, not needs_review
                loop::Outcome w; w.state = "needs_licence"; w.reason = "window at the probe's load during the tone check (" + toneWhy + "); the export stands, not retried until --retry-licence";
                return loop::makeRow (identity, product, category, w, recordFile.getFullPathName(), profilePath, {}, nowStamp());
            }
        }
        o = loop::outcomeAfterExport (e.ok, e.refused, toneRan, toneWhy, toneRan ? juce::JSON::parse (juce::File (tonePath).loadFileAsString()) : juce::var());
    }
    return loop::makeRow (identity, product, category, o, recordFile.getFullPathName(), profilePath, tonePath, nowStamp());
}

inline int runPreflight (const SweepOptions& opt, const juce::File& executable)
{
    std::cout << "EJ Map pre-flight" << std::endl;
    std::cout << "  executable : " << executable.getFullPathName() << std::endl;
    {
        auto d = runChild ({ "/usr/bin/codesign", "-dvv", executable.getFullPathName() }, 20000);
        juce::String team = "not set", auth;
        for (auto line : juce::StringArray::fromLines (d.out)) { line = line.trim(); if (line.startsWith ("TeamIdentifier=")) team = line.fromFirstOccurrenceOf ("=", false, false); if (line.startsWith ("Authority=Developer ID Application")) auth = line.fromFirstOccurrenceOf ("=", false, false); }
        std::cout << "  app signed : team " << team << (auth.isNotEmpty() ? " (" + auth + ")" : juce::String (" (NOT a Developer ID signature)")) << std::endl;
    }
    const auto id = checkProbe (opt.probe, {}, {});
    std::cout << "  probe      : " << opt.probe.getFullPathName() << (opt.probe == defaultProbeBeside (executable) ? "  (beside the executable: the default)" : "  (NOT the default location)") << std::endl;
    std::cout << "  probe sig  : " << (id.ok ? "team " + id.team + ", cdhash " + id.cdhash : "REFUSED - " + id.why) << std::endl;
    std::cout << "  cert root  : " << opt.out.getFullPathName() << (opt.out == defaultCertRoot() ? "  (default)" : "") << std::endl;
    std::cout << "  store      : " << opt.fixtures.getFullPathName() << std::endl;
    std::cout << "  ledger     : " << opt.ledger.getFullPathName() << (opt.ledger.exists() ? "" : "  (MISSING - scan first)") << std::endl;
    std::cout << "  iLok       : " << iLokPresence() << std::endl;
    std::cout << "  power      : " << runChild ({ "/usr/bin/pmset", "-g", "ps" }, 10000).out.upToFirstOccurrenceOf ("\n", false, false).trim() << std::endl;
    return id.ok ? 0 : 3;
}

// TONE-CHECK-ONLY MODE (v1.7, for the follow-up on a Mac that already ran the batch): from a cert folder's records and
// traces, re-derive every exported record (the deep points 4..12 come out of the same traces), carry over what the traces
// do not hold, re-export, then load the plugin for the tone checks only - g = 2 with the v1.7 pick plus every deep level
// the profile carries. No sweeps. Resumable: a profile whose tone check already carries spec v1.7 and its deep levels is
// skipped. The same window watch: a window during the check is needs_licence; a product the scan stopped is not loaded.
// THE REVIEW SHEET (ruled 4 Oct): for every record still needs_review with candidates, each candidate's verdict and its 2 dB
// curve (norm -> in_at_gr["2"], sine RMS), so a review pick can be made from data and written to cert/review_picks.json.
inline void printReviewSheet (const juce::File& fixturesDir, std::ostream& out)
{
    int products = 0;
    for (const auto& f : fixturesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (f.getFileName().endsWith (".defaults.json")) continue;
        const auto r = juce::JSON::parse (f.loadFileAsString()); const auto cands = r.getProperty ("thresholdCandidates", {});
        if (! cands.isArray() || loop::outcomeForRecord (r).state != "needs_review" || loop::outcomeForRecord (r).exportPending || r.getProperty ("pickedCandidate", {}).isObject()) continue;
        ++products;
        out << "\nREVIEW " << r.getProperty ("product", "").toString() << " (" << r.getProperty ("version", "").toString() << ")  - " << loop::outcomeForRecord (r).reason << "\n";
        for (int i = 0; i < cands.size(); ++i)
        {
            const auto c = cands[i]; const auto sw = c.getProperty ("thresholdSweep", {});
            out << "  [" << (int) c.getProperty ("index", -1) << "] " << c.getProperty ("name", "").toString() << ": " << (sw.isObject() ? sw.getProperty ("result", "").toString() : juce::String ("licence_suspect"));
            if (sw.isObject())
            {
                juce::StringArray pts; const auto norms = sw.getProperty ("positionNorms", {}); const auto ia = sw.getProperty ("inAtGr", {});
                for (int k = 0; k < norms.size() && k < ia.size(); ++k) { const auto v = ia[k].getProperty ("2", {}); pts.add (juce::String ((double) norms[k], 2) + ":" + ((v.isDouble() || v.isInt()) ? juce::String ((double) v - 3.0103, 1) : v.toString())); }
                out << "  2 dB curve (norm:dBFS RMS) " << pts.joinIntoString (" ");
            }
            out << "\n";
        }
    }
    out << "\nREVIEW SHEET: " << products << " product(s) in review with candidates; to pick, add to cert/review_picks.json: {\"product\": \"<name>\", \"candidate\": \"<candidate name>\", \"by\": \"<initials>\", \"date\": \"<YYYY-MM-DD>\"}" << std::endl;
}

// THE ONE READING UNDER THE NEW SIDECHAIN POLICY (ruled 4 Oct; the rule and the pure parts in EjmapSidechainCheck.h).
// Reads the record's own position traces in cert/raw, repeats one of them at one loud level through the probe as it now
// is, compares, and writes `sidechainPolicyCheck` on the record so the next run does not repeat it. Returns whether the
// product is in the set at all, and whether it must be re-swept.
// THE LANDING READ (ruled 4 Oct, Lindell 254E; factored 5 Oct for Sean's stepped rule): the amount control written at 41 norms
// (k/40) in one probe process, each write's read-back recorded; writes that land only on N values make the control stepped
// with those N detents - `amountLanding` on the record. On-grid writes alone prove nothing (detentsFromLanding). Run once
// per record; the follow-up runs it in its decision pass so a re-sweep lands on the detents the first time.
struct LandingRead { bool ran = false, window = false; std::optional<int> detents; juce::String note; };
inline LandingRead readAmountLanding (const SweepOptions& opt, const juce::PluginDescription& desc, juce::var& record, const juce::File& recordFile, int thr)
{
    LandingRead lr;
    juce::StringArray gs; for (int k = 0; k <= 40; ++k) gs.add (juce::String (k / 40.0, 4));
    const auto r = runChild ({ opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId), "--text-at-norms", juce::String (thr), gs.joinIntoString (",") }, opt.timeoutMs);
    if (r.kind == ChildResult::Kind::uiShown) { lr.window = true; lr.note = "a window appeared"; return lr; }
    std::vector<profile::LandingRow> rows;
    if (r.cleanExit())
        for (const auto& line : juce::StringArray::fromLines (r.out))
        {
            const auto f = juce::StringArray::fromTokens (line, "\t", "");
            if (f.size() < 5 || f[0] != "at" || f.indexOf ("getValue") < 0) continue;
            profile::LandingRow row; row.norm = f[1].getDoubleValue(); row.getValue = f[f.indexOf ("getValue") + 1].getDoubleValue(); row.landed = f[2] == "landed"; rows.push_back (row);
        }
    lr.ran = ! rows.empty();
    if (! lr.ran) { lr.note = "not read (" + r.describe() + ")"; return lr; }
    auto* ev = new juce::DynamicObject(); ev->setProperty ("control", thr); ev->setProperty ("readAt", nowStamp());
    juce::Array<juce::var> sm; for (const auto& row : rows) { auto* o = new juce::DynamicObject(); o->setProperty ("norm", row.norm); o->setProperty ("getValue", row.getValue); sm.add (juce::var (o)); } ev->setProperty ("samples", sm);
    if (const auto n = profile::detentsFromLanding (rows))
    {
        lr.detents = n; ev->setProperty ("detents", *n);
        ev->setProperty ("note", "declared continuous; writes land only on " + juce::String (*n) + " values (k/" + juce::String (*n - 1) + ") - stepped by evidence (ruled 4 Oct)");
        lr.note = "stepped by evidence: " + juce::String (*n) + " detents";
    }
    else { ev->setProperty ("detents", 0); ev->setProperty ("note", "continuous: every write landed where it was written (41 norms)"); lr.note = "continuous (every write landed where it was written)"; }
    // ON THE RECORD ON DISK, never the caller's view (6 Oct: the tone check's `record` is the picked candidate's single view, and writing
    // it back collapsed sixteen candidates records on Sean's Mac); the caller's copy gets the evidence too
    record.getDynamicObject()->setProperty ("amountLanding", juce::var (ev));
    { auto disk = juce::JSON::parse (recordFile.loadFileAsString()); if (auto* o = disk.getDynamicObject()) { o->setProperty ("amountLanding", juce::var (ev)); recordFile.replaceWithText (juce::JSON::toString (disk) + "\n", false, false, "\n"); } }
    return lr;
}

// NAMES PROPOSE, MEASUREMENT DECIDES (5 Oct evening): the helpers every prototype mode's role step uses.
// unnamedPool: the numeric controls a name did not nominate (never-touch, word-valued and two-step ones out), capped.
inline constexpr int kUnnamedCap = 40;
struct PoolControl { int index; juce::String name; };
inline std::vector<PoolControl> unnamedPool (const juce::var& base, const std::vector<int>& nominated, const std::set<int>* onlySampled = nullptr)
{
    std::vector<PoolControl> out;
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const int idx = (int) c.getProperty ("index", -1); const auto n = c.getProperty ("name", "").toString();
            if (std::find (nominated.begin(), nominated.end(), idx) != nominated.end()) continue;
            if (onlySampled != nullptr && ! onlySampled->count (idx)) continue;   // ruling 3: only what the text pass sampled is probed
            if (sweep::wordValued (c) || (int) c.getProperty ("numSteps", 0) == 2 || sweep::neverTouchName (n)) continue;
            // meters and readouts are not controls (bx_delay2500's Input VU L: writing it reads as a "mute"); an unnamed parameter is not probed
            const auto l = n.toLowerCase(); if (n.trim().isEmpty() || l.contains ("vu") || l.contains ("meter") || c.getProperty ("readout", {}).isObject() || (bool) c.getProperty ("readoutCheck", false)) continue;
            out.push_back ({ idx, n });
            if ((int) out.size() >= kUnnamedCap) break;
        }
    return out;
}
inline void sayRoles (const std::function<void (const juce::String&)>& say, const std::vector<roleevidence::RoleVerdict>& all, const juce::String& unprobedNote = {})
{
    int notShown = 0; const auto kept = roleevidence::keep (all, notShown);
    say ("  ROLES BY MEASUREMENT: " + juce::String ((int) kept.size()) + " verdict(s)" + (notShown > 0 ? ", " + juce::String (notShown) + " unnamed control(s) probed without the signature" : juce::String()) + (unprobedNote.isNotEmpty() ? "; " + unprobedNote : juce::String()));
    for (const auto& v : kept) say ("    " + roleevidence::line (v));
}
inline void setRoles (juce::DynamicObject* o, const std::vector<roleevidence::RoleVerdict>& all, const juce::String& unprobedNote = {})
{
    int notShown = 0; const auto kept = roleevidence::keep (all, notShown);
    o->setProperty ("roles_by_measurement", roleevidence::toVar (kept)); o->setProperty ("unnamed_probed_without_signature", notShown);
    if (unprobedNote.isNotEmpty()) o->setProperty ("unnamed_not_probed", unprobedNote);
    o->setProperty ("role_rule", "names propose, measurement decides (5 Oct): a nominee keeps its role only when its two ends show the role's signature; an unnamed control that shows it is reported, not swept as the role");
}
// THE TEXT PASS SAMPLES ONLY WHAT THE MODE NEEDS (Kathy's ruling 3, 5 Oct evening; Saturn 2's 951 parameters took 221 s under a
// 120 s timeout). list-params first (0.2 s); the mode NOMINATES on the names alone (its nominate lambda, run on a fixture with
// no text); the unnamed pool is drawn from the same; the text pass then samples nominees + pool in one instantiation
// ("--text-at 3,7,12"), with the timeout scaled to the count actually sampled; the mode nominates again on the sampled fixture.
inline constexpr double kTextSampleMs = 200.0;   // the measured 78 ms per sample (6 run-loop spins) with room; three samples per control
inline int textPassTimeoutMs (int sampled, int floorMs) { return juce::jmax (floorMs, (int) std::lround (sampled * 3.0 * kTextSampleMs) + 30000); }
struct ModeFixture { bool ok = false; juce::String why, note; juce::var base; std::set<int> sampled; int params = 0; int timeoutMs = 0; double textSeconds = 0.0; };
inline ModeFixture sampledFixture (const SweepOptions& opt, const juce::PluginDescription& desc, const juce::File& raw, const juce::String& stem, const juce::String& mode,
                                   const Subject& s, const juce::String& probeNote, const std::vector<const char*>& terms)
{
    // the mode's lexicon nominates on the names list-params gives; its own nomination block then runs on the sampled fixture
    auto nominate = [&] (const juce::var& b) { std::vector<int> idxs; if (const auto* cs = b.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) { const auto n = c.getProperty ("name", "").toString(); for (const char* t : terms) if (nametokens::controlAnswersTerm (n, t)) { idxs.push_back ((int) c.getProperty ("index", -1)); break; } } return idxs; };
    ModeFixture fx;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra, int timeoutMs) { juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, timeoutMs); raw.getChildFile (stem + "." + mode + "." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r; };
    const auto lp = run ("list-params", { "--list-params" }, opt.timeoutMs); if (! lp.cleanExit()) { fx.why = "--list-params " + lp.describe(); return fx; }
    const auto list = parseListParams (lp.out); fx.params = (int) list.size();
    const auto date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    // the names fixture: composeFixture builds controls from text rows, so before the text pass the list rows stand in (index, name, numSteps)
    juce::var base0; { auto* o = new juce::DynamicObject(); juce::Array<juce::var> cs; for (const auto& [i, r] : list) { if (! r.automatable || r.meta) continue; auto* c = new juce::DynamicObject(); c->setProperty ("index", i); c->setProperty ("name", r.name); c->setProperty ("numSteps", r.numSteps); cs.add (juce::var (c)); } o->setProperty ("controls", cs); base0 = juce::var (o); }
    std::set<int> needed; for (int i : nominate (base0)) needed.insert (i);
    for (const auto& pc : unnamedPool (base0, std::vector<int> (needed.begin(), needed.end()))) needed.insert (pc.index);
    if (needed.empty()) { fx.why = "no control to sample (nothing nominated, no numeric pool)"; return fx; }
    juce::StringArray idx; for (int i : needed) idx.add (juce::String (i));
    fx.timeoutMs = textPassTimeoutMs ((int) needed.size(), opt.timeoutMs);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto ta = run ("text-at", { "--text-at", idx.joinIntoString (",") }, fx.timeoutMs);
    fx.textSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    if (! ta.cleanExit()) { fx.why = "--text-at (" + juce::String ((int) needed.size()) + " of " + juce::String (fx.params) + " controls, timeout " + juce::String (fx.timeoutMs / 1000) + " s) " + ta.describe(); return fx; }
    fx.base = composeFixture (s, list, parseTextAt (ta.out), lp.code, ta.code, probeNote, date);
    fx.sampled = needed; fx.ok = true;
    fx.note = "text pass: " + juce::String ((int) needed.size()) + " of " + juce::String (fx.params) + " controls sampled in " + juce::String (fx.textSeconds, 1) + " s (timeout " + juce::String (fx.timeoutMs / 1000) + " s, scaled to the count)";
    return fx;
}
// the fixture's display for a control at (nearest) a norm, before any write - for scaling a window to a label
inline juce::String displayNear (const juce::var& base, int idx, double norm)
{
    const auto c = sweep::findControl (base, idx); const auto at = c.getProperty ("displayAt", {}); juce::String best; double bd = 1e9;
    if (at.isObject()) for (const auto& kv : at.getDynamicObject()->getProperties()) { const double d = std::abs (kv.name.toString().getDoubleValue() - norm); if (d < bd) { bd = d; best = kv.value.toString(); } }
    return bd <= 0.05 ? best : juce::String();
}
// the text at an unnamed control's end, from a run's set lines
inline juce::String setText (const juce::String& out, int idx)
{
    juce::String text; for (const auto& line : juce::StringArray::fromLines (out)) { const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() > 2 && f[0] == "set" && f[1].getIntValue() == idx) for (int i = 2; i + 1 < f.size(); ++i) if (f[i] == "text") text = f[i + 1]; }
    return text;
}

// GAIN / OUTPUT CALIBRATION (roadmap 2.1; PROTOTYPE, 5 Oct overnight B1; the derivation in EjmapGainCal.h). One product:
// its gain-role controls (output, makeup, input when not the amount control, and names answering trim / gain / level) each
// at 21 norms, -20 then -40 dBFS, everything else as instantiated; writes cert/gaincal/<identity>.gaincal.json. Nothing
// exported, nothing published. Returns 0 when at least one control was measured.
inline int runGainCal (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("GAINCAL: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("GAINCAL: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("GAINCAL: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("gaincal"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs);
        raw.getChildFile (stem + ".gaincal." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n");
        return r;
    };
    // the controls, from the plugin itself (list-params + text-at), roled with the compressor lexicon
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "gaincal", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "gain", "output", "makeup", "make-up", "trim", "level", "input", "threshold", "thresh" });
    if (! fx.ok) { say ("GAINCAL: " + fx.why); return 1; }
    const auto base = fx.base; say ("GAINCAL: " + fx.note);
    const auto plan = sweep::planFromFixture (base);
    std::vector<roles::NamedControl> named;
    if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(), false });
    const auto cl = roles::classify (named, roles::Category::compressor);
    struct Target { int index; juce::String name, role; };
    std::vector<Target> targets;
    for (const auto& r : cl.controls)
    {
        if (r.index == plan.thr) continue;                                              // the amount control is the sweep's business
        bool cand = false; for (const auto& c : plan.candidates) if (c.index == r.index) cand = true; if (cand) continue;
        const auto ctl = sweep::findControl (base, r.index);
        if (sweep::wordValued (ctl) || (int) ctl.getProperty ("numSteps", 0) == 2) continue;
        if (r.reason.startsWith ("veto")) continue;                                      // "EQ Gain" (filter), "Gain Reduction" (meter): the roles' vetoes stand
        if (r.role == "output" || r.role == "makeup" || r.role == "input") targets.push_back ({ r.index, r.name, r.role });
        else if (r.role.isEmpty() && (nametokens::controlAnswersTerm (r.name, "trim") || nametokens::controlAnswersTerm (r.name, "gain") || nametokens::controlAnswersTerm (r.name, "level")) && ! sweep::neverTouchName (r.name))
            targets.push_back ({ r.index, r.name, "gain" });
    }
    say ("GAINCAL: " + opt.product + " " + desc.version + ": " + juce::String ((int) targets.size()) + " gain-role control(s)" + (plan.thr >= 0 ? " (amount [" + juce::String (plan.thr) + "] " + plan.thrName + " excluded)" : juce::String()));
    juce::StringArray norms; for (int k = 0; k < gaincal::kNorms; ++k) norms.add (juce::String ((float) k / (float) (gaincal::kNorms - 1), 6));
    juce::Array<juce::var> controls; int measured = 0;
    std::vector<roleevidence::RoleVerdict> roles; std::vector<int> nominated; for (const auto& t : targets) nominated.push_back (t.index); if (plan.thr >= 0) nominated.push_back (plan.thr);
    for (const auto& t : targets)
    {
        std::vector<std::vector<gaincal::Reading>> runs;
        for (double L : t.role == "input" ? gaincal::kInputLevelsDbfs : gaincal::kLevelsDbfs)   // inputs get -60 too (R8c)
        {
            const auto r = run ("c" + juce::String (t.index) + ".L" + juce::String ((int) -L), { "--sweep", "thr=" + juce::String (t.index), "norms=" + norms.joinIntoString (","), "levels=" + juce::String ((int) L), "hz=997", "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" });
            if (r.kind == ChildResult::Kind::uiShown) { say ("GAINCAL: a window appeared on [" + juce::String (t.index) + "] " + t.name + "; stopping"); return 5; }
            runs.push_back (gaincal::parseLevelRun (r.cleanExit() ? r.out : juce::String(), L));
        }
        const auto rows = gaincal::mergeLevels (runs);
        const auto c = gaincal::judge (rows, sweep::findControl (base, t.index).getProperty ("unit", "").toString(), t.role == "input" ? gaincal::kInputRefDbfs : -40.0);
        if (c.measuredPoints > 0) ++measured;
        {   // the role's signature on the control's ends: the level moves, the same at the two levels measured
            roleevidence::Figure fa, fb; const double ref = t.role == "input" ? gaincal::kInputRefDbfs : -40.0, quiet = t.role == "input" ? -40.0 : -20.0;
            for (const auto& r : rows) { roleevidence::Figure* f = r.norm < 0.01f ? &fa : r.norm > 0.99f ? &fb : nullptr; if (f && r.landed && r.measuredDb.count (ref)) { f->ok = true; f->levelDb = r.measuredDb.at (ref); if (r.measuredDb.count (quiet)) f->levelDbQuiet = r.measuredDb.at (quiet); } }
            roles.push_back (roleevidence::nominee (t.index, t.name, t.role.isEmpty() ? "gain" : t.role, roleevidence::signatureHolds ("gain", fa, fb)));
        }
        say ("  [" + juce::String (t.index) + "] " + t.name + " (" + t.role + "): " + c.verdict + " - " + c.note);
        controls.add (gaincal::toVar (t.index, t.name, t.role, rows, c));
    }
    // the unnamed pool: one process per control at its two ends, -40 and -60 dBFS (below any compression path)
    for (const auto& pc : unnamedPool (base, nominated, &fx.sampled))
    {
        const auto r = run ("u" + juce::String (pc.index), { "--sweep", "thr=" + juce::String (pc.index), "norms=0,1", "levels=-60,-40", "hz=997", "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" });
        if (r.kind == ChildResult::Kind::uiShown) { say ("GAINCAL: a window appeared on [" + juce::String (pc.index) + "] " + pc.name + "; stopping"); return 5; }
        const auto m = sweep::parseSweep (r.cleanExit() ? r.out : juce::String());
        roleevidence::Figure fa, fb;
        for (const auto& pos : m.positions) { roleevidence::Figure* f = pos.norm < 0.01f ? &fa : pos.norm > 0.99f ? &fb : nullptr; if (! f) continue;
            for (const auto& [lk, h] : pos.holds) { if (! h.present || h.levelDb < -200.0) continue; const double g = h.levelDb - h.inRmsDb; if (std::abs (lk.getDoubleValue() + 40.0) < 0.1) { f->ok = true; f->levelDb = g; } else if (std::abs (lk.getDoubleValue() + 60.0) < 0.1) f->levelDbQuiet = g; } }
        roles.push_back (roleevidence::unnamed (pc.index, pc.name, "gain", roleevidence::signatureHolds ("gain", fa, fb)));
    }
    sayRoles (say, roles);
    auto* o = new juce::DynamicObject();
    setRoles (o, roles);
    o->setProperty ("schema", "ej_gaincal_prototype/0"); o->setProperty ("status", "PROTOTYPE - roadmap 2.1, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version);
    o->setProperty ("signal", "997 Hz sine at -20 and -40 dBFS peak, 1.5 s hold, 21 norms per control, everything else as instantiated");
    o->setProperty ("measuredAt", nowStamp()); o->setProperty ("controls", controls);
    outDir.getChildFile (stem + ".gaincal.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("GAINCAL: -> " + outDir.getChildFile (stem + ".gaincal.json").getFullPathName());
    return measured > 0 ? 0 : 4;
}

// COMPRESSOR TIMING (roadmap 2.3; PROTOTYPE, 5 Oct overnight B2; derivation in EjmapTiming.h, burst in probe_burst.h).
// From the product's record in <out>/fixtures (its amount control, writes and 1 dB points): the position whose 1 dB point
// (peak) is nearest kTimingAnchorDb is the setting; quiet = that point - 6, loud = + 10 (capped at -3 dBFS). One burst per
// attack position (release as instantiated) and per release position (attack as instantiated), 8 evenly spaced or the
// declared detents; then the release at instantiate from a 0.3 s burst against a 3 s one for program dependence.
inline constexpr double kTimingAnchorDb = -30.0;
inline int runTiming (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("TIMING: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("TIMING: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("TIMING: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    const auto recordFile = latestRecordFor (opt.out.getChildFile ("fixtures"), opt.product);
    auto record = juce::JSON::parse (recordFile.loadFileAsString());
    if (! record.isObject()) { say ("TIMING: no record for " + opt.product + " in " + opt.out.getChildFile ("fixtures").getFullPathName() + " (sweep it first)"); return 2; }
    if (const auto vm = profile::versionMismatch (record.getProperty ("version", "").toString(), desc.version); vm.isNotEmpty()) { say ("TIMING: " + vm); return 2; }
    // the single view: a decided pick, else the single sweep
    juce::String why; const auto pick = record.getProperty ("pickedCandidate", {}).getProperty ("name", "").toString();
    if (pick.isNotEmpty()) { auto v = profile::candidateAsSingle (record, pick, why); if (! v.isVoid()) record = v; }
    const auto sw = record.getProperty ("thresholdSweep", {});
    if (! sw.isObject() || sw.getProperty ("result", "").toString() != "certified") { say ("TIMING: the record's sweep is not certified (" + sw.getProperty ("result", "").toString() + "): nothing to time against"); return 4; }
    auto plan = sweep::planFromFixture (record);
    if (! plan.ok || plan.thr < 0) { say ("TIMING: the record has no single amount control: " + plan.why); return 4; }
    // the setting: the position whose 1 dB point is nearest the anchor
    const auto norms = sw.getProperty ("positionNorms", {}); const auto inAt = sw.getProperty ("inAtGr", {});
    int best = -1; double bestOne = 0.0;
    for (int i = 0; i < norms.size() && i < inAt.size(); ++i) { const auto one = inAt[i].getProperty ("1", {}); if (! (one.isDouble() || one.isInt())) continue; if (best < 0 || std::abs ((double) one - kTimingAnchorDb) < std::abs (bestOne - kTimingAnchorDb)) { best = i; bestOne = (double) one; } }
    if (best < 0) { say ("TIMING: no position reaches 1 dB"); return 4; }
    const double quiet = bestOne - 6.0, loud = juce::jmin (-3.0, bestOne + 10.0);
    juce::StringArray sets; for (const auto& [i, n] : sidechaincheck::recordWrites (sw)) sets.add (juce::String (i) + ":" + juce::String (n, 6));
    sets.add (juce::String (plan.thr) + ":" + juce::String ((double) norms[best], 6));
    say ("TIMING: " + opt.product + " " + desc.version + ": amount [" + juce::String (plan.thr) + "] " + plan.thrName + " at norm " + juce::String ((double) norms[best], 3) + " (1 dB point " + juce::String (bestOne, 2) + " dBFS peak); burst " + juce::String (quiet, 1) + " -> " + juce::String (loud, 1));
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("timing"); outDir.createDirectory();
    const auto stem = recordFile.getFileNameWithoutExtension();
    auto burst = [&] (const juce::String& tag, const juce::StringArray& extraSets, double holdS, double postS) -> std::pair<timing::Timing, ChildResult>
    {
        juce::StringArray all = sets; all.addArray (extraSets);
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId), "--burst", "quiet=" + juce::String (quiet, 2), "loud=" + juce::String (loud, 2),
                                 "pre=1.0", "hold=" + juce::String (holdS, 2), "post=" + juce::String (postS, 2), "hz=997", "win_ms=5", "set=" + all.joinIntoString (",") };
        const auto r = runChild (args, opt.timeoutMs);
        raw.getChildFile (stem + ".timing." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n");
        return { timing::derive (timing::parseBurst (r.cleanExit() ? r.out : juce::String ("refused " + r.describe()))), r };
    };
    std::vector<roleevidence::RoleVerdict> roles;
    // the roles: attack and release controls
    std::vector<roles::NamedControl> named;
    if (const auto* cs = record.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(), false });
    const auto cl = roles::classify (named, roles::Category::compressor);
    juce::Array<juce::var> controls; int measured = 0;
    for (const auto& r : cl.controls)
    {
        if (r.role != "attack" && r.role != "release") continue;
        const auto ctl = sweep::findControl (record, r.index);
        juce::StringArray cn; juce::String positionsBy = "even8"; std::vector<std::pair<float, juce::String>> landed;   // the landing read's norm -> text (R8b reads the label from it)
        if (sweep::isSteppedControl (ctl)) { positionsBy = "declared"; const int n = (int) ctl.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) cn.add (juce::String ((float) k / (float) juce::jmax (1, n - 1), 6)); }
        else
        {
            // DECLARED CONTINUOUS: the control is written at 33 norms and the distinct values it READS BACK are its positions - a
            // snapping control (7X-500's Fast / Medium / Slow, 254E's 100mS / 400mS / 800mS / 1.5S / Auto) gives its detents, a
            // true continuous one gives all 33 and eight evenly spaced positions are used
            juce::StringArray gs; for (int k = 0; k <= 32; ++k) gs.add (juce::String ((float) k / 32.0f, 6));
            const auto tg = runChild ({ opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId), "--text-at-norms", juce::String (r.index), gs.joinIntoString (",") }, opt.timeoutMs);
            const auto det = pitch::detentsFromTextGrid (pitch::parseTextGrid (tg.cleanExit() ? tg.out : juce::String()));
            landed = det;
            if (! det.empty() && (int) det.size() < 33) { positionsBy = "landing"; for (const auto& [n, tx] : det) cn.add (juce::String (n, 6)); }
            else for (int k = 0; k < 8; ++k) cn.add (juce::String ((float) k / 7.0f, 6));
        }
        auto* co = new juce::DynamicObject(); co->setProperty ("index", r.index); co->setProperty ("control", r.name); co->setProperty ("role", r.role); co->setProperty ("positionsBy", positionsBy);
        juce::Array<juce::var> positions;
        // the label at each position BEFORE the burst (the landing read's texts, else the fixture's displayAt nearest the norm), so
        // the loud and post segments can be scaled to it (R8b): hold = 5 x an attack label, post = 5 x a release label, 2 / 4 s at least
        auto textAt = [&] (const juce::String& normText) -> juce::String
        {
            const double n = normText.getDoubleValue();
            for (const auto& [dn, tx] : landed) if (std::abs (dn - n) < 1e-4) return tx;
            const auto at = ctl.getProperty ("displayAt", {}); juce::String best; double bd = 1e9;
            if (at.isObject()) for (const auto& kv : at.getDynamicObject()->getProperties()) { const double d = std::abs (kv.name.toString().getDoubleValue() - n); if (d < bd) { bd = d; best = kv.value.toString(); } }
            return bd <= 0.05 ? best : juce::String();
        };
        timing::Timing firstT, lastT;   // the role's two ends (the first and last positions measured): the signature reads them
        for (int k = 0; k < cn.size(); ++k)
        {
            const auto label = textAt (cn[k]);
            const double holdS = r.role == "attack" ? timing::segmentFor (label, timing::kDefaultHoldS) : timing::kDefaultHoldS;
            const double postS = r.role == "release" ? timing::segmentFor (label, timing::kDefaultPostS) : timing::kDefaultHoldS;
            if (holdS > timing::kDefaultHoldS + 1e-9 || postS > timing::kDefaultPostS + 1e-9) say ("    segments scaled to the label '" + label + "': hold " + juce::String (holdS, 1) + " s, post " + juce::String (postS, 1) + " s");
            auto [t, cr] = burst (r.role + juce::String (r.index) + ".p" + juce::String (k).paddedLeft ('0', 2), { juce::String (r.index) + ":" + cn[k] }, holdS, postS);
            if (cr.kind == ChildResult::Kind::uiShown) { say ("TIMING: a window appeared; stopping"); return 5; }
            juce::String display; for (const auto& line : juce::StringArray::fromLines (cr.out)) { const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() > 2 && f[0] == "set" && f[1].getIntValue() == r.index) { const int tx = f.indexOf ("text"); if (tx >= 0 && tx + 1 < f.size()) display = f[tx + 1]; } }
            auto pv = timing::toVar (t); pv.getDynamicObject()->setProperty ("norm", cn[k].getDoubleValue()); pv.getDynamicObject()->setProperty ("display", display);
            positions.add (pv);
            if (k == 0) firstT = t; if (k == cn.size() - 1) lastT = t;
            if (t.result == "measured" || t.result == "bound") ++measured;
            say ("  " + r.role + " [" + juce::String (r.index) + "] " + r.name + " = " + display + ": " + t.result + (t.attackMs ? " attack " + juce::String (*t.attackMs, 1) + " ms" : t.attackBoundMs ? " attack < " + juce::String (*t.attackBoundMs, 1) + " ms" : juce::String()) + (t.releaseMs ? " release " + juce::String (*t.releaseMs, 1) + " ms" : t.releaseBoundMs ? " release > " + juce::String (*t.releaseBoundMs, 0) + " ms" : juce::String()) + " (GR step " + juce::String (t.stepDb, 2) + ")" + (t.result == "refused" ? " - " + t.reason : juce::String()));
        }
        co->setProperty ("positions", positions); controls.add (juce::var (co));
        {   // ROLES BY MEASUREMENT: the nominee's own figure must move >= 50 % between its first and last positions
            roleevidence::Figure fa, fb;
            // a BOUND at an end counts as its bound (SBC's Attack 0.03 ms reads "< 6.4 ms"; its 30 ms end 48 ms: that is movement)
            auto fill = [] (const timing::Timing& t, roleevidence::Figure& f) { if (t.result != "measured" && t.result != "bound") return; f.ok = true; f.attackMs = t.attackMs ? t.attackMs : t.attackBoundMs; f.releaseMs = t.releaseMs ? t.releaseMs : t.releaseBoundMs; };
            fill (firstT, fa); fill (lastT, fb);
            roles.push_back (roleevidence::nominee (r.index, r.name, r.role, roleevidence::signatureHolds (r.role, fa, fb)));
        }
    }
    // program dependence: the release at instantiate, short burst vs long burst
    auto* pd = new juce::DynamicObject();
    {
        auto [shortT, r1] = burst ("pd.short", {}, 0.3, 4.0);
        auto [longT, r2] = burst ("pd.long", {}, 3.0, 4.0);
        pd->setProperty ("short_burst", timing::toVar (shortT)); pd->setProperty ("long_burst", timing::toVar (longT));
        if (shortT.releaseMs && longT.releaseMs && *shortT.releaseMs > 1.0)
        {
            const double ratio = *longT.releaseMs / *shortT.releaseMs; pd->setProperty ("release_ratio_long_over_short", std::round (ratio * 100.0) / 100.0);
            pd->setProperty ("program_dependent", ratio > timing::kProgramDependentRatio || ratio < 1.0 / timing::kProgramDependentRatio);
            say ("  program dependence: release after 0.3 s " + juce::String (*shortT.releaseMs, 0) + " ms, after 3 s " + juce::String (*longT.releaseMs, 0) + " ms (ratio " + juce::String (ratio, 2) + ")" + ((bool) pd->getProperty ("program_dependent") ? " - PROGRAM-DEPENDENT" : ""));
        }
        else { pd->setProperty ("program_dependent", juce::var()); say ("  program dependence: not decided (" + (shortT.result == "measured" ? longT.reason : shortT.reason) + ")"); }
    }
    sayRoles (say, roles, "unnamed controls not probed in this mode (a burst pair per control costs ~12 s; the roles' lexicon nominates)");
    auto* o = new juce::DynamicObject();
    setRoles (o, roles, "a burst pair per control costs ~12 s: the unnamed pool is not probed in this mode");
    o->setProperty ("schema", "ej_timing_prototype/0"); o->setProperty ("status", "PROTOTYPE - roadmap 2.3, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", record.getProperty ("identity", {}));
    o->setProperty ("amount_control", plan.thrName); o->setProperty ("amount_norm", (double) norms[best]); o->setProperty ("one_db_point_dbfs_peak", bestOne);
    o->setProperty ("quiet_dbfs", quiet); o->setProperty ("loud_dbfs", loud); o->setProperty ("method", "997 Hz sine, 1 s at quiet, step to loud, step back; gain per 5 ms window; attack = 63 % of the GR step, release = 63 % recovery");
    o->setProperty ("controls", controls); o->setProperty ("program_dependence", juce::var (pd)); o->setProperty ("measuredAt", nowStamp());
    outDir.getChildFile (stem + ".timing.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("TIMING: -> " + outDir.getChildFile (stem + ".timing.json").getFullPathName());
    return measured > 0 ? 0 : 4;
}

// LIMITER CEILING ACCURACY (roadmap 2.4; PROTOTYPE, 5 Oct overnight B3; derivation in EjmapLimiter.h). The ceiling control by
// name (ceiling / out ceiling / output ceiling / margin), the amount control from the plan (threshold or input-as-threshold), the
// oversampling switch by name (oversampl / os / true peak), every other control as instantiated; the hard end by measurement.
inline int runLimiter (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("LIMITER: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("LIMITER: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("LIMITER: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("limiter"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs);
        raw.getChildFile (stem + ".limiter." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n");
        return r;
    };
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "limiter", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "ceiling", "margin", "oversampling", "oversample", "os", "threshold", "thresh", "input", "gain", "drive" });
    if (! fx.ok) { say ("LIMITER: " + fx.why); return 1; }
    const auto base = fx.base; say ("LIMITER: " + fx.note);
    const auto plan = sweep::planFromFixture (base);
    int ceilingIdx = -1, osIdx = -1; juce::String ceilingName, osName;
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const int idx = (int) c.getProperty ("index", -1); const auto name = c.getProperty ("name", "").toString();
            if (ceilingIdx < 0 && (nametokens::controlAnswersTerm (name, "ceiling") || nametokens::controlAnswersTerm (name, "margin")) && ! sweep::wordValued (c)) { ceilingIdx = idx; ceilingName = name; }
            if (osIdx < 0 && (int) c.getProperty ("numSteps", 0) == 2 && (name.containsIgnoreCase ("oversampl") || nametokens::controlAnswersTerm (name, "os") || name.containsIgnoreCase ("true peak"))) { osIdx = idx; osName = name; }
        }
    int amount = plan.ok ? plan.thr : -1; juce::String amountName = plan.thrName;
    if (amount < 0 && plan.ok && ! plan.candidates.empty()) { amount = plan.candidates.front().index; amountName = plan.candidates.front().name; }
    if (ceilingIdx < 0)
    {
        juce::StringArray names; if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString());
        say ("LIMITER: " + opt.product + ": no ceiling control by name (ceiling / margin): nothing to judge; controls: " + names.joinIntoString (", ")); return 4;
    }
    if (amount < 0) { say ("LIMITER: " + opt.product + ": no amount control planned (" + plan.why + ")"); return 4; }
    say ("LIMITER: " + opt.product + " " + desc.version + ": ceiling [" + juce::String (ceilingIdx) + "] " + ceilingName + ", amount [" + juce::String (amount) + "] " + amountName + (osIdx >= 0 ? ", oversampling [" + juce::String (osIdx) + "] " + osName : juce::String (", no oversampling switch")));
    // the ceiling's labels at 33 norms -> the positions nearest the targets
    juce::StringArray gs; for (int k = 0; k <= 32; ++k) gs.add (juce::String ((float) k / 32.0f, 6));
    const auto tg = run ("ceiling.textgrid", { "--text-at-norms", juce::String (ceilingIdx), gs.joinIntoString (",") });
    std::vector<std::pair<float, juce::String>> grid;
    for (const auto& row : pitch::parseTextGrid (tg.cleanExit() ? tg.out : juce::String())) grid.push_back ({ (float) row.getValue, row.text });
    const auto positions = limiter::ceilingPositions (grid);
    if (positions.empty()) { say ("LIMITER: the ceiling's labels do not read as dB (" + (grid.empty() ? juce::String ("no grid") : grid.front().second + " .. " + grid.back().second) + ")"); return 4; }
    juce::StringArray sets; for (auto [i, v] : plan.sets) sets.add (juce::String (i) + ":" + juce::String (v, 6));
    auto measure = [&] (const juce::String& tag, float amountNorm, float ceilingNorm, std::optional<float> osNorm)
    {
        juce::StringArray all = sets; all.add (juce::String (ceilingIdx) + ":" + juce::String (ceilingNorm, 6)); if (osNorm) all.add (juce::String (osIdx) + ":" + juce::String (*osNorm, 6));
        const auto r = run (tag, { "--sweep", "thr=" + juce::String (amount), "norms=" + juce::String (amountNorm, 6), "levels=" + juce::String ((int) limiter::kDriveDbfs), "hz=997", "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0", "set=" + all.joinIntoString (",") });
        return std::make_pair (limiter::parsePeaks (r.cleanExit() ? r.out : juce::String()), r);
    };
    // the hard end, by measurement: at the -1 dBFS target (or the first position), which amount end leaves the lower true peak
    const auto& probePos = positions.size() > 2 ? positions[2] : positions.front();
    const auto e0 = measure ("hard.n0", 0.0f, probePos.norm, {}), e1 = measure ("hard.n1", 1.0f, probePos.norm, {});
    if (e0.second.kind == ChildResult::Kind::uiShown || e1.second.kind == ChildResult::Kind::uiShown) { say ("LIMITER: a window appeared; stopping"); return 5; }
    if (! e0.first.ok && ! e1.first.ok) { say ("LIMITER: neither amount end gave a reading (" + e0.second.describe() + " / " + e1.second.describe() + ")"); return 4; }
    // the hard end is where the output is PINNED at the ceiling (its true peak nearest the label), not where it is merely quieter:
    // bx_limiter True Peak's Input Trim at 0.0 is -12 dB of input, output -13 (no limiting); at 1.0 the output sits at the ceiling
    auto dist = [&] (const limiter::PeakReading& r) { return r.ok ? std::abs (r.truePeakDb - probePos.labelDb) : 1e9; };
    const float hard = dist (e0.first) <= dist (e1.first) ? 0.0f : 1.0f;
    say ("  hard end: norm " + juce::String (hard, 1) + " (true peak " + juce::String (e0.first.truePeakDb, 2) + " at 0.0 vs " + juce::String (e1.first.truePeakDb, 2) + " at 1.0; the end pinned at the ceiling " + probePos.display + ")");
    std::vector<limiter::CeilingResult> results;
    std::vector<std::optional<float>> osStates; if (osIdx >= 0) { osStates.push_back (0.0f); osStates.push_back (1.0f); } else osStates.push_back (std::nullopt);
    for (const auto os : osStates)
        for (const auto& cp : positions)
        {
            const auto [pk, r] = measure ("c" + juce::String (cp.labelDb, 1).replace ("-", "m").replace (".", "p") + (os ? (*os > 0.5f ? ".os1" : ".os0") : juce::String()), hard, cp.norm, os);
            limiter::CeilingResult cr; cr.pos = cp; cr.hasOs = os.has_value(); cr.oversampling = os && *os > 0.5f; cr.reading = pk;
            if (pk.ok) { cr.sampleErrDb = pk.peakDb - cp.labelDb; cr.trueErrDb = pk.truePeakDb - cp.labelDb; }
            results.push_back (cr);
            say ("  ceiling " + cp.display + " (" + juce::String (cp.labelDb, 1) + ")" + (os ? (*os > 0.5f ? " OS on " : " OS off") : juce::String()) + ": " + (pk.ok ? "sample peak " + juce::String (pk.peakDb, 2) + " (" + juce::String (cr.sampleErrDb, 2) + "), true peak " + juce::String (pk.truePeakDb, 2) + " (" + juce::String (cr.trueErrDb, 2) + ")" : "no reading (" + r.describe() + ")"));
        }
    // ROLES BY MEASUREMENT (5 Oct evening): the ceiling nominee keeps its role when the output peak moves >= 1 dB between its
    // first and last positions (the first oversampling state); every other numeric control is probed at its ends with the amount
    // hard and the ceiling at the -1 dBFS target (two processes each)
    std::vector<roleevidence::RoleVerdict> roles;
    {
        roleevidence::Figure fa, fb; const limiter::CeilingResult* first = nullptr; const limiter::CeilingResult* last = nullptr;
        for (const auto& cr : results) if (! cr.hasOs || ! cr.oversampling) { if (! first) first = &cr; last = &cr; }
        if (first && first->reading.ok) { fa.ok = true; fa.peakDb = first->reading.peakDb; } if (last && last != first && last->reading.ok) { fb.ok = true; fb.peakDb = last->reading.peakDb; }
        roles.push_back (roleevidence::nominee (ceilingIdx, ceilingName, "ceiling", roleevidence::signatureHolds ("ceiling", fa, fb)));
        std::vector<int> nominated { ceilingIdx, amount }; if (osIdx >= 0) nominated.push_back (osIdx); for (auto [i, v] : plan.sets) nominated.push_back (i);
        for (const auto& pc : unnamedPool (base, nominated, &fx.sampled))
        {
            roleevidence::Figure ua, ub;
            for (float n : { 0.0f, 1.0f })
            {
                juce::StringArray all = sets; all.add (juce::String (ceilingIdx) + ":" + juce::String (probePos.norm, 6)); all.add (juce::String (pc.index) + ":" + juce::String (n, 6));
                const auto r = run ("u" + juce::String (pc.index) + ".n" + juce::String (n, 0), { "--sweep", "thr=" + juce::String (amount), "norms=" + juce::String (hard, 6), "levels=" + juce::String ((int) limiter::kDriveDbfs), "hz=997", "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0", "set=" + all.joinIntoString (",") });
                if (r.kind == ChildResult::Kind::uiShown) { say ("LIMITER: a window appeared; stopping"); return 5; }
                const auto pk = limiter::parsePeaks (r.cleanExit() ? r.out : juce::String()); auto& f = n < 0.5f ? ua : ub; if (pk.ok) { f.ok = true; f.peakDb = pk.peakDb; }
            }
            // a candidate whose peak moved gets the drive test at its 1.0 end: 6 dB less drive - a ceiling holds the peak, a gain passes the change
            if (ua.ok && ub.ok && ua.peakDb && ub.peakDb && std::abs (*ub.peakDb - *ua.peakDb) >= roleevidence::kCeilingMoveDb)
            {
                juce::StringArray all = sets; all.add (juce::String (ceilingIdx) + ":" + juce::String (probePos.norm, 6)); all.add (juce::String (pc.index) + ":1.000000");
                const auto r = run ("u" + juce::String (pc.index) + ".drive", { "--sweep", "thr=" + juce::String (amount), "norms=" + juce::String (hard, 6), "levels=" + juce::String ((int) limiter::kDriveDbfs - 6), "hz=997", "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0", "set=" + all.joinIntoString (",") });
                if (r.kind == ChildResult::Kind::uiShown) { say ("LIMITER: a window appeared; stopping"); return 5; }
                const auto pk = limiter::parsePeaks (r.cleanExit() ? r.out : juce::String()); if (pk.ok) ub.peakDriveDeltaDb = pk.peakDb - *ub.peakDb;
            }
            roles.push_back (roleevidence::unnamed (pc.index, pc.name, "ceiling", roleevidence::signatureHolds ("ceiling", ua, ub)));
        }
        sayRoles (say, roles);
    }
    auto* o = new juce::DynamicObject();
    setRoles (o, roles);
    o->setProperty ("schema", "ej_limiter_prototype/0"); o->setProperty ("status", "PROTOTYPE - roadmap 2.4, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version);
    o->setProperty ("ceiling_control", ceilingName); o->setProperty ("amount_control", amountName); o->setProperty ("amount_hard_norm", hard); o->setProperty ("oversampling_control", osIdx >= 0 ? juce::var (osName) : juce::var());
    o->setProperty ("drive_dbfs_peak", limiter::kDriveDbfs); o->setProperty ("method", "997 Hz sine at -1 dBFS peak, amount at its hard end, ceiling at the labels nearest -0.1/-0.3/-1/-3/-6; output sample peak and 4x cubic true-peak estimate over 0.75 s");
    juce::Array<juce::var> rows;
    for (const auto& r : results) { auto* ro = new juce::DynamicObject(); ro->setProperty ("ceiling_display", r.pos.display); ro->setProperty ("ceiling_db", r.pos.labelDb); ro->setProperty ("norm", r.pos.norm); if (r.hasOs) ro->setProperty ("oversampling", r.oversampling);
        if (r.reading.ok) { ro->setProperty ("out_sample_peak_db", std::round (r.reading.peakDb * 100.0) / 100.0); ro->setProperty ("out_true_peak_db", std::round (r.reading.truePeakDb * 100.0) / 100.0); ro->setProperty ("sample_overshoot_db", std::round (r.sampleErrDb * 100.0) / 100.0); ro->setProperty ("true_overshoot_db", std::round (r.trueErrDb * 100.0) / 100.0); ro->setProperty ("driven", limiter::driven (r)); } else ro->setProperty ("no_reading", true); rows.add (juce::var (ro)); }
    o->setProperty ("ceiling", rows);
    for (const auto os : osStates)
    {
        std::vector<limiter::CeilingResult> sub; for (const auto& r : results) if (! os || r.oversampling == (*os > 0.5f)) sub.push_back (r);
        const auto v = limiter::judge (sub);
        auto* vo = new juce::DynamicObject(); vo->setProperty ("holds_ceiling_sample", v.holdsSample); vo->setProperty ("holds_ceiling_true", v.holdsTrue); vo->setProperty ("worst_sample_overshoot_db", std::round (v.worstSampleDb * 100.0) / 100.0); vo->setProperty ("worst_true_overshoot_db", std::round (v.worstTrueDb * 100.0) / 100.0); vo->setProperty ("note", v.note);
        o->setProperty (os ? (*os > 0.5f ? "verdict_oversampling_on" : "verdict_oversampling_off") : "verdict", juce::var (vo));
        say ("  verdict" + juce::String (os ? (*os > 0.5f ? " (OS on)" : " (OS off)") : "") + ": sample " + (v.holdsSample ? "holds" : "OVER") + ", true peak " + (v.holdsTrue ? "holds" : "OVER") + " - " + v.note);
    }
    o->setProperty ("measuredAt", nowStamp());
    outDir.getChildFile (stem + ".limiter.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("LIMITER: -> " + outDir.getChildFile (stem + ".limiter.json").getFullPathName());
    return 0;
}

// EQ RESPONSE (roadmap 2.2; PROTOTYPE, 5 Oct overnight B4; derivation and band grouping in EjmapEq.h, multitone in probe_response.h).
// Per band: the gain control at 7 norms (freq and Q as instantiated); the frequency control at 7 norms with the gain at the
// position nearest +6 dB (else its top); each Q position at that gain. Every response is read against the band's baseline
// (everything as instantiated) and derived to centre / gain / bandwidth, against the labels.
inline int runEq (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("EQ: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("EQ: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("EQ: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("eq"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false);
    const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs);
        raw.getChildFile (stem + ".eq." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n");
        return r;
    };
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "eq", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "gain", "boost", "cut", "atten", "attenuation", "level", "freq", "frequency", "hz", "khz", "q", "width", "bandwidth", "bw", "shape", "slope", "on", "in", "enable", "active", "bypass" });
    if (! fx.ok) { say ("EQ: " + fx.why); return 1; }
    const auto base = fx.base; say ("EQ: " + fx.note);
    std::vector<std::pair<int, juce::String>> controls;
    if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) controls.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", "").toString() });
    const auto bands = eq::bandsFrom (controls);
    say ("EQ: " + opt.product + " " + desc.version + ": " + juce::String ((int) bands.size()) + " band(s) by name");
    if (bands.empty()) { juce::StringArray names; for (const auto& [i, n] : controls) names.add (n); say ("  controls: " + names.joinIntoString (", ")); return 4; }
    auto normsFor = [&] (int idx, int n) {
        const auto ctl = sweep::findControl (base, idx); juce::StringArray out;
        if (sweep::isSteppedControl (ctl)) { const int st = (int) ctl.getProperty ("numSteps", 0); for (int k = 0; k < st; ++k) out.add (juce::String ((float) k / (float) juce::jmax (1, st - 1), 6)); }
        else for (int k = 0; k < n; ++k) out.add (juce::String ((float) k / (float) (n - 1), 6));
        return out; };
    auto response = [&] (const juce::String& tag, int ctl, const juce::StringArray& norms, const juce::StringArray& sets)
    {
        juce::StringArray a { "--response", "ctl=" + juce::String (ctl), "norms=" + norms.joinIntoString (","), "tones=121", "lo=20", "hi=20000", "db=-12", "hold=1.0", "discard=0.5" };
        if (! sets.isEmpty()) a.add ("set=" + sets.joinIntoString (","));
        const auto r = run (tag, a);
        return std::make_pair (eq::parseResponse (r.cleanExit() ? r.out : juce::String ("refused " + r.describe())), r);
    };
    auto bandVar = [] (const eq::Band& b) { auto* o = new juce::DynamicObject(); o->setProperty ("result", b.result); if (b.reason.isNotEmpty()) o->setProperty ("reason", b.reason); o->setProperty ("shape", b.shape);
        if (b.result == "measured" || b.result == "shelf") { o->setProperty ("centre_hz", std::round (b.centreHz * 10.0) / 10.0); o->setProperty ("gain_db", std::round (b.gainDb * 100.0) / 100.0); }
        if (b.result == "shelf" && b.cornerHz > 0.0) o->setProperty ("corner_hz", std::round (b.cornerHz * 10.0) / 10.0);
        if (b.result == "measured") { o->setProperty ("bandwidth_oct", std::round (b.bandwidthOct * 1000.0) / 1000.0); o->setProperty ("low_3db_hz", std::round (b.lowHz * 10.0) / 10.0); o->setProperty ("high_3db_hz", std::round (b.highHz * 10.0) / 10.0); }
        o->setProperty ("tones", b.tonesUsed); return juce::var (o); };
    // THE SWITCHES (5 Oct R8a): every two-step or word-valued control with its texts, for the band engage search
    std::vector<std::tuple<int, juce::String, bool, std::map<juce::String, float>>> switches; std::vector<int> bandControlIdx;
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            std::map<juce::String, float> texts; if (const auto at = c.getProperty ("displayAt", {}); at.isObject()) for (const auto& kv : at.getDynamicObject()->getProperties()) texts[kv.value.toString()] = (float) kv.name.toString().getDoubleValue();
            switches.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", "").toString(), sweep::wordValued (c) || (int) c.getProperty ("numSteps", 0) == 2, texts });
        }
    for (const auto& b : bands) { bandControlIdx.insert (bandControlIdx.end(), b.gains.begin(), b.gains.end()); bandControlIdx.insert (bandControlIdx.end(), b.freqs.begin(), b.freqs.end()); bandControlIdx.insert (bandControlIdx.end(), b.qs.begin(), b.qs.end()); }
    juce::Array<juce::var> bandRows; int measured = 0, engaged = 0; std::vector<roleevidence::RoleVerdict> roles;
    for (const auto& band : bands)
    {
        const int gIdx = band.gains.front(), fIdx = band.freqs.front();
        const auto tag = "b" + juce::String (gIdx);
        juce::StringArray engageSets; juce::String engagedBy;   // a switch the engage search found for this band: written on every sweep of it
        roleevidence::Figure gFirst, gLast, fFirst, fLast, qFirst, qLast;   // the nominees' ends, for the role step
        say ("  band '" + band.key + "': gain [" + juce::String (gIdx) + "] " + band.gainNames.front() + ", freq [" + juce::String (fIdx) + "] " + band.freqNames.front() + (band.qs.empty() ? juce::String() : ", q [" + juce::String (band.qs.front()) + "] " + band.qNames.front()));
        // baseline: the gain control at its instantiate norm (one position) = everything as instantiated
        auto [baseR, br] = response (tag + ".base", gIdx, { "current" }, {});   // the control at its instantiate value, no write
        if (br.kind == ChildResult::Kind::uiShown) { say ("EQ: a window appeared; stopping"); return 5; }
        if (! baseR.ok || baseR.positions.empty() || baseR.positions[0].tones.size() < 8) { say ("    baseline failed (" + (baseR.refused.isNotEmpty() ? baseR.refused : br.describe()) + ")"); continue; }
        const auto& baseline = baseR.positions[0];
        auto* bo = new juce::DynamicObject(); bo->setProperty ("band", band.key); bo->setProperty ("gain_control", band.gainNames.front()); bo->setProperty ("freq_control", band.freqNames.front()); if (! band.qs.empty()) bo->setProperty ("q_control", band.qNames.front());
        // GAIN sweep
        juce::Array<juce::var> gainRows; float boostNorm = -1.0f; double boostDb = 0.0; std::optional<double> bestBoostDist;
        { auto [gr, r] = response (tag + ".gain", gIdx, normsFor (gIdx, 7), {});
          // THE ENGAGE SEARCH (R8a): flat at every position -> try the band's switches, closest name first, at most four
          { bool allFlat = ! gr.positions.empty(); for (const auto& p : gr.positions) if (eq::deriveBand (eq::deviation (p, baseline)).result != "flat") allFlat = false;
            if (allFlat)
            {
                int tried = 0;
                for (const auto& c : eq::engageCandidates (band.key, switches, bandControlIdx))
                {
                    if (++tried > 4) break;
                    const juce::StringArray sets { juce::String (c.index) + ":" + juce::String (c.onNorm, 6) };
                    auto [gr2, r2] = response (tag + ".gain.e" + juce::String (c.index), gIdx, normsFor (gIdx, 7), sets);
                    if (r2.kind == ChildResult::Kind::uiShown) { say ("EQ: a window appeared; stopping"); return 5; }
                    bool moved = false; for (const auto& p : gr2.positions) if (eq::deriveBand (eq::deviation (p, baseline)).result != "flat") moved = true;
                    say ("    engage search: [" + juce::String (c.index) + "] " + c.name + " -> '" + c.onText + "' (norm " + juce::String (c.onNorm, 2) + "): " + (moved ? "the band now moves" : "still flat"));
                    if (moved) { gr = gr2; engageSets = sets; engagedBy = c.name + " = '" + c.onText + "'"; ++engaged; break; }
                }
                if (engagedBy.isEmpty()) say ("    engage search: " + juce::String (tried) + " switch(es) tried, the band stays flat");
            } }
          { auto fig = [] (const eq::Band& b, const std::vector<std::pair<double, double>>& dev) { roleevidence::Figure f; if (b.result == "measured" || b.result == "shelf") { f.ok = true; f.bandGainDb = b.gainDb; f.centreHz = b.result == "shelf" && b.cornerHz > 0.0 ? b.cornerHz : b.centreHz; if (b.result == "measured") f.bandwidthOct = b.bandwidthOct; } else if (b.result == "flat") { f.ok = true; f.bandGainDb = 0.0; }
                             std::vector<double> ds; for (const auto& [hz, d] : dev) ds.push_back (d); if (! ds.empty()) { std::sort (ds.begin(), ds.end()); f.levelShiftDb = ds[ds.size() / 2]; } return f; };
            if (! gr.positions.empty()) { const auto d0 = eq::deviation (gr.positions.front(), baseline), d1 = eq::deviation (gr.positions.back(), baseline); gFirst = fig (eq::deriveBand (d0), d0); gLast = fig (eq::deriveBand (d1), d1); } }
          for (const auto& p : gr.positions)
          {
              const auto b = eq::deriveBand (eq::deviation (p, baseline));
              auto v = bandVar (b); v.getDynamicObject()->setProperty ("norm", p.norm); v.getDynamicObject()->setProperty ("display", p.text);
              if (const auto lab = eq::labelNumber (p.text)) { v.getDynamicObject()->setProperty ("label_db", *lab); if (b.result == "measured" || b.result == "shelf") v.getDynamicObject()->setProperty ("gain_off_db", std::round ((b.gainDb - *lab) * 100.0) / 100.0); if (! bestBoostDist || std::abs (*lab - 6.0) < *bestBoostDist) { bestBoostDist = std::abs (*lab - 6.0); boostNorm = p.norm; boostDb = *lab; } }
              gainRows.add (v); if (b.result == "measured" || b.result == "shelf") ++measured;
              say ("    gain " + p.text + ": " + b.result + (b.result == "measured" || b.result == "shelf" ? " centre " + juce::String (b.centreHz, 0) + " Hz, gain " + juce::String (b.gainDb, 2) + " dB" + (b.result == "measured" ? ", bw " + juce::String (b.bandwidthOct, 2) + " oct" : " (" + b.shape + ")") : " - " + b.reason));
          }
          if (boostNorm < 0.0f && ! gr.positions.empty()) { boostNorm = gr.positions.back().norm; }
        }
        bo->setProperty ("gain_sweep", gainRows); if (engagedBy.isNotEmpty()) bo->setProperty ("engaged_by", engagedBy);
        // FREQ sweep at the boost
        juce::Array<juce::var> freqRows;
        if (boostNorm >= 0.0f)
        {
            juce::StringArray fsets = engageSets; fsets.add (juce::String (gIdx) + ":" + juce::String (boostNorm, 6));
            auto [fr, r] = response (tag + ".freq", fIdx, normsFor (fIdx, 7), fsets);
            { auto fig = [] (const eq::Band& b) { roleevidence::Figure f; if (b.result == "measured" || b.result == "shelf") { f.ok = true; f.bandGainDb = b.gainDb; f.centreHz = b.result == "shelf" && b.cornerHz > 0.0 ? b.cornerHz : b.centreHz; if (b.result == "measured") f.bandwidthOct = b.bandwidthOct; } else if (b.result == "flat") { f.ok = true; f.bandGainDb = 0.0; } return f; }; if (! fr.positions.empty()) { fFirst = fig (eq::deriveBand (eq::deviation (fr.positions.front(), baseline))); fLast = fig (eq::deriveBand (eq::deviation (fr.positions.back(), baseline))); } }
            for (const auto& p : fr.positions)
            {
                const auto b = eq::deriveBand (eq::deviation (p, baseline));
                auto v = bandVar (b); v.getDynamicObject()->setProperty ("norm", p.norm); v.getDynamicObject()->setProperty ("display", p.text);
                if (const auto lab = eq::labelNumber (p.text)) { v.getDynamicObject()->setProperty ("label_hz", *lab); if ((b.result == "measured" || (b.result == "shelf" && b.cornerHz > 0.0)) && *lab > 0.0) v.getDynamicObject()->setProperty ("centre_off_oct", std::round (std::log2 ((b.result == "shelf" ? b.cornerHz : b.centreHz) / *lab) * 1000.0) / 1000.0); }
                freqRows.add (v); if (b.result == "measured" || b.result == "shelf") ++measured;
                say ("    freq " + p.text + " (gain at " + juce::String (boostDb, 1) + "): " + b.result + (b.result == "measured" ? " centre " + juce::String (b.centreHz, 0) + " Hz, gain " + juce::String (b.gainDb, 2) + ", bw " + juce::String (b.bandwidthOct, 2) + " oct" : b.result == "shelf" ? " " + b.shape + (b.cornerHz > 0.0 ? " corner " + juce::String (b.cornerHz, 0) + " Hz" : juce::String()) + ", plateau " + juce::String (b.gainDb, 2) : " - " + b.reason));
            }
        }
        bo->setProperty ("freq_sweep", freqRows); bo->setProperty ("boost_norm", boostNorm); bo->setProperty ("boost_label_db", boostDb);
        // Q positions at the boost
        juce::Array<juce::var> qRows;
        if (! band.qs.empty() && boostNorm >= 0.0f)
        {
            juce::StringArray qsets = engageSets; qsets.add (juce::String (gIdx) + ":" + juce::String (boostNorm, 6));
            auto [qr, r] = response (tag + ".q", band.qs.front(), normsFor (band.qs.front(), 5), qsets);
            { auto fig = [] (const eq::Band& b) { roleevidence::Figure f; if (b.result == "measured" || b.result == "shelf") { f.ok = true; f.bandGainDb = b.gainDb; f.centreHz = b.result == "shelf" && b.cornerHz > 0.0 ? b.cornerHz : b.centreHz; if (b.result == "measured") f.bandwidthOct = b.bandwidthOct; } else if (b.result == "flat") { f.ok = true; f.bandGainDb = 0.0; } return f; }; if (! qr.positions.empty()) { qFirst = fig (eq::deriveBand (eq::deviation (qr.positions.front(), baseline))); qLast = fig (eq::deriveBand (eq::deviation (qr.positions.back(), baseline))); } }
            for (const auto& p : qr.positions)
            {
                const auto b = eq::deriveBand (eq::deviation (p, baseline));
                auto v = bandVar (b); v.getDynamicObject()->setProperty ("norm", p.norm); v.getDynamicObject()->setProperty ("display", p.text); qRows.add (v);
                say ("    q " + p.text + ": " + b.result + (b.result == "measured" ? " bw " + juce::String (b.bandwidthOct, 2) + " oct (centre " + juce::String (b.centreHz, 0) + ", gain " + juce::String (b.gainDb, 2) + ")" : b.result == "shelf" ? " (" + b.shape + ")" : " - " + b.reason));
            }
        }
        bo->setProperty ("q_sweep", qRows);
        // ROLES BY MEASUREMENT for this band's three nominees: gain = the band's gain moves; frequency = the centre moves with the
        // band staying; q = the bandwidth moves with the centre still (the freq and q sweeps run with the gain boosted)
        roles.push_back (roleevidence::nominee (gIdx, band.gainNames.front(), "eq_gain", roleevidence::signatureHolds ("eq_gain", gFirst, gLast)));
        roles.push_back (roleevidence::nominee (fIdx, band.freqNames.front(), "frequency", roleevidence::signatureHolds ("frequency", fFirst, fLast)));
        if (! band.qs.empty()) roles.push_back (roleevidence::nominee (band.qs.front(), band.qNames.front(), "q", roleevidence::signatureHolds ("q", qFirst, qLast)));
        bandRows.add (juce::var (bo));
    }
    // the unnamed pool: one response process per control at its ends, against the unit's own baseline: a band appearing (>= 1 dB) is the
    // eq_gain signature; frequency and q signatures need a boosted band and are not probed for unnamed controls (said)
    {
        auto [b0, br0] = response ("u.base", bands.front().gains.front(), { "current" }, {});
        if (b0.ok && ! b0.positions.empty())
            for (const auto& pc : unnamedPool (base, bandControlIdx, &fx.sampled))
            {
                auto [ur, r] = response ("u" + juce::String (pc.index), pc.index, { "0.000000", "1.000000" }, {});
                if (r.kind == ChildResult::Kind::uiShown) { say ("EQ: a window appeared; stopping"); return 5; }
                roleevidence::Figure fa, fb; if (ur.positions.size() == 2)
                { auto fig = [] (const std::vector<std::pair<double, double>>& dev) { roleevidence::Figure f; f.ok = true; const auto b = eq::deriveBand (dev); f.bandGainDb = (b.result == "measured" || b.result == "shelf") ? b.gainDb : 0.0;
                                 std::vector<double> ds; for (const auto& [hz, d] : dev) ds.push_back (d); if (! ds.empty()) { std::sort (ds.begin(), ds.end()); f.levelShiftDb = ds[ds.size() / 2]; } return f; };
                  fa = fig (eq::deviation (ur.positions[0], b0.positions[0])); fb = fig (eq::deviation (ur.positions[1], b0.positions[0])); }
                roles.push_back (roleevidence::unnamed (pc.index, pc.name, "eq_gain", roleevidence::signatureHolds ("eq_gain", fa, fb)));
            }
    }
    sayRoles (say, roles, "frequency and q signatures need a boosted band: unnamed controls are probed for the gain signature only");
    auto* o = new juce::DynamicObject();
    setRoles (o, roles, "unnamed controls probed for the gain signature only (frequency / q need a boosted band)");
    o->setProperty ("schema", "ej_eq_prototype/0"); o->setProperty ("status", "PROTOTYPE - roadmap 2.2, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version);
    o->setProperty ("method", "121-tone log multitone 20 Hz-20 kHz at -12 dBFS peak, 1 s hold, 0.5 s discard; deviation against the band's baseline; centre = largest deviation (parabolic in log f), bandwidth = -3 dB span; the grid is 1/12 octave: a centre is known to about 3 %, a bandwidth to about 0.1 octave");
    o->setProperty ("bands", bandRows); o->setProperty ("measuredAt", nowStamp());
    outDir.getChildFile (stem + ".eq.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("EQ: -> " + outDir.getChildFile (stem + ".eq.json").getFullPathName() + (engaged > 0 ? " (" + juce::String (engaged) + " band(s) engaged by a switch the search found)" : juce::String()));
    return measured > 0 ? 0 : 4;
}

// SATURATION: LEVEL CHANGE AND HARMONIC SHARE AGAINST DRIVE (roadmap 2.5; PROTOTYPE, 5 Oct overnight B5). Reuses the sweep
// process as it is: the drive-role control (drive / saturation / sat / color / colour / harmonics / warmth / mix excluded) is the
// swept control at 11 norms, a 997 Hz sine at -20 then -12 dBFS; the hold line's out - in is the level change and its tone_frac
// (the output's power share at the tone) gives the harmonic share: thd_db = 10 log10 ((1 - tone_frac) / tone_frac). Nothing new
// is rendered. Writes cert/saturation/<identity>.saturation.json. Nothing exported, nothing published.
inline int runSaturation (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("SAT: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("SAT: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("SAT: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("saturation"); outDir.createDirectory();
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false); const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra) { juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs); raw.getChildFile (stem + ".saturation." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r; };
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    struct Target { int index; juce::String name; };
    std::vector<Target> targets;
    auto nominate = [&] (const juce::var& b)
    {
        targets.clear(); std::vector<int> idxs;
        if (const auto* cs = b.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs)
            {
                const int idx = (int) c.getProperty ("index", -1); const auto name = c.getProperty ("name", "").toString();
                if (sweep::wordValued (c) || (int) c.getProperty ("numSteps", 0) == 2 || sweep::neverTouchName (name)) continue;
                bool drive = false; for (const char* t : { "drive", "saturation", "sat", "saturate", "color", "colour", "harmonics", "warmth", "heat", "crush", "amount" }) if (nametokens::controlAnswersTerm (name, t)) drive = true;
                if (drive && ! nametokens::controlAnswersTerm (name, "mix")) { targets.push_back ({ idx, name }); idxs.push_back (idx); }
            }
        return idxs;
    };
    const auto fx = sampledFixture (opt, desc, raw, stem, "saturation", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "drive", "saturation", "sat", "saturate", "color", "colour", "harmonics", "warmth", "heat", "crush", "amount" });
    if (! fx.ok) { say ("SAT: " + fx.why); return 1; }
    const auto base = fx.base; nominate (base); say ("SAT: " + fx.note);
    say ("SAT: " + opt.product + " " + desc.version + ": " + juce::String ((int) targets.size()) + " drive-type control(s)");
    if (targets.empty()) { juce::StringArray names; if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString()); say ("  controls: " + names.joinIntoString (", ")); return 4; }
    // THE MEASUREMENT (R3, 5 Oct): one sine at 997 Hz (an exact bin), the drive at 11 norms, harmonics 2..5 read by exact
    // Goertzel bins (probe --response tones=1 harmonics=5), one process per level; EjmapSaturation.h derives the rest
    juce::StringArray norms; for (int k = 0; k < 11; ++k) norms.add (juce::String (k / 10.0f, 6));
    juce::Array<juce::var> controls; int measured = 0, inertN = 0, noEffectN = 0;
    for (const auto& t : targets)
    {
        std::vector<saturation::LevelResult> levels;
        for (double L : { -20.0, -12.0, -6.0 })
        {
            const auto r = run ("c" + juce::String (t.index) + ".L" + juce::String ((int) -L), { "--response", "ctl=" + juce::String (t.index), "norms=" + norms.joinIntoString (","), "tones=1", "lo=997", "harmonics=5", "db=" + juce::String (L, 0), "hold=1.0", "discard=0.5" });
            if (r.kind == ChildResult::Kind::uiShown) { say ("SAT: a window appeared; stopping"); return 5; }
            if (! r.cleanExit()) { say ("SAT: [" + juce::String (t.index) + "] " + t.name + " at " + juce::String (L, 0) + " dBFS: the probe " + r.describe()); continue; }
            const auto resp = saturation::parseHarmonics (r.out);
            if (! resp.ok) { say ("SAT: [" + juce::String (t.index) + "] " + t.name + " at " + juce::String (L, 0) + " dBFS: " + (resp.refused.isNotEmpty() ? "refused " + resp.refused : juce::String ("no response header"))); continue; }
            levels.push_back (saturation::deriveLevel (resp, L));
            for (const auto& rd : levels.back().readings) if (rd.valid) ++measured;
        }
        const auto c = saturation::judge (t.index, t.name, levels);
        if (c.inert || c.silent) ++inertN; if (c.noEffect) ++noEffectN;
        say ("  [" + juce::String (t.index) + "] " + t.name + ": " + c.note);
        for (const auto& L : c.levels) for (const auto& rd : L.readings) if (rd.valid && std::abs (L.levelDbfs + 12.0) < 0.1)
            say ("      @-12 " + rd.text.paddedRight (' ', 10) + " gain " + juce::String (rd.gainDb, 2).paddedLeft (' ', 7) + " dB  THD " + (rd.thdDb > -200.0 ? juce::String (rd.thdDb, 1) + " dB (" + juce::String (rd.thdPct, 3) + " %)" : juce::String ("none"))
                 + "  h2 " + (rd.harmonicDb.count (2) ? juce::String (rd.harmonicDb.at (2), 1) : "-") + " h3 " + (rd.harmonicDb.count (3) ? juce::String (rd.harmonicDb.at (3), 1) : "-") + " h4 " + (rd.harmonicDb.count (4) ? juce::String (rd.harmonicDb.at (4), 1) : "-") + " h5 " + (rd.harmonicDb.count (5) ? juce::String (rd.harmonicDb.at (5), 1) : "-")
                 + (rd.character.isNotEmpty() ? "  " + rd.character + (rd.evenOddKnown ? " (even/odd " + juce::String (rd.evenOddDb, 1) + " dB)" : juce::String()) : juce::String()));
        controls.add (saturation::toVar (c));
    }
    // ROLES BY MEASUREMENT (5 Oct evening): a drive nominee keeps its role when THD moves >= 3 dB between its ends at -12 dBFS
    // (reaching -60 at least); every other numeric control is probed at its two ends with one process and reported if it shows it
    std::vector<roleevidence::RoleVerdict> roles; std::vector<int> nominated;
    for (int i = 0; i < controls.size(); ++i)
    {
        const auto c = controls[i]; const int idx = (int) c.getProperty ("index", -1); nominated.push_back (idx);
        roleevidence::Figure a, b;
        if (const auto* lv = c.getProperty ("levels", {}).getArray()) for (const auto& L : *lv) if (std::abs ((double) L.getProperty ("level_dbfs", 0.0) + 12.0) < 0.1)
            if (const auto* cv = L.getProperty ("curve", {}).getArray()) for (const auto& r : *cv) if ((bool) r.getProperty ("valid", false))
            { const double n = (double) r.getProperty ("norm", 0.0); const auto t = r.getProperty ("thd_db", {}); const auto sb = r.getProperty ("sideband_db", {}); roleevidence::Figure* f = n < 0.01 ? &a : n > 0.99 ? &b : nullptr; if (f) { f->ok = true; f->thdDb = (t.isDouble() || t.isInt()) ? (double) t : -200.0; if (sb.isDouble() || sb.isInt()) f->sidebandDb = (double) sb; } }
        if ((bool) c.getProperty ("silent", false) || (bool) c.getProperty ("inert", false)) { roleevidence::RoleVerdict v; v.index = idx; v.name = c.getProperty ("control", "").toString(); v.role = "drive"; v.verdict = "dropped"; v.reason = c.getProperty ("note", "").toString().upToFirstOccurrenceOf (":", false, false) + ": nothing to decide"; roles.push_back (v); continue; }
        roles.push_back (roleevidence::nominee (idx, c.getProperty ("control", "").toString(), "drive", roleevidence::signatureHolds ("drive", a, b)));
    }
    for (const auto& pc : unnamedPool (base, nominated, &fx.sampled))
    {
        const auto r = run ("u" + juce::String (pc.index), { "--response", "ctl=" + juce::String (pc.index), "norms=0,1", "tones=1", "lo=997", "harmonics=5", "db=-12", "hold=1.0", "discard=0.5" });
        if (r.kind == ChildResult::Kind::uiShown) { say ("SAT: a window appeared; stopping"); return 5; }
        const auto L = saturation::deriveLevel (saturation::parseHarmonics (r.cleanExit() ? r.out : juce::String()), -12.0);
        roleevidence::Figure a, b; for (const auto& rd : L.readings) { roleevidence::Figure* f = rd.norm < 0.01f ? &a : rd.norm > 0.99f ? &b : nullptr; if (f && (rd.valid || rd.silent)) { f->ok = true; f->thdDb = rd.valid && rd.thdDb > -200.0 ? rd.thdDb : -200.0; f->outputDb = rd.valid ? rd.outDb : -999.0; if (rd.valid) f->sidebandDb = rd.sidebandDb; } }
        // ruling 1: energy beside the tone is modulation, reported as such, never as drive
        if (const auto m = roleevidence::modulationOf (a, b); m.holds) { roles.push_back (roleevidence::unnamed (pc.index, pc.name, "modulation", m)); continue; }
        roles.push_back (roleevidence::unnamed (pc.index, pc.name, "drive", roleevidence::signatureHolds ("drive", a, b)));
    }
    sayRoles (say, roles);
    auto* o = new juce::DynamicObject();
    setRoles (o, roles);
    o->setProperty ("schema", "ej_saturation_prototype/1"); o->setProperty ("status", "PROTOTYPE - roadmap 2.5, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version);
    o->setProperty ("method", "one 997 Hz sine (an exact bin) at -20, -12 and -6 dBFS peak, the drive control at 11 norms; per position the fundamental's gain (out - in) and the 2nd-5th harmonics by exact Goertzel bins (probe --response tones=1 harmonics=5): thd_db = 10 log10 (sum of harmonic power / fundamental power), thd_pct its square root x 100, even_odd_db = (h2 + h4) / (h3 + h5) in dB; onset = the first position by norm whose THD reaches 1 % / 0.1 %; inert = fundamental within 0.05 dB and no harmonic above -90 dB everywhere");
    o->setProperty ("controls", controls); o->setProperty ("inert_or_silent_controls", inertN); o->setProperty ("no_effect_controls", noEffectN); o->setProperty ("measuredAt", nowStamp());
    outDir.getChildFile (stem + ".saturation.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("SAT: -> " + outDir.getChildFile (stem + ".saturation.json").getFullPathName());
    return measured > 0 ? 0 : 4;
}

// REVERB AND DELAY PROTOTYPE (roadmap 2.7, 5 Oct R4): --cert-reverb-delay <product> [--kind reverb|delay]. Mix law over 11
// mix positions; decay (reverb) or feedback (delay) at 5 positions against the labels; the time control (pre-delay / delay
// time) at 5 positions against its labels; a tempo-synced delay at 90 / 120 / 140 bpm against the note value. Nothing exported.
inline int runReverbDelay (const SweepOptions& opt, juce::String kind)
{
    using namespace reverbdelay;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("RD: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("RD: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("RD: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false); const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    if (kind.isEmpty()) { const auto in = loadDiscoveryInputs (opt.ledger); const auto it = in.categoryByUid.find ("AudioUnit|" + uidHex); kind = it != in.categoryByUid.end() ? it->second : juce::String(); }
    if (kind != "reverb" && kind != "delay") { say ("RD: '" + opt.product + "' is category '" + kind + "' in the ledger; say --kind reverb or --kind delay"); return 2; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("reverbdelay"); outDir.createDirectory();
    int processN = 0;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra) { juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs); ++processN; raw.getChildFile (stem + ".reverbdelay." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r; };
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "reverbdelay", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "mix", "dry/wet", "wet", "blend", "dry wet", "drywet", "sync", "tempo sync", "note", "division", "subdivision", "beat", "rate", "delay", "time", "pre-delay", "predelay", "pre delay", "pre", "decay", "reverb time", "rt60", "length", "size", "tail", "feedback", "regen", "regeneration", "repeats", "fb" });
    if (! fx.ok) { say ("RD: " + fx.why); return 1; }
    const auto base = fx.base; say ("RD: " + fx.note);
    // THE CONTROLS BY NAME (a prototype's rule, said in the proposal): the first match of each role
    struct Ctl { int index = -1; juce::String name; bool word = false; int steps = 0; double onNorm = 1.0, offNorm = 0.0; juce::String onText, offText; };
    Ctl mix, timeCtl, decayCtl, feedback, sync, note;
    // a switch's ON / OFF positions BY TEXT (H-Delay's Sync is BPM / Host / ms: norm 0 is an internal tempo, not off)
    auto switchPositions = [] (const juce::var& c, Ctl& k)
    {
        const auto at = c.getProperty ("displayAt", {}); if (! at.isObject()) return;
        for (const auto& kv : at.getDynamicObject()->getProperties())
        {
            const auto text = kv.value.toString().toLowerCase(); const double norm = kv.name.toString().getDoubleValue();
            if (text.contains ("host") || text == "on" || text.contains ("sync")) { k.onNorm = norm; k.onText = kv.value.toString(); }
            if (text == "off" || text.contains ("free") || text == "ms" || text.contains ("msec") || text == "time" || text == "sec") { k.offNorm = norm; k.offText = kv.value.toString(); }
        }
    };
    auto noteTexts = [] (const juce::var& c) { int n = 0; const auto at = c.getProperty ("displayAt", {}); if (at.isObject()) for (const auto& kv : at.getDynamicObject()->getProperties()) if (kv.value.toString().contains ("/")) ++n; return n; };
    auto answers = [] (const juce::String& name, std::initializer_list<const char*> terms) { for (const char* t : terms) if (nametokens::controlAnswersTerm (name, t)) return true; return false; };
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            Ctl k; k.index = (int) c.getProperty ("index", -1); k.name = c.getProperty ("name", "").toString(); k.word = sweep::wordValued (c); k.steps = (int) c.getProperty ("numSteps", 0);
            const auto n = k.name;
            if (mix.index < 0 && ! k.word && answers (n, { "mix", "dry/wet", "wet", "blend", "dry wet", "drywet" }) && ! answers (n, { "sync", "lock" })) mix = k;
            else if (sync.index < 0 && answers (n, { "sync", "tempo sync" }) && (k.word || k.steps == 2)) { sync = k; switchPositions (c, sync); }
            else if (note.index < 0 && (answers (n, { "note", "division", "subdivision", "beat" }) || noteTexts (c) >= 2)) note = k;
            else if (timeCtl.index < 0 && ! k.word && (kind == "delay" ? answers (n, { "delay", "time", "delay time", "left delay", "delay l", "delay left" }) : answers (n, { "pre-delay", "predelay", "pre delay", "pre" })) && ! answers (n, { "sync", "feedback", "decay", "mod", "modulation" })) timeCtl = k;
            else if (decayCtl.index < 0 && kind == "reverb" && ! k.word && answers (n, { "decay", "reverb time", "rt60", "time", "length", "size", "tail" }) && ! answers (n, { "pre", "mod", "modulation", "damp" })) decayCtl = k;
            else if (feedback.index < 0 && kind == "delay" && ! k.word && answers (n, { "feedback", "regen", "regeneration", "repeats", "fb" })) feedback = k;
        }
    auto named = [] (const Ctl& c) { return c.index < 0 ? juce::String ("-") : "[" + juce::String (c.index) + "] " + c.name; };
    say ("RD: " + opt.product + " " + desc.version + " (" + kind + "): mix " + named (mix) + "; " + (kind == "delay" ? "time " + named (timeCtl) + "; feedback " + named (feedback) + "; sync " + named (sync) + (sync.index >= 0 ? " (off = '" + sync.offText + "' @" + juce::String (sync.offNorm, 2) + ", on = '" + sync.onText + "' @" + juce::String (sync.onNorm, 2) + ")" : juce::String()) + "; note " + named (note) : "pre-delay " + named (timeCtl) + "; decay " + named (decayCtl)));
    if (mix.index < 0 && timeCtl.index < 0 && decayCtl.index < 0 && feedback.index < 0) { juce::StringArray names; if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString()); say ("  controls: " + names.joinIntoString (", ")); return 4; }
    const juce::String burstMs = kind == "delay" ? "50" : "200";
    // THE WINDOW SCALED TO THE DECAY LABEL (5 Oct evening, item 2): a decay run's tail is at least 1.5 x the label in seconds
    // (6 s at least, 30 s at most); the label is read from the fixture's display nearest the norm before the write
    auto tailFor = [&] (double labelS) { return tailForLabel (labelS); };
    auto tailArgs = [&] (const juce::StringArray& sets, double tempo = 0.0, double tailS = 6.0) { juce::StringArray a { "--tail", "db=-12", "burst_ms=" + burstMs, "tail_s=" + juce::String (tailS, 1), "hz=997", "win_ms=1" }; if (tempo > 0.0) a.add ("tempo=" + juce::String (tempo, 0)); if (! sets.isEmpty()) a.add ("set=" + sets.joinIntoString (",")); return a; };
    auto setOf = [] (int idx, double norm) { return juce::String (idx) + ":" + juce::String (norm, 6); };
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_reverb_delay_prototype/0"); o->setProperty ("status", "PROTOTYPE - roadmap 2.7, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version); o->setProperty ("kind", kind);
    auto ctlVar = [] (const Ctl& c) { if (c.index < 0) return juce::var(); auto* x = new juce::DynamicObject(); x->setProperty ("index", c.index); x->setProperty ("name", c.name); return juce::var (x); };
    o->setProperty ("mix_control", ctlVar (mix)); o->setProperty ("time_control", ctlVar (timeCtl)); o->setProperty ("decay_control", ctlVar (decayCtl)); o->setProperty ("feedback_control", ctlVar (feedback)); o->setProperty ("sync_control", ctlVar (sync)); o->setProperty ("note_control", ctlVar (note));
    int measured = 0;
    std::vector<roleevidence::RoleVerdict> roles; roleevidence::Figure mixA, mixB, timeA, timeB, decayA, decayB, fbA, fbB;   // the nominees' ends
    // the wet-only preconditions: mix at 1 (a delay's dry would otherwise hide the first repeat inside the burst)
    juce::StringArray wetOnly; if (mix.index >= 0) wetOnly.add (setOf (mix.index, 1.0));
    if (sync.index >= 0) wetOnly.add (setOf (sync.index, sync.offNorm));   // the time control's own units for the baseline and the mix law
    // 1. THE WET-ONLY BASELINE: onset (pre-delay / delay time at the instantiate position), decay, repeats
    std::optional<double> baseOnsetMs;
    {
        const auto r = run ("wet", tailArgs (wetOnly));
        if (r.kind == ChildResult::Kind::uiShown) { say ("RD: a window appeared; stopping"); return 5; }
        const auto t = parseTail (r.cleanExit() ? r.out : juce::String());
        if (! t.ok) say ("  wet-only baseline: " + (t.refused.isNotEmpty() ? "refused " + t.refused : r.describe()));
        else
        {
            ++measured;
            const auto on = onsetsOf (t); const auto dc = decayOf (t); baseOnsetMs = on.onsetMs;
            say ("  wet-only (mix " + (mix.index >= 0 ? juce::String ("1.0") : juce::String ("as instantiated")) + "): onset " + (on.onsetMs ? juce::String (*on.onsetMs, 1) + " ms" : juce::String ("none above the floor")) + "; " + on.why
                 + (on.spacingMs ? "; repeats every " + juce::String (*on.spacingMs, 1) + " ms falling " + juce::String (*on.fallPerRepeatDb, 2) + " dB each" : juce::String())
                 + "; decay " + (dc.ok ? "RT60 " + juce::String (dc.t20RT60s, 2) + " s (T20)" + (dc.t30 ? ", " + juce::String (dc.t30RT60s, 2) + " s (T30)" : juce::String ("")) : dc.why) + "; floor " + juce::String (on.floorDb, 1) + " dB");
            auto* b = new juce::DynamicObject(); b->setProperty ("onset_ms", on.onsetMs ? juce::var (*on.onsetMs) : juce::var()); b->setProperty ("repeats", (int) on.repeats.size()); b->setProperty ("repeat_spacing_ms", on.spacingMs ? juce::var (*on.spacingMs) : juce::var()); b->setProperty ("fall_per_repeat_db", on.fallPerRepeatDb ? juce::var (*on.fallPerRepeatDb) : juce::var());
            b->setProperty ("rt60_t20_s", dc.ok ? juce::var (dc.t20RT60s) : juce::var()); b->setProperty ("rt60_t30_s", dc.t30 ? juce::var (dc.t30RT60s) : juce::var()); b->setProperty ("decay_note", dc.why); b->setProperty ("floor_db", on.floorDb);
            o->setProperty ("wet_only_baseline", juce::var (b));
        }
    }
    // 2. THE MIX LAW: eleven mix positions; dry in the burst's first 2 ms, wet at the first repeat (delay: the baseline's onset) or just after the burst (reverb)
    if (mix.index >= 0)
    {
        std::vector<MixPoint> pts; juce::Array<juce::var> rows;
        for (int k = 0; k <= 10; ++k)
        {
            const double norm = k / 10.0;
            const auto r = run ("mix" + juce::String (k), tailArgs ({ setOf (mix.index, norm) }));
            if (r.kind == ChildResult::Kind::uiShown) { say ("RD: a window appeared; stopping"); return 5; }
            const auto t = parseTail (r.cleanExit() ? r.out : juce::String()); if (! t.ok) { say ("  mix " + juce::String (norm, 1) + ": " + (t.refused.isNotEmpty() ? "refused " + t.refused : r.describe())); continue; }
            auto L = levelsOf (t);
            if (kind == "delay" && baseOnsetMs && *baseOnsetMs > t.burstMs)
            {   // the first repeat's head, where the dry has already stopped
                std::vector<double> wet; for (const auto& w : t.windows) if (w.tMs >= *baseOnsetMs && w.tMs < *baseOnsetMs + kWetWindowMs && w.outDb > -500.0) wet.push_back (w.outDb);
                L.wetDb = powerMeanDb (wet);
            }
            MixPoint p; p.norm = (float) norm; p.text = t.setTexts.count (mix.index) ? t.setTexts.at (mix.index) : juce::String(); p.dryDb = L.dryDb; p.wetDb = L.wetDb; pts.push_back (p); ++measured;
            auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", p.text); ro->setProperty ("dry_db", L.dryDb > -500.0 ? juce::var (std::round (L.dryDb * 100.0) / 100.0) : juce::var()); ro->setProperty ("wet_db", L.wetDb > -500.0 ? juce::var (std::round (L.wetDb * 100.0) / 100.0) : juce::var()); rows.add (juce::var (ro));
        }
        if (! pts.empty()) { auto fig = [] (const MixPoint& p) { roleevidence::Figure f; f.ok = true; if (p.dryDb > -500.0) f.dryDb = p.dryDb; if (p.wetDb > -500.0) f.wetDb = p.wetDb; return f; }; std::vector<MixPoint> sorted = pts; std::sort (sorted.begin(), sorted.end(), [] (const MixPoint& a, const MixPoint& b) { return a.norm < b.norm; }); mixA = fig (sorted.front()); mixB = fig (sorted.back()); }
        const auto law = mixLaw (pts);
        say ("  mix law: " + law.law + " (" + law.why + "); dry at 0 " + juce::String (law.dryRefDb, 2) + " dB, wet at 1 " + juce::String (law.wetRefDb, 2) + " dB" + (kind == "reverb" ? " (dry = the first 2 ms of the burst, before any wet; wet = the 4 ms after the burst ends)" : " (wet = the first repeat's head)"));
        for (const auto& p : pts) say ("      mix " + juce::String (p.norm, 1) + " '" + p.text + "': dry " + (p.dryDb > -500.0 ? juce::String (p.dryDb, 2) : juce::String ("floor")) + "  wet " + (p.wetDb > -500.0 ? juce::String (p.wetDb, 2) : juce::String ("floor")));
        auto* m = new juce::DynamicObject(); m->setProperty ("law", law.law); m->setProperty ("why", law.why); m->setProperty ("worst_linear_db", std::round (law.worstLinearDb * 100.0) / 100.0); m->setProperty ("worst_equal_power_db", std::round (law.worstEqualPowerDb * 100.0) / 100.0); m->setProperty ("positions", rows);
        o->setProperty ("mix_law", juce::var (m));
    }
    // 3. TIME (pre-delay / delay time) at five positions, wet only, sync off where there is one
    juce::StringArray syncOff;   // (sync off rides in wetOnly since 5 Oct evening)
    auto fiveNorms = [] { return std::vector<double> { 0.0, 0.25, 0.5, 0.75, 1.0 }; };
    if (timeCtl.index >= 0)
    {
        juce::Array<juce::var> rows; int within = 0, read = 0;
        for (double norm : fiveNorms())
        {
            juce::StringArray sets = wetOnly; sets.addArray (syncOff); sets.add (setOf (timeCtl.index, norm));
            const auto r = run ("time" + juce::String (norm, 2), tailArgs (sets));
            if (r.kind == ChildResult::Kind::uiShown) { say ("RD: a window appeared; stopping"); return 5; }
            const auto t = parseTail (r.cleanExit() ? r.out : juce::String()); if (! t.ok) { say ("  time " + juce::String (norm, 2) + ": " + (t.refused.isNotEmpty() ? "refused " + t.refused : r.describe())); continue; }
            const auto on = onsetsOf (t); const auto text = t.setTexts.count (timeCtl.index) ? t.setTexts.at (timeCtl.index) : juce::String(); const auto lab = labelMs (text); ++measured; ++read;
            const double meas = on.onsetMs ? *on.onsetMs : -1.0;   // ruling 2: time is the first repeat's onset (the spacing counts ringing inside a repeat as repeats: Feedback Hi Pass read 192 -> 142)
            const bool ok = lab && meas >= 0.0 && std::abs (meas - *lab) <= juce::jmax (2.0, 0.02 * *lab); if (ok) ++within;
            { roleevidence::Figure f; f.ok = true; if (meas >= 0.0) f.onsetMs = meas; if (norm < 0.01) timeA = f; if (norm > 0.99) timeB = f; }
            say ("  " + timeCtl.name + " = '" + text + "' (norm " + juce::String (norm, 2) + "): onset " + (on.onsetMs ? juce::String (*on.onsetMs, 1) : juce::String ("none")) + " ms" + (on.spacingMs ? " (repeats every " + juce::String (*on.spacingMs, 1) + " ms)" : juce::String()) + (lab ? " vs label " + juce::String (*lab, 1) + " ms -> " + juce::String (ok ? "within 2 % / 2 ms (the onset)" : "OFF by " + juce::String (meas - *lab, 1) + " ms (the onset)") : juce::String (" (label not a time)")));
            auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", text); ro->setProperty ("label_ms", lab ? juce::var (*lab) : juce::var()); ro->setProperty ("onset_ms", on.onsetMs ? juce::var (*on.onsetMs) : juce::var()); ro->setProperty ("repeat_spacing_ms", on.spacingMs ? juce::var (*on.spacingMs) : juce::var()); ro->setProperty ("within", ok); rows.add (juce::var (ro));
        }
        { std::set<juce::String> distinct; for (const auto& r : rows) { const auto sp = r.getProperty ("repeat_spacing_ms", {}), on = r.getProperty ("onset_ms", {}); distinct.insert (juce::String (! sp.isVoid() ? (double) sp : ! on.isVoid() ? (double) on : -1.0, 0)); }
          if (read >= 3 && distinct.size() == 1) say ("  the time control moved NOTHING across its positions (the same reading at every one): a sync mode may still be engaged, or the control is not the delay time"); }
        auto* m = new juce::DynamicObject(); m->setProperty ("control", timeCtl.name); m->setProperty ("positions", rows); m->setProperty ("within_count", within); m->setProperty ("read_count", read); o->setProperty ("time", juce::var (m));
    }
    // 4. DECAY (reverb) against the label in seconds; FEEDBACK (delay) against 20 log10 (label %) per repeat
    if (kind == "reverb" && decayCtl.index >= 0)
    {
        juce::Array<juce::var> rows;
        for (double norm : fiveNorms())
        {
            juce::StringArray sets = wetOnly; sets.add (setOf (decayCtl.index, norm));
            double tailS = 6.0; { const auto lab0 = labelSeconds (displayNear (base, decayCtl.index, norm)); if (lab0 && *lab0 > 0.0) tailS = tailFor (*lab0); }
            if (tailS > 6.0) say ("    tail scaled to the label '" + displayNear (base, decayCtl.index, norm) + "': " + juce::String (tailS, 1) + " s");
            const auto r = run ("decay" + juce::String (norm, 2), tailArgs (sets, 0.0, tailS));
            if (r.kind == ChildResult::Kind::uiShown) { say ("RD: a window appeared; stopping"); return 5; }
            const auto t = parseTail (r.cleanExit() ? r.out : juce::String()); if (! t.ok) { say ("  decay " + juce::String (norm, 2) + ": " + (t.refused.isNotEmpty() ? "refused " + t.refused : r.describe())); continue; }
            const auto dc = decayOf (t); const auto text = t.setTexts.count (decayCtl.index) ? t.setTexts.at (decayCtl.index) : juce::String(); const auto lab = labelSeconds (text); ++measured;
            { roleevidence::Figure f; f.ok = true; if (dc.ok) f.rt60s = dc.t20RT60s; if (norm < 0.01) decayA = f; if (norm > 0.99) decayB = f; }
            say ("  " + decayCtl.name + " = '" + text + "' (norm " + juce::String (norm, 2) + "): " + (dc.ok ? "RT60 " + juce::String (dc.t20RT60s, 2) + " s (T20)" + (dc.t30 ? ", " + juce::String (dc.t30RT60s, 2) + " s (T30)" : juce::String()) : dc.why) + (lab && dc.ok ? " vs label " + juce::String (*lab, 2) + " s (ratio " + juce::String (dc.t20RT60s / juce::jmax (1e-6, *lab), 2) + ")" : juce::String (lab ? "" : " (label not a time)")));
            auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", text); ro->setProperty ("label_s", lab ? juce::var (*lab) : juce::var()); ro->setProperty ("rt60_t20_s", dc.ok ? juce::var (std::round (dc.t20RT60s * 1000.0) / 1000.0) : juce::var()); ro->setProperty ("rt60_t30_s", dc.t30 ? juce::var (std::round (dc.t30RT60s * 1000.0) / 1000.0) : juce::var()); ro->setProperty ("note", dc.why); rows.add (juce::var (ro));
        }
        auto* m = new juce::DynamicObject(); m->setProperty ("control", decayCtl.name); m->setProperty ("positions", rows); o->setProperty ("decay", juce::var (m));
    }
    if (kind == "delay" && feedback.index >= 0)
    {
        juce::Array<juce::var> rows;
        for (double norm : fiveNorms())
        {
            juce::StringArray sets = wetOnly; sets.addArray (syncOff); sets.add (setOf (feedback.index, norm));
            const auto r = run ("fb" + juce::String (norm, 2), tailArgs (sets));
            if (r.kind == ChildResult::Kind::uiShown) { say ("RD: a window appeared; stopping"); return 5; }
            const auto t = parseTail (r.cleanExit() ? r.out : juce::String()); if (! t.ok) { say ("  feedback " + juce::String (norm, 2) + ": " + (t.refused.isNotEmpty() ? "refused " + t.refused : r.describe())); continue; }
            const auto on = onsetsOf (t); const auto text = t.setTexts.count (feedback.index) ? t.setTexts.at (feedback.index) : juce::String(); ++measured;
            { roleevidence::Figure f; f.ok = true; f.repeats = (int) on.repeats.size(); f.fallPerRepeatDb = on.fallPerRepeatDb; if (! on.repeats.empty()) f.firstRepeatDb = on.repeats.front().levelDb; if (norm < 0.01) fbA = f; if (norm > 0.99) fbB = f; }
            std::optional<double> labPct; { const auto n = labelMs (text); if (n && text.contains ("%")) labPct = *n; }
            const std::optional<double> expectFall = labPct && *labPct > 0.0 ? std::optional<double> (20.0 * std::log10 (*labPct / 100.0)) : std::nullopt;
            const std::optional<double> rt60 = on.spacingMs && on.fallPerRepeatDb && *on.fallPerRepeatDb < -0.01 ? std::optional<double> (60.0 / -*on.fallPerRepeatDb * *on.spacingMs / 1000.0) : std::nullopt;
            say ("  " + feedback.name + " = '" + text + "' (norm " + juce::String (norm, 2) + "): " + on.why + (on.fallPerRepeatDb ? ", fall " + juce::String (*on.fallPerRepeatDb, 2) + " dB per repeat" : juce::String()) + (expectFall ? " vs 20 log10 (label) " + juce::String (*expectFall, 2) + " dB" : juce::String()) + (rt60 ? ", repeats reach -60 dB in " + juce::String (*rt60, 2) + " s" : juce::String()));
            auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", text); ro->setProperty ("repeats", (int) on.repeats.size()); ro->setProperty ("fall_per_repeat_db", on.fallPerRepeatDb ? juce::var (std::round (*on.fallPerRepeatDb * 100.0) / 100.0) : juce::var()); ro->setProperty ("expected_fall_db", expectFall ? juce::var (std::round (*expectFall * 100.0) / 100.0) : juce::var()); ro->setProperty ("repeat_spacing_ms", on.spacingMs ? juce::var (*on.spacingMs) : juce::var()); ro->setProperty ("decay_to_minus_60_s", rt60 ? juce::var (std::round (*rt60 * 100.0) / 100.0) : juce::var()); rows.add (juce::var (ro));
        }
        auto* m = new juce::DynamicObject(); m->setProperty ("control", feedback.name); m->setProperty ("positions", rows); o->setProperty ("feedback", juce::var (m));
    }
    // 5. TEMPO SYNC (delay): sync on, the note control as instantiated, host tempo 90 / 120 / 140
    if (kind == "delay" && sync.index >= 0)
    {
        juce::Array<juce::var> rows;
        for (double bpm : { 90.0, 120.0, 140.0 })
        {
            juce::StringArray sets = wetOnly; sets.add (setOf (sync.index, sync.onNorm));
            const auto r = run ("tempo" + juce::String (bpm, 0), tailArgs (sets, bpm));
            if (r.kind == ChildResult::Kind::uiShown) { say ("RD: a window appeared; stopping"); return 5; }
            const auto t = parseTail (r.cleanExit() ? r.out : juce::String()); if (! t.ok) { say ("  tempo " + juce::String (bpm, 0) + ": " + (t.refused.isNotEmpty() ? "refused " + t.refused : r.describe())); continue; }
            const auto on = onsetsOf (t); ++measured;
            juce::String noteText; if (note.index >= 0) if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) if ((int) c.getProperty ("index", -1) == note.index) noteText = c.getProperty ("defaultOnInstantiate", {}).getProperty ("display", "").toString();
            const auto beats = noteBeats (noteText);
            const double meas = on.spacingMs ? *on.spacingMs : (on.onsetMs ? *on.onsetMs : -1.0);
            const std::optional<double> expect = beats ? std::optional<double> (expectedSyncMs (*beats, bpm)) : std::nullopt;
            const double measBeats = meas > 0.0 ? meas * bpm / 60000.0 : -1.0;
            say ("  sync on at " + juce::String (bpm, 0) + " bpm" + (noteText.isNotEmpty() ? " (note '" + noteText + "')" : juce::String()) + ": " + (meas > 0.0 ? juce::String (meas, 1) + " ms = " + juce::String (measBeats, 3) + " beat(s)" : juce::String ("no onset")) + (expect ? " vs " + juce::String (*expect, 1) + " ms expected" + (meas > 0.0 ? " (" + juce::String (meas - *expect, 1) + " ms off)" : juce::String()) : juce::String (" (the note value is not readable from the control's text: the beats figure is the finding)")));
            auto* ro = new juce::DynamicObject(); ro->setProperty ("bpm", bpm); ro->setProperty ("note_display", noteText); ro->setProperty ("measured_ms", meas > 0.0 ? juce::var (meas) : juce::var()); ro->setProperty ("measured_beats", measBeats > 0.0 ? juce::var (std::round (measBeats * 1000.0) / 1000.0) : juce::var()); ro->setProperty ("expected_ms", expect ? juce::var (*expect) : juce::var()); rows.add (juce::var (ro));
        }
        auto* m = new juce::DynamicObject(); m->setProperty ("control", sync.name); m->setProperty ("tempos", rows); o->setProperty ("tempo_sync", juce::var (m));
    }
    // ROLES BY MEASUREMENT (5 Oct evening): the nominees by their own sections' ends; the unnamed pool by one --tail pair per control
    // (as instantiated, no other write), each pair read for every signature at once: mix (dry / wet), time (onset), decay (RT60),
    // feedback (repeats / fall) - a control whose mix sits at dry gives no wet readings, and the record says "not measured"
    std::vector<int> nominated; for (const auto& c : { mix, timeCtl, decayCtl, feedback, sync, note }) if (c.index >= 0) nominated.push_back (c.index);
    if (mix.index >= 0) roles.push_back (roleevidence::nominee (mix.index, mix.name, "mix", roleevidence::signatureHolds ("mix", mixA, mixB)));
    if (timeCtl.index >= 0) roles.push_back (roleevidence::nominee (timeCtl.index, timeCtl.name, "time", roleevidence::signatureHolds ("time", timeA, timeB)));
    if (decayCtl.index >= 0) roles.push_back (roleevidence::nominee (decayCtl.index, decayCtl.name, "decay", roleevidence::signatureHolds ("decay", decayA, decayB)));
    if (feedback.index >= 0) roles.push_back (roleevidence::nominee (feedback.index, feedback.name, "feedback", roleevidence::signatureHolds ("feedback", fbA, fbB)));
    for (const auto& pc : unnamedPool (base, nominated, &fx.sampled))
    {
        roleevidence::Figure ua, ub;
        for (double n : { 0.0, 1.0 })
        {
            const auto r = run ("u" + juce::String (pc.index) + ".n" + juce::String (n, 0), tailArgs ({ setOf (pc.index, n) }));
            if (r.kind == ChildResult::Kind::uiShown) { say ("RD: a window appeared; stopping"); return 5; }
            const auto t = parseTail (r.cleanExit() ? r.out : juce::String()); if (! t.ok) continue;
            auto& f = n < 0.5 ? ua : ub; f.ok = true;
            const auto lv = levelsOf (t); if (lv.ok) { if (lv.dryDb > -500.0) f.dryDb = lv.dryDb; if (lv.wetDb > -500.0) f.wetDb = lv.wetDb; }
            const auto on = onsetsOf (t); f.onsetMs = on.onsetMs; f.repeats = (int) on.repeats.size(); f.fallPerRepeatDb = on.fallPerRepeatDb; if (! on.repeats.empty()) f.firstRepeatDb = on.repeats.front().levelDb;
            const auto dc = decayOf (t); if (dc.ok) f.rt60s = dc.t20RT60s;
        }
        for (const char* role : { "mix", "time", "decay", "feedback" }) roles.push_back (roleevidence::unnamed (pc.index, pc.name, role, roleevidence::signatureHolds (role, ua, ub)));
        // ruling 2: a control that holds the onset and is not feedback (the first repeat put) but changes the repeats' shape or level is the path's tone
        if (! roleevidence::signatureHolds ("time", ua, ub).holds && ! roleevidence::signatureHolds ("feedback", ua, ub).holds)
            if (const auto tn = roleevidence::signatureHolds ("tone", ua, ub); tn.holds) roles.push_back (roleevidence::unnamed (pc.index, pc.name, "tone (feedback path)", tn));
    }
    sayRoles (say, roles);
    setRoles (o, roles);
    o->setProperty ("processes", processN); o->setProperty ("measuredAt", nowStamp());
    o->setProperty ("method", "probe --tail: a 997 Hz burst (" + burstMs + " ms at -12 dBFS) then 6 s of silence, the output's RMS and peak per 1 ms window on the input's clock (latency taken out); dry = the burst's first 2 ms, wet = the 4 ms after the burst (reverb) or the first repeat's head (delay); mix law = worst deviation from linear / equal power over 11 positions, fit within 1 dB; decay = T20 / T30 least-squares on the 10 ms-smoothed tail, RT60 extrapolated; onset = the first window 20 dB above the measured floor; repeats = the tail's peak maxima, spacing and fall as medians; tempo sync = the probe's playhead (playing, 4/4) at 90 / 120 / 140");
    outDir.getChildFile (stem + ".reverbdelay.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("RD: -> " + outDir.getChildFile (stem + ".reverbdelay.json").getFullPathName() + " (" + juce::String (processN) + " processes)");
    return measured > 0 ? 0 : 4;
}

// TRANSIENT SHAPERS AND GATES PROTOTYPE (roadmap 2.8, 5 Oct R5): --cert-dynamics <product> [--kind transient|gate].
// Transient: the attack and sustain controls at five positions on the drum-like burst (probe --hits), each against the
// neutral run (the instantiate state); a dB label is compared. Gate: the threshold control at five positions on the level
// ramp (probe --ramp): open / close level against the label, hysteresis, range; the range control at three positions;
// attack / hold / release controls at three positions each on the burst (probe --burst). Nothing exported.
inline int runDynamics (const SweepOptions& opt, juce::String kind)
{
    using namespace dynamics;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("DYN: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("DYN: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("DYN: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false); const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    if (kind.isEmpty()) { const auto in = loadDiscoveryInputs (opt.ledger); const auto it = in.categoryByUid.find ("AudioUnit|" + uidHex); kind = it != in.categoryByUid.end() ? it->second : juce::String(); if (kind == "transient_shaper") kind = "transient"; }
    if (kind != "transient" && kind != "gate") { say ("DYN: '" + opt.product + "' is category '" + kind + "' in the ledger; say --kind transient or --kind gate"); return 2; }
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("dynamics"); outDir.createDirectory();
    int processN = 0;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra) { juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs); ++processN; raw.getChildFile (stem + ".dynamics." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r; };
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "dynamics", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "attack", "transient", "punch", "transients", "sustain", "body", "release", "tail", "threshold", "thresh", "open", "range", "floor", "depth", "reduction", "hold", "decay" });
    if (! fx.ok) { say ("DYN: " + fx.why); return 1; }
    const auto base = fx.base; say ("DYN: " + fx.note);
    struct Ctl { int index = -1; juce::String name; };
    Ctl attack, sustain, threshold, range, gAttack, gHold, gRelease;
    auto answers = [] (const juce::String& name, std::initializer_list<const char*> terms) { for (const char* t : terms) if (nametokens::controlAnswersTerm (name, t)) return true; return false; };
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const int idx = (int) c.getProperty ("index", -1); const auto n = c.getProperty ("name", "").toString();
            if (sweep::wordValued (c) || (int) c.getProperty ("numSteps", 0) == 2 || sweep::neverTouchName (n)) continue;
            if (kind == "transient")
            {
                if (attack.index < 0 && answers (n, { "attack", "transient", "punch", "transients" }) && ! answers (n, { "time", "speed", "ms" })) attack = { idx, n };
                else if (sustain.index < 0 && answers (n, { "sustain", "body", "release", "tail" }) && ! answers (n, { "time", "ms" })) sustain = { idx, n };
            }
            else
            {
                if (threshold.index < 0 && answers (n, { "threshold", "thresh", "open" })) threshold = { idx, n };
                else if (range.index < 0 && answers (n, { "range", "floor", "depth", "reduction" })) range = { idx, n };
                else if (gAttack.index < 0 && answers (n, { "attack" })) gAttack = { idx, n };
                else if (gHold.index < 0 && answers (n, { "hold" })) gHold = { idx, n };
                else if (gRelease.index < 0 && answers (n, { "release", "decay" })) gRelease = { idx, n };
            }
        }
    auto named = [] (const Ctl& c) { return c.index < 0 ? juce::String ("-") : "[" + juce::String (c.index) + "] " + c.name; };
    say ("DYN: " + opt.product + " " + desc.version + " (" + kind + "): " + (kind == "transient" ? "attack " + named (attack) + "; sustain " + named (sustain) : "threshold " + named (threshold) + "; range " + named (range) + "; attack " + named (gAttack) + "; hold " + named (gHold) + "; release " + named (gRelease)));
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_dynamics_prototype/0"); o->setProperty ("status", "PROTOTYPE - roadmap 2.8, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version); o->setProperty ("kind", kind);
    auto setOf = [] (int idx, double norm) { return juce::String (idx) + ":" + juce::String (norm, 6); };
    auto fiveNorms = [] { return std::vector<double> { 0.0, 0.25, 0.5, 0.75, 1.0 }; };
    int measured = 0;
    std::vector<roleevidence::RoleVerdict> roles; std::vector<int> nominated;
    if (kind == "transient")
    {
        if (attack.index < 0 && sustain.index < 0) { juce::StringArray names; if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString()); say ("  no attack / sustain control by name; controls: " + names.joinIntoString (", ")); return 4; }
        // THE HIT LENGTH (5 Oct evening, item 2): the attack control is read on the short hit (150 ms decay); the sustain control on a
        // LONG hit (500 ms decay, 1500 ms period) so the 80-250 ms sustain window sits in a body that is still there - MTransient's
        // +-24 dB sustain lane read +-2.9 on the short hit. Each hit shape has its own neutral run.
        const juce::StringArray hitArgs { "--hits", "db=-6", "hz=997", "decay_ms=150", "period_ms=600", "hits=4", "win_ms=1" };
        const juce::StringArray longHitArgs { "--hits", "db=-6", "hz=997", "decay_ms=500", "period_ms=1500", "hits=4", "win_ms=1" };
        const auto n0 = run ("neutral", hitArgs);
        if (n0.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
        const auto neutral = hitFigures (parseHits (n0.cleanExit() ? n0.out : juce::String()));
        if (! neutral.ok) { say ("  neutral run: " + neutral.why + " (" + n0.describe() + ")"); return 4; }
        ++measured;
        say ("  neutral (as instantiated, short hit): transient " + juce::String (neutral.transientDb, 2) + " dB, sustain " + juce::String (neutral.sustainDb, 2) + " dB against the input, over " + juce::String (neutral.hitsUsed) + " hits");
        auto* nv = new juce::DynamicObject(); nv->setProperty ("transient_db", std::round (neutral.transientDb * 100.0) / 100.0); nv->setProperty ("sustain_db", std::round (neutral.sustainDb * 100.0) / 100.0); o->setProperty ("neutral", juce::var (nv));
        HitFigures neutralLong;
        if (sustain.index >= 0)
        {
            const auto n1 = run ("neutral.long", longHitArgs);
            if (n1.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
            neutralLong = hitFigures (parseHits (n1.cleanExit() ? n1.out : juce::String()));
            if (neutralLong.ok) say ("  neutral (long hit, 500 ms decay): transient " + juce::String (neutralLong.transientDb, 2) + " dB, sustain " + juce::String (neutralLong.sustainDb, 2) + " dB");
            auto* nl = new juce::DynamicObject(); nl->setProperty ("transient_db", std::round (neutralLong.transientDb * 100.0) / 100.0); nl->setProperty ("sustain_db", std::round (neutralLong.sustainDb * 100.0) / 100.0); nl->setProperty ("hit", "500 ms decay, 1500 ms period"); o->setProperty ("neutral_long_hit", juce::var (nl));
        }
        // the sustain control is read on BOTH hits (the long hit read sustain SMALLER on Smack Attack and MTransient, not larger: the
        // sustain window against the unit's own envelope is the open question, said in the proposal); the attack control on the short one
        std::vector<std::pair<Ctl, bool>> passes; if (attack.index >= 0) passes.push_back ({ attack, false }); if (sustain.index >= 0) { passes.push_back ({ sustain, false }); if (neutralLong.ok) passes.push_back ({ sustain, true }); }
        for (const auto& [ctl, longHit] : passes)
        {
            const auto& hitArgsFor = longHit ? longHitArgs : hitArgs; const auto& neutralFor = longHit ? neutralLong : neutral;
            juce::Array<juce::var> rows; int withinLabel = 0, labelled = 0; roleevidence::Figure endA, endB; nominated.push_back (ctl.index);
            for (double norm : fiveNorms())
            {
                juce::StringArray a = hitArgsFor; a.add ("set=" + setOf (ctl.index, norm));
                const auto r = run ("c" + juce::String (ctl.index) + (longHit ? ".long" : "") + ".n" + juce::String (norm, 2), a);
                if (r.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
                const auto h = parseHits (r.cleanExit() ? r.out : juce::String()); const auto f = hitFigures (h);
                const auto text = h.setTexts.count (ctl.index) ? h.setTexts.at (ctl.index) : juce::String();
                const auto pt = transientPoint ((float) norm, text, f, neutralFor);
                if (! pt.ok) { say ("  " + ctl.name + " = '" + text + "': " + f.why); continue; }
                ++measured;
                const bool isAttack = ctl.index == attack.index; const double effect = isAttack ? pt.dTransientDb : pt.dSustainDb;
                { roleevidence::Figure f; f.ok = true; f.transientDb = pt.transientDb; f.sustainDb = pt.sustainDb; if (norm < 0.01) endA = f; if (norm > 0.99) endB = f; }
                bool within = false; if (pt.labelDb) { ++labelled; within = std::abs (effect - *pt.labelDb) <= 1.0; if (within) ++withinLabel; }
                say ("  " + ctl.name + (longHit ? " [long hit]" : "") + " = '" + text + "' (norm " + juce::String (norm, 2) + "): transient " + juce::String (pt.dTransientDb, 2) + " dB, sustain " + juce::String (pt.dSustainDb, 2) + " dB against neutral" + (pt.labelDb ? " | label " + juce::String (*pt.labelDb, 1) + " dB -> " + (within ? "within 1 dB" : "OFF by " + juce::String (effect - *pt.labelDb, 2)) : juce::String()));
                auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", text); ro->setProperty ("transient_db", std::round (pt.transientDb * 100.0) / 100.0); ro->setProperty ("sustain_db", std::round (pt.sustainDb * 100.0) / 100.0); ro->setProperty ("d_transient_db", std::round (pt.dTransientDb * 100.0) / 100.0); ro->setProperty ("d_sustain_db", std::round (pt.dSustainDb * 100.0) / 100.0); ro->setProperty ("label_db", pt.labelDb ? juce::var (*pt.labelDb) : juce::var()); rows.add (juce::var (ro));
            }
            auto* m = new juce::DynamicObject(); m->setProperty ("index", ctl.index); m->setProperty ("control", ctl.name); m->setProperty ("positions", rows); m->setProperty ("labelled", labelled); m->setProperty ("within_1db", withinLabel); m->setProperty ("hit", longHit ? "500 ms decay, 1500 ms period (the long hit)" : "150 ms decay, 600 ms period"); o->setProperty (ctl.index == attack.index ? "attack" : longHit ? "sustain_long_hit" : "sustain", juce::var (m));
            if (! longHit) roles.push_back (roleevidence::nominee (ctl.index, ctl.name, ctl.index == attack.index ? "transient" : "sustain", roleevidence::signatureHolds (ctl.index == attack.index ? "transient" : "sustain", endA, endB)));
        }
        // the unnamed pool: one --hits pair per control, read for both signatures
        for (const auto& pc : unnamedPool (base, nominated, &fx.sampled))
        {
            roleevidence::Figure ua, ub;
            for (double n : { 0.0, 1.0 })
            {
                juce::StringArray a = hitArgs; a.add ("set=" + setOf (pc.index, n));
                const auto r = run ("u" + juce::String (pc.index) + ".n" + juce::String (n, 0), a);
                if (r.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
                const auto f = hitFigures (parseHits (r.cleanExit() ? r.out : juce::String())); if (! f.ok) continue;
                auto& g = n < 0.5 ? ua : ub; g.ok = true; g.transientDb = f.transientDb; g.sustainDb = f.sustainDb;
            }
            for (const char* role : { "transient", "sustain" }) roles.push_back (roleevidence::unnamed (pc.index, pc.name, role, roleevidence::signatureHolds (role, ua, ub)));
        }
    }
    else
    {
        if (threshold.index < 0) { juce::StringArray names; if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString()); say ("  no threshold control by name; controls: " + names.joinIntoString (", ")); return 4; }
        const juce::StringArray rampArgs { "--ramp", "from=-70", "to=-6", "up_s=3", "down_s=3", "hz=997", "win_ms=5" };
        juce::Array<juce::var> rows; std::optional<double> midOpenRms; roleevidence::Figure thrA, thrB; nominated.push_back (threshold.index);
        for (double norm : fiveNorms())
        {
            juce::StringArray a = rampArgs; a.add ("set=" + setOf (threshold.index, norm));
            const auto r = run ("thr.n" + juce::String (norm, 2), a);
            if (r.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
            const auto rp = parseRamp (r.cleanExit() ? r.out : juce::String()); const auto g = gateLevels (rp);
            const auto text = rp.setTexts.count (threshold.index) ? rp.setTexts.at (threshold.index) : juce::String(); const auto lab = labelNumber (text);
            if (! g.ok) { say ("  " + threshold.name + " = '" + text + "': " + g.why); continue; }
            ++measured;
            const std::optional<double> openPeak = g.openAtInDb ? std::optional<double> (*g.openAtInDb + 3.01) : std::nullopt;
            if (std::abs (norm - 0.5) < 1e-6 && g.openAtInDb) midOpenRms = *g.openAtInDb;
            { roleevidence::Figure f; f.ok = true; f.openLevelDb = g.openAtInDb; if (g.gating) f.rangeDb = -g.rangeDb; if (norm < 0.01) thrA = f; if (norm > 0.99) thrB = f; }
            say ("  " + threshold.name + " = '" + text + "' (norm " + juce::String (norm, 2) + "): " + g.why + (g.gating && openPeak ? " (" + juce::String (*openPeak, 1) + " dBFS peak)" : juce::String()) + (g.hysteresisDb ? ", hysteresis " + juce::String (*g.hysteresisDb, 1) + " dB" : juce::String()) + (lab && openPeak && text.containsIgnoreCase ("db") ? " | label " + juce::String (*lab, 1) + " -> " + juce::String (*openPeak - *lab, 1) + " dB above it (peak)" : juce::String()));
            auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", text); ro->setProperty ("label", lab ? juce::var (*lab) : juce::var()); ro->setProperty ("gating", g.gating); ro->setProperty ("range_db", std::round (g.rangeDb * 100.0) / 100.0);
            ro->setProperty ("open_at_rms_dbfs", g.openAtInDb ? juce::var (std::round (*g.openAtInDb * 100.0) / 100.0) : juce::var()); ro->setProperty ("open_at_peak_dbfs", openPeak ? juce::var (std::round (*openPeak * 100.0) / 100.0) : juce::var()); ro->setProperty ("close_at_rms_dbfs", g.closeAtInDb ? juce::var (std::round (*g.closeAtInDb * 100.0) / 100.0) : juce::var()); ro->setProperty ("hysteresis_db", g.hysteresisDb ? juce::var (std::round (*g.hysteresisDb * 100.0) / 100.0) : juce::var()); rows.add (juce::var (ro));
        }
        { auto* m = new juce::DynamicObject(); m->setProperty ("index", threshold.index); m->setProperty ("control", threshold.name); m->setProperty ("positions", rows); o->setProperty ("threshold", juce::var (m)); }
        // the gate's open level at the threshold's ends is often "never opened" / "not gating" at an end (-inf, 0 dB): the signature then
        // reads the two positions nearest the ends that opened
        { std::vector<std::pair<double, double>> opened; for (const auto& r : rows) { const auto v = r.getProperty ("open_at_rms_dbfs", {}); if (! v.isVoid()) opened.push_back ({ (double) r.getProperty ("norm", 0.0), (double) v }); }
          if (opened.size() >= 2) { thrA.ok = thrB.ok = true; thrA.openLevelDb = opened.front().second; thrB.openLevelDb = opened.back().second; } }
        roles.push_back (roleevidence::nominee (threshold.index, threshold.name, "gate_threshold", roleevidence::signatureHolds ("gate_threshold", thrA, thrB)));
        if (range.index >= 0)
        {
            juce::Array<juce::var> rr; roleevidence::Figure rA, rB; nominated.push_back (range.index);
            for (double norm : { 0.0, 0.5, 1.0 })
            {
                juce::StringArray a = rampArgs; a.add ("set=" + setOf (threshold.index, 0.5) + "," + setOf (range.index, norm));
                const auto r = run ("range.n" + juce::String (norm, 2), a);
                if (r.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
                const auto rp = parseRamp (r.cleanExit() ? r.out : juce::String()); const auto g = gateLevels (rp);
                const auto text = rp.setTexts.count (range.index) ? rp.setTexts.at (range.index) : juce::String(); const auto lab = labelNumber (text);
                if (! g.ok) { say ("  " + range.name + " = '" + text + "': " + g.why); continue; }
                ++measured;
                say ("  " + range.name + " = '" + text + "' (threshold at norm 0.5): closed " + juce::String (-g.rangeDb, 1) + " dB" + (lab && text.containsIgnoreCase ("db") ? " vs label " + juce::String (*lab, 1) + " -> " + juce::String (-g.rangeDb - *lab, 1) + " dB off" : juce::String()));
                auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", text); ro->setProperty ("label", lab ? juce::var (*lab) : juce::var()); ro->setProperty ("closed_db", std::round (-g.rangeDb * 100.0) / 100.0); rr.add (juce::var (ro));
                { roleevidence::Figure f; f.ok = true; f.rangeDb = -g.rangeDb; f.openGainDb = g.openGainDb; if (norm < 0.01) rA = f; if (norm > 0.99) rB = f; }
            }
            auto* m = new juce::DynamicObject(); m->setProperty ("index", range.index); m->setProperty ("control", range.name); m->setProperty ("positions", rr); o->setProperty ("range", juce::var (m));
            roles.push_back (roleevidence::nominee (range.index, range.name, "range", roleevidence::signatureHolds ("range", rA, rB)));
        }
        // timing on the burst: the threshold at norm 0.5; quiet 12 dB under its open level, loud 12 dB over (peak), or -50 / -10 when it never opened
        const double openRms = midOpenRms ? *midOpenRms : -33.0;
        const juce::String quiet = juce::String (juce::jmax (-80.0, openRms + 3.01 - 12.0), 1), loud = juce::String (juce::jmin (-1.0, openRms + 3.01 + 12.0), 1);
        for (const auto& ctl : { gAttack, gHold, gRelease })
        {
            if (ctl.index < 0) continue;
            juce::Array<juce::var> rr; roleevidence::Figure tA, tB; nominated.push_back (ctl.index);
            for (double norm : { 0.0, 0.5, 1.0 })
            {
                juce::StringArray a { "--burst", "quiet=" + quiet, "loud=" + loud, "pre=1.0", "hold=1.0", "post=3.0", "hz=997", "win_ms=1", "set=" + setOf (threshold.index, 0.5) + "," + setOf (ctl.index, norm) };
                const auto r = run ("c" + juce::String (ctl.index) + ".n" + juce::String (norm, 2), a);
                if (r.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
                const auto b = timing::parseBurst (r.cleanExit() ? r.out : juce::String()); const auto t = gateTiming (b);
                juce::String text; for (const auto& line : juce::StringArray::fromLines (r.out)) { const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() > 2 && f[0] == "set" && f[1].getIntValue() == ctl.index) for (int i = 2; i + 1 < f.size(); ++i) if (f[i] == "text") text = f[i + 1]; }
                const auto lab = labelMs (text);
                if (! t.ok) { say ("  " + ctl.name + " = '" + text + "': " + t.why); continue; }
                ++measured;
                const std::optional<double> meas = ctl.index == gAttack.index ? t.attackMs : ctl.index == gHold.index ? t.holdMs : t.releaseMs;
                { roleevidence::Figure f; f.ok = true; f.attackMs = t.attackMs; f.holdMs = t.holdMs; f.releaseMs = t.releaseMs; if (norm < 0.01) tA = f; if (norm > 0.99) tB = f; }
                say ("  " + ctl.name + " = '" + text + "' (norm " + juce::String (norm, 2) + "): " + t.why + (lab && meas ? " | this control's figure " + juce::String (*meas, 1) + " ms vs label " + juce::String (*lab, 1) + " ms (ratio " + juce::String (*meas / juce::jmax (1e-6, *lab), 2) + ")" : juce::String()));
                auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", norm); ro->setProperty ("display", text); ro->setProperty ("label_ms", lab ? juce::var (*lab) : juce::var()); ro->setProperty ("attack_ms", t.attackMs ? juce::var (std::round (*t.attackMs * 10.0) / 10.0) : juce::var()); ro->setProperty ("hold_ms", t.holdMs ? juce::var (std::round (*t.holdMs * 10.0) / 10.0) : juce::var()); ro->setProperty ("release_ms", t.releaseMs ? juce::var (std::round (*t.releaseMs * 10.0) / 10.0) : juce::var()); rr.add (juce::var (ro));
            }
            auto* m = new juce::DynamicObject(); m->setProperty ("index", ctl.index); m->setProperty ("control", ctl.name); m->setProperty ("positions", rr); o->setProperty (ctl.index == gAttack.index ? "attack" : ctl.index == gHold.index ? "hold" : "release", juce::var (m));
            const juce::String role = ctl.index == gAttack.index ? "attack" : ctl.index == gHold.index ? "hold" : "release";
            roles.push_back (roleevidence::nominee (ctl.index, ctl.name, role, roleevidence::signatureHolds (role, tA, tB)));
        }
        // the unnamed pool: one --ramp pair per control with the threshold at norm 0.5, read for the open level and the closed level
        for (const auto& pc : unnamedPool (base, nominated, &fx.sampled))
        {
            roleevidence::Figure ua, ub;
            for (double n : { 0.0, 1.0 })
            {
                juce::StringArray a = rampArgs; a.add ("set=" + setOf (threshold.index, 0.5) + "," + setOf (pc.index, n));
                const auto r = run ("u" + juce::String (pc.index) + ".n" + juce::String (n, 0), a);
                if (r.kind == ChildResult::Kind::uiShown) { say ("DYN: a window appeared; stopping"); return 5; }
                const auto g = gateLevels (parseRamp (r.cleanExit() ? r.out : juce::String())); if (! g.ok) continue;
                auto& f = n < 0.5 ? ua : ub; f.ok = true; f.openLevelDb = g.openAtInDb; f.rangeDb = -g.rangeDb; f.openGainDb = g.openGainDb;
            }
            for (const char* role : { "gate_threshold", "range" }) roles.push_back (roleevidence::unnamed (pc.index, pc.name, role, roleevidence::signatureHolds (role, ua, ub)));
        }
    }
    sayRoles (say, roles, kind == "gate" ? "unnamed controls are probed for the threshold and range signatures (a burst pair per control is not run)" : juce::String());
    setRoles (o, roles, kind == "gate" ? "unnamed controls probed for threshold and range only" : juce::String());
    o->setProperty ("processes", processN); o->setProperty ("measuredAt", nowStamp());
    o->setProperty ("method", kind == "transient" ? juce::String ("probe --hits: a 997 Hz drum-like burst (one-sample attack, 150 ms exponential decay, -6 dBFS peak) four times at 600 ms; transient = output peak over input peak in the first 10 ms, sustain = output RMS over input RMS at 80-250 ms, medians over hits 2-4; each position against the neutral (instantiate) run; a dB label compared within 1 dB")
                                                 : juce::String ("probe --ramp: a 997 Hz sine rising -70 -> -6 dBFS over 3 s and back; closed / open gain = medians at the quiet / loud tenth, range = their difference (gating when >= 3 dB), open / close level = the input RMS where the gain crosses midway up / down, hysteresis = open - close; probe --burst for attack (90 % open after the step up), hold (to 1 dB of fall after the step down) and release (from there to 90 % closed), interpolated between 1 ms windows"));
    outDir.getChildFile (stem + ".dynamics.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("DYN: -> " + outDir.getChildFile (stem + ".dynamics.json").getFullPathName() + " (" + juce::String (processN) + " processes)");
    return measured > 0 ? 0 : 4;
}

// DE-ESSERS PROTOTYPE (roadmap 2.9, 5 Oct R6): --cert-deesser <product>. The threshold ladder at 6.5 kHz (and at 997 Hz as the
// control), the frequency control's display against the measured centre of the reduction (multitone response, hard
// threshold against the open end), split-band against wideband from the same response (reduction at 997 Hz vs the band).
inline int runDeesser (const SweepOptions& opt)
{
    using namespace deesser;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("DS: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("DS: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("DS: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false); const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("deesser"); outDir.createDirectory();
    int processN = 0;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra) { juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs); ++processN; raw.getChildFile (stem + ".deesser." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r; };
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "deesser", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "threshold", "thresh", "thr", "sensitivity", "amount", "reduction", "range", "frequency", "freq", "hz", "center", "centre", "tune", "mode", "type", "split", "wide", "band" });
    if (! fx.ok) { say ("DS: " + fx.why); return 1; }
    const auto base = fx.base; say ("DS: " + fx.note);
    struct Ctl { int index = -1; juce::String name; std::map<juce::String, double> texts; };
    Ctl threshold, freq, mode;
    auto answers = [] (const juce::String& name, std::initializer_list<const char*> terms) { for (const char* t : terms) if (nametokens::controlAnswersTerm (name, t)) return true; return false; };
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const int idx = (int) c.getProperty ("index", -1); const auto n = c.getProperty ("name", "").toString();
            if (sweep::neverTouchName (n)) continue;
            const bool word = sweep::wordValued (c) || (int) c.getProperty ("numSteps", 0) == 2;
            if (! word && threshold.index < 0 && answers (n, { "threshold", "thresh", "thr", "sensitivity", "amount", "reduction", "range" })) threshold = { idx, n, {} };
            else if (! word && freq.index < 0 && answers (n, { "frequency", "freq", "hz", "center", "centre", "tune" })) freq = { idx, n, {} };
            else if (word && mode.index < 0 && (answers (n, { "mode", "type", "split", "wide", "band" }) || [&] { const auto at = c.getProperty ("displayAt", {}); if (! at.isObject()) return false; for (const auto& kv : at.getDynamicObject()->getProperties()) { const auto t = kv.value.toString().toLowerCase(); if (t.contains ("split") || t.contains ("wide") || t.contains ("broad")) return true; } return false; }()))
            { mode = { idx, n, {} }; if (const auto at = c.getProperty ("displayAt", {}); at.isObject()) for (const auto& kv : at.getDynamicObject()->getProperties()) mode.texts[kv.value.toString()] = kv.name.toString().getDoubleValue(); }
        }
    auto named = [] (const Ctl& c) { return c.index < 0 ? juce::String ("-") : "[" + juce::String (c.index) + "] " + c.name; };
    say ("DS: " + opt.product + " " + desc.version + ": threshold " + named (threshold) + "; frequency " + named (freq) + "; mode " + named (mode) + (mode.index >= 0 ? " (" + [&] { juce::StringArray t; for (const auto& [k, v] : mode.texts) t.add (k); return t.joinIntoString (" / "); }() + ")" : juce::String()));
    if (threshold.index < 0) { juce::StringArray names; if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString()); say ("  no threshold control by name; controls: " + names.joinIntoString (", ")); return 4; }
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_deesser_prototype/0"); o->setProperty ("status", "PROTOTYPE - roadmap 2.9, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version);
    auto setOf = [] (int idx, double norm) { return juce::String (idx) + ":" + juce::String (norm, 6); };
    juce::StringArray norms; for (int k = 0; k <= 5; ++k) norms.add (juce::String (k / 5.0f, 6));
    int measured = 0;
    std::vector<roleevidence::RoleVerdict> roles; std::vector<int> nominated; for (const auto& c : { threshold, freq, mode }) if (c.index >= 0) nominated.push_back (c.index);
    // 1. THE LADDER at 6.5 kHz and at 997 Hz: threshold at 6 norms, five levels, one process each
    std::map<double, Ladder> ladders;
    for (double hz : { 6500.0, 997.0 })
    {
        const auto r = run ("ladder" + juce::String (hz, 0), { "--sweep", "thr=" + juce::String (threshold.index), "norms=" + norms.joinIntoString (","), "levels=-30,-24,-18,-12,-6", "hz=" + juce::String (hz, 0), "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" });
        if (r.kind == ChildResult::Kind::uiShown) { say ("DS: a window appeared; stopping"); return 5; }
        const auto m = sweep::parseSweep (r.cleanExit() ? r.out : juce::String());
        const auto L = ladderOf (m, hz); ladders[hz] = L;
        if (! L.ok) { say ("  ladder at " + juce::String (hz, 0) + " Hz: " + L.why + " (" + r.describe() + ")"); continue; }
        ++measured;
        say ("  ladder at " + juce::String (hz, 0) + " Hz: " + L.why);
        juce::StringArray texts; for (const auto& c : L.cells) texts.addIfNotAlreadyThere (c.text);
        for (const auto& t : texts)
        {
            juce::String line = "      " + threshold.name + " = '" + t + "':";
            for (const auto& c : L.cells) if (c.text == t && c.ok) line << "  " << juce::String (c.levelDbfs, 0) << " dBFS GR " << juce::String (c.grDb, 2);
            say (line);
        }
    }
    {
        juce::Array<juce::var> lv;
        for (const auto& [hz, L] : ladders)
        {
            auto* lo = new juce::DynamicObject(); lo->setProperty ("hz", hz); lo->setProperty ("ok", L.ok); lo->setProperty ("note", L.why); lo->setProperty ("max_gr_db", std::round (L.maxGrDb * 100.0) / 100.0);
            juce::Array<juce::var> cells; for (const auto& c : L.cells) { auto* co = new juce::DynamicObject(); co->setProperty ("norm", c.norm); co->setProperty ("display", c.text); co->setProperty ("level_dbfs", c.levelDbfs); co->setProperty ("gain_db", std::round (c.gainDb * 100.0) / 100.0); co->setProperty ("gr_db", std::round (c.grDb * 100.0) / 100.0); cells.add (juce::var (co)); }
            lo->setProperty ("cells", cells); lv.add (juce::var (lo));
        }
        o->setProperty ("ladders", lv);
        if (ladders.count (6500.0) && ladders.count (997.0) && ladders.at (6500.0).ok && ladders.at (997.0).ok)
            say ("  band selectivity on the ladder: max GR " + juce::String (ladders.at (6500.0).maxGrDb, 2) + " dB at 6.5 kHz vs " + juce::String (ladders.at (997.0).maxGrDb, 2) + " dB at 997 Hz");
    }
    // 1b. THE NOISE LADDER (5 Oct evening, item 2): band-limited noise 4-10 kHz - 121 random-phase tones over that band through the
    // probe's --response, the threshold at 6 norms per process, one process per level; GR = total output power against the open
    // end's at that level (the de-esser's own band, not one tone)
    {
        juce::Array<juce::var> rows; double maxNoiseGr = 0.0; int readN = 0;
        std::map<double, std::vector<std::pair<float, double>>> gainByLevel;   // level -> (norm, total gain)
        for (double Lv : { -30.0, -24.0, -18.0, -12.0, -6.0 })
        {
            const auto r = run ("noise.L" + juce::String ((int) -Lv), { "--response", "ctl=" + juce::String (threshold.index), "norms=" + norms.joinIntoString (","), "tones=121", "lo=4000", "hi=10000", "db=" + juce::String (Lv, 0), "hold=1.5", "discard=0.75" });
            if (r.kind == ChildResult::Kind::uiShown) { say ("DS: a window appeared; stopping"); return 5; }
            const auto resp = eq::parseResponse (r.cleanExit() ? r.out : juce::String()); if (! resp.ok) continue;
            for (const auto& p : resp.positions) if (p.landed) if (const auto g = multiband::totalGainDb (p)) gainByLevel[Lv].push_back ({ p.norm, *g });
        }
        for (auto& [Lv, v] : gainByLevel)
        {
            double open = -1e9; for (const auto& [n, g] : v) open = juce::jmax (open, g);
            for (const auto& [n, g] : v) { const double gr = open - g; maxNoiseGr = juce::jmax (maxNoiseGr, gr); ++readN; auto* ro = new juce::DynamicObject(); ro->setProperty ("level_dbfs", Lv); ro->setProperty ("norm", n); ro->setProperty ("gr_db", std::round (gr * 100.0) / 100.0); rows.add (juce::var (ro)); }
        }
        auto* m = new juce::DynamicObject(); m->setProperty ("signal", "121 random-phase tones 4-10 kHz (band-limited noise), total power"); m->setProperty ("max_gr_db", std::round (maxNoiseGr * 100.0) / 100.0); m->setProperty ("cells", rows); o->setProperty ("noise_ladder", juce::var (m));
        say ("  noise ladder (4-10 kHz, " + juce::String (readN) + " readings): max GR " + juce::String (maxNoiseGr, 2) + " dB" + (ladders.count (6500.0) && ladders.at (6500.0).ok ? " (the 6.5 kHz tone gave " + juce::String (ladders.at (6500.0).maxGrDb, 2) + ")" : juce::String()));
        if (readN > 0) ++measured;
    }
    // ROLES BY MEASUREMENT: the threshold nominee by GR at -12 dBFS on the sibilance tone between its ends
    const auto& L65 = ladders[6500.0];
    {
        roleevidence::Figure a, b; for (const auto& c : L65.cells) if (c.ok && std::abs (c.levelDbfs + 12.0) < 0.01) { roleevidence::Figure* f = c.norm < 0.01f ? &a : c.norm > 0.99f ? &b : nullptr; if (f) { f->ok = true; f->grDb = c.grDb; } }
        roles.push_back (roleevidence::nominee (threshold.index, threshold.name, "threshold", roleevidence::signatureHolds ("threshold", a, b)));
    }
    roleevidence::Figure freqA, freqB;
    // 2. THE CENTRE and 3. THE MODE from the multitone response: hard threshold (most GR at -12) against the open end
    const auto hard = hardestNorm (L65, -12.0);
    if (! L65.ok || ! hard || L65.maxGrDb < kBandCutDb) say ("  centre / mode not read: the ladder at 6.5 kHz shows " + juce::String (L65.maxGrDb, 2) + " dB of GR at most (needs " + juce::String (kBandCutDb, 0) + ")");
    else
    {
        const float openNorm = L65.openIndex >= 0 ? L65.cells[(size_t) L65.openIndex].norm : 0.0f;
        auto response = [&] (const juce::String& tag, const juce::StringArray& sets) {
            juce::StringArray a { "--response", "ctl=" + juce::String (threshold.index), "norms=current", "tones=121", "lo=20", "hi=20000", "db=-12", "hold=1.5", "discard=0.75" }; if (! sets.isEmpty()) a.add ("set=" + sets.joinIntoString (","));
            const auto r = run (tag, a); return eq::parseResponse (r.cleanExit() ? r.out : juce::String()); };
        auto centreFor = [&] (const juce::String& tag, const juce::StringArray& extra) -> Centre
        {
            juce::StringArray openSets = extra; openSets.add (setOf (threshold.index, openNorm)); juce::StringArray hardSets = extra; hardSets.add (setOf (threshold.index, *hard));
            const auto open = response (tag + ".open", openSets), hardR = response (tag + ".hard", hardSets);
            Centre c; if (! open.ok || open.positions.empty() || ! hardR.ok || hardR.positions.empty()) { c.why = "the response did not run (" + open.refused + " / " + hardR.refused + ")"; return c; }
            return centreOf (eq::deviation (hardR.positions[0], open.positions[0]));
        };
        // the frequency control at three positions (or the unit as it is)
        juce::Array<juce::var> fr;
        std::vector<double> fnorms = freq.index >= 0 ? std::vector<double> { 0.0, 0.5, 1.0 } : std::vector<double> { -1.0 };
        for (double fn : fnorms)
        {
            juce::StringArray extra; if (fn >= 0.0) extra.add (setOf (freq.index, fn));
            const auto c = centreFor ("centre" + (fn >= 0.0 ? juce::String (fn, 2) : juce::String ("")), extra);
            juce::String text; if (fn >= 0.0) { const auto r = juce::File (raw.getChildFile (stem + ".deesser.centre" + juce::String (fn, 2) + ".hard.1.txt")).loadFileAsString(); for (const auto& line : juce::StringArray::fromLines (r)) { const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() > 2 && f[0] == "set" && f[1].getIntValue() == freq.index) for (int i = 2; i + 1 < f.size(); ++i) if (f[i] == "text") text = f[i + 1]; } }
            const auto lab = labelHz (text);
            if (fn >= 0.0) { roleevidence::Figure f; f.ok = true; if (c.ok) { f.centreHz = c.figureHz(); } if (fn < 0.01) freqA = f; if (fn > 0.99) freqB = f; }
            if (! c.ok) { say ("  centre" + (fn >= 0.0 ? " (" + freq.name + " = '" + text + "')" : juce::String()) + ": " + c.why); continue; }
            ++measured;
            say ("  centre" + (fn >= 0.0 ? " (" + freq.name + " = '" + text + "')" : juce::String ("")) + ": " + c.why + " -> " + modeWord (c) + (lab ? " | label " + juce::String (*lab, 0) + " Hz vs the " + (c.shape == "notch" ? "centre" : "corner") + " " + juce::String (c.figureHz(), 0) + ": " + juce::String (100.0 * (c.figureHz() - *lab) / *lab, 1) + " % off" : juce::String()));
            auto* ro = new juce::DynamicObject(); ro->setProperty ("freq_norm", fn >= 0.0 ? juce::var (fn) : juce::var()); ro->setProperty ("display", text); ro->setProperty ("label_hz", lab ? juce::var (*lab) : juce::var()); ro->setProperty ("shape", c.shape); ro->setProperty ("deepest_hz", std::round (c.hz)); ro->setProperty ("corner_hz", std::round (c.cornerHz)); ro->setProperty ("figure_hz", std::round (c.figureHz())); ro->setProperty ("depth_db", std::round (c.depthDb * 100.0) / 100.0); ro->setProperty ("at_997_db", std::round (c.at997Db * 100.0) / 100.0); ro->setProperty ("mode_read", modeWord (c)); fr.add (juce::var (ro));
        }
        o->setProperty ("centre", fr);
        // the mode switch, each text
        if (mode.index >= 0)
        {
            juce::Array<juce::var> mr;
            for (const auto& [text, norm] : mode.texts)
            {
                const auto c = centreFor ("mode" + juce::String (norm, 2), { setOf (mode.index, norm) });
                if (! c.ok) { say ("  " + mode.name + " = '" + text + "': " + c.why); continue; }
                ++measured;
                say ("  " + mode.name + " = '" + text + "': " + c.why + " -> " + modeWord (c));
                auto* ro = new juce::DynamicObject(); ro->setProperty ("display", text); ro->setProperty ("norm", norm); ro->setProperty ("shape", c.shape); ro->setProperty ("deepest_hz", std::round (c.hz)); ro->setProperty ("corner_hz", std::round (c.cornerHz)); ro->setProperty ("depth_db", std::round (c.depthDb * 100.0) / 100.0); ro->setProperty ("at_997_db", std::round (c.at997Db * 100.0) / 100.0); ro->setProperty ("mode_read", modeWord (c)); mr.add (juce::var (ro));
            }
            o->setProperty ("mode", mr);
        }
    }
    if (freq.index >= 0) roles.push_back (roleevidence::nominee (freq.index, freq.name, "frequency", roleevidence::signatureHolds ("frequency", freqA, freqB)));
    // the unnamed pool: one --sweep per control at its ends on the sibilance tone at -12 dBFS; GR between the ends = the threshold signature
    for (const auto& pc : unnamedPool (base, nominated, &fx.sampled))
    {
        const auto r = run ("u" + juce::String (pc.index), { "--sweep", "thr=" + juce::String (pc.index), "norms=0,1", "levels=-12", "hz=6500", "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" });
        if (r.kind == ChildResult::Kind::uiShown) { say ("DS: a window appeared; stopping"); return 5; }
        const auto m = sweep::parseSweep (r.cleanExit() ? r.out : juce::String());
        roleevidence::Figure a, b; for (const auto& pos : m.positions) { roleevidence::Figure* f = pos.norm < 0.01f ? &a : pos.norm > 0.99f ? &b : nullptr; if (! f) continue; for (const auto& [lk, h] : pos.holds) if (h.present && h.levelDb > -200.0) { f->ok = true; f->grDb = -(h.levelDb - h.inRmsDb); } }
        roles.push_back (roleevidence::unnamed (pc.index, pc.name, "threshold", roleevidence::signatureHolds ("threshold", a, b)));
    }
    sayRoles (say, roles, "unnamed controls probed for the threshold signature only");
    setRoles (o, roles, "unnamed controls probed for the threshold signature only (a frequency probe needs the hard threshold)");
    o->setProperty ("processes", processN); o->setProperty ("measuredAt", nowStamp());
    o->setProperty ("method", "ladder: probe --sweep with a 6.5 kHz sine (and 997 Hz as the control), the threshold at 6 norms, levels -30..-6 dBFS, GR = gain at the open end minus gain at the position; centre and mode: probe --response (121-tone multitone at -12 dBFS) at the hardest threshold against the open end, the deepest deviation's frequency (parabolic) is the centre; split_band = within 1 dB at 997 Hz while the band is cut 3 dB or more, wideband = 997 Hz cut within 1.5 dB of the band, else partial");
    outDir.getChildFile (stem + ".deesser.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("DS: -> " + outDir.getChildFile (stem + ".deesser.json").getFullPathName() + " (" + juce::String (processN) + " processes)");
    return measured > 0 ? 0 : 4;
}

// MULTIBAND PROTOTYPE (docs/MULTIBAND_PROFILE_PROPOSAL.md, 5 Oct R7, no spec change): --cert-multiband <product>. One tone per
// band at each band's centre (from the default crossovers), the band's threshold swept against it; the whole-unit figure on
// the vocal-shaped multitone at five levels; the amount = the global control where one exists, else every band threshold
// moved by one common dB offset (-24 .. 0 in 6 dB steps). Nothing exported.
inline int runMultiband (const SweepOptions& opt)
{
    using namespace multiband;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("MB: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.size() != 1) { say ("MB: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), opt.product, opt.retryLicence); known.isNotEmpty())
    { say ("MB: " + opt.product + " - " + known); return kToneLicenceKnownExit; }
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false); const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory(); auto outDir = opt.out.getChildFile ("multiband"); outDir.createDirectory();
    int processN = 0;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra) { juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runChild (args, opt.timeoutMs); ++processN; raw.getChildFile (stem + ".multiband." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r; };
    Subject s; s.product = opt.product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto fx = sampledFixture (opt, desc, raw, stem, "multiband", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash, { "threshold", "thresh", "thr", "crossover", "xover", "cross", "x-over", "freq", "frequency", "hz", "amount", "depth", "compression" });
    if (! fx.ok) { say ("MB: " + fx.why); return 1; }
    const auto base = fx.base; say ("MB: " + fx.note);
    // THE CONTROLS: band thresholds (threshold / thresh with a band word or number; never a sidechain "S" one), crossovers
    // (crossover / cross / xover / freq with low / high / a number), a global amount (amount / compression / depth; mix is not)
    struct Thr { int index; juce::String name, display0, display1, instantiate; double instNorm; };
    std::vector<Thr> thresholds; std::vector<juce::String> crossoverDisplays; juce::StringArray crossoverNames; int globalIdx = -1; juce::String globalName;
    auto answers = [] (const juce::String& name, std::initializer_list<const char*> terms) { for (const char* t : terms) if (nametokens::controlAnswersTerm (name, t)) return true; return false; };
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const int idx = (int) c.getProperty ("index", -1); const auto n = c.getProperty ("name", "").toString();
            if (sweep::wordValued (c) || (int) c.getProperty ("numSteps", 0) == 2 || sweep::neverTouchName (n)) continue;
            const auto at = c.getProperty ("displayAt", {}); const auto inst = c.getProperty ("defaultOnInstantiate", {});
            auto d0 = at.getProperty ("0.000", "").toString(), d1 = at.getProperty ("1.000", "").toString();
            if (answers (n, { "threshold", "thresh", "thr" }) && ! answers (n, { "s", "sc", "sidechain", "key" }))
                thresholds.push_back ({ idx, n, d0, d1, inst.getProperty ("display", "").toString(), (double) inst.getProperty ("normalised", 0.0) });
            else if (answers (n, { "crossover", "xover", "cross", "x-over" }) || (answers (n, { "freq", "frequency", "hz" }) && answers (n, { "low", "high", "mid", "band", "lo", "hi", "1", "2", "3", "4", "5" })))
            { crossoverDisplays.push_back (inst.getProperty ("display", "").toString()); crossoverNames.add (n); }
            else if (globalIdx < 0 && (answers (n, { "amount", "depth" }) || (answers (n, { "compression" }) && answers (n, { "globals", "global" })))) { globalIdx = idx; globalName = n; }
        }
    juce::StringArray skipped; const auto bands = crossoverDisplays.empty() ? std::vector<Band>() : bandsFromCrossovers (crossoverDisplays, skipped);
    say ("MB: " + opt.product + " " + desc.version + ": " + juce::String ((int) thresholds.size()) + " band threshold(s) [" + [&] { juce::StringArray a; for (const auto& t : thresholds) a.add (t.name + " @ '" + t.instantiate + "'"); return a.joinIntoString (", "); }() + "]; crossovers [" + crossoverNames.joinIntoString (", ") + "] -> " + juce::String ((int) bands.size()) + " band(s)" + (skipped.isEmpty() ? juce::String() : " (skipped: " + skipped.joinIntoString (", ") + ")") + "; global amount " + (globalIdx >= 0 ? "[" + juce::String (globalIdx) + "] " + globalName : juce::String ("none")));
    if (thresholds.empty()) { juce::StringArray names; if (const auto* cs = base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString()); say ("  no band thresholds by name; controls: " + names.joinIntoString (", ")); return 4; }
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_multiband_prototype/0"); o->setProperty ("status", "PROTOTYPE - the multiband proposal with real numbers, not exported, not published");
    o->setProperty ("product", opt.product); o->setProperty ("version", desc.version); o->setProperty ("identity", "AudioUnit|" + uidHex + "|" + desc.version);
    { juce::Array<juce::var> bv; for (const auto& b : bands) { auto* x = new juce::DynamicObject(); x->setProperty ("band", b.index); x->setProperty ("lo_hz", std::round (b.loHz)); x->setProperty ("hi_hz", std::round (b.hiHz)); x->setProperty ("centre_hz", std::round (b.centreHz)); bv.add (juce::var (x)); } o->setProperty ("bands", bv); o->setProperty ("crossover_controls", crossoverNames.joinIntoString (", ")); }
    auto setOf = [] (int idx, double norm) { return juce::String (idx) + ":" + juce::String (norm, 6); };
    juce::StringArray norms; for (int k = 0; k <= 5; ++k) norms.add (juce::String (k / 5.0f, 6));
    int measured = 0;
    std::vector<roleevidence::RoleVerdict> roles;
    // PAIRING BY MEASUREMENT (5 Oct evening): each nominated threshold gets one flat multitone response at its two ends; the region the
    // hard end cuts by 3 dB or more names the band it owns (its centre picks the band by the edges); a threshold that cuts nothing is
    // dropped here (Melda's gate / processor thresholds, C6's thresholds on tones that are not theirs)
    std::vector<int> pairedBand (thresholds.size(), -1); std::vector<juce::String> pairNote (thresholds.size());
    for (size_t i = 0; i < thresholds.size(); ++i)
    {
        const auto& t = thresholds[i];
        const auto r = run ("pair" + juce::String (t.index), { "--response", "ctl=" + juce::String (t.index), "norms=0,1", "tones=121", "lo=20", "hi=20000", "db=-12", "hold=1.0", "discard=0.5" });
        if (r.kind == ChildResult::Kind::uiShown) { say ("MB: a window appeared; stopping"); return 5; }
        const auto resp = eq::parseResponse (r.cleanExit() ? r.out : juce::String());
        if (! resp.ok || resp.positions.size() < 2) { pairNote[i] = "no response at both ends"; roles.push_back (roleevidence::nominee (t.index, t.name, "band_threshold", { false, pairNote[i] })); continue; }
        const auto d01 = eq::deviation (resp.positions[1], resp.positions[0]); double worst01 = 0.0; for (const auto& [f, d] : d01) worst01 = juce::jmin (worst01, d);
        const auto d10 = eq::deviation (resp.positions[0], resp.positions[1]); double worst10 = 0.0; for (const auto& [f, d] : d10) worst10 = juce::jmin (worst10, d);
        const auto& dev = worst01 <= worst10 ? d01 : d10; const double worst = juce::jmin (worst01, worst10);
        if (worst > -deesser::kBandCutDb) { pairNote[i] = "cuts nothing by " + juce::String (deesser::kBandCutDb, 0) + " dB between its ends on the multitone (deepest " + juce::String (worst, 2) + ")"; roles.push_back (roleevidence::nominee (t.index, t.name, "band_threshold", { false, pairNote[i] })); say ("  pairing [" + juce::String (t.index) + "] " + t.name + ": " + pairNote[i]); continue; }
        double lo = 0.0, hi = 0.0, sumLog = 0.0; int n = 0; for (const auto& [f, d] : dev) if (d <= worst / 2.0) { if (lo == 0.0) lo = f; hi = f; sumLog += std::log2 (f); ++n; }
        const double centre = std::pow (2.0, sumLog / juce::jmax (1, n));
        int best = -1; for (size_t k = 0; k < bands.size(); ++k) if (centre >= bands[k].loHz && centre < bands[k].hiHz) best = (int) k;
        pairedBand[i] = best; pairNote[i] = "cuts " + juce::String (lo, 0) + "-" + juce::String (hi, 0) + " Hz (deepest " + juce::String (worst, 1) + " dB, centre " + juce::String (centre, 0) + ")" + (best >= 0 ? " -> band " + juce::String (bands[(size_t) best].index) : juce::String (" -> no band holds that centre"));
        say ("  pairing [" + juce::String (t.index) + "] " + t.name + ": " + pairNote[i]);
    }
    // 1. PER-BAND LADDERS: each threshold against the tone at the centre of the band the MEASUREMENT paired it with
    {
        juce::Array<juce::var> lv;
        for (size_t i = 0; i < thresholds.size(); ++i)
        {
            if (pairedBand[i] < 0) continue;
            const auto& t = thresholds[i]; const auto& b = bands[(size_t) pairedBand[i]];
            const auto r = run ("band" + juce::String ((int) i + 1), { "--sweep", "thr=" + juce::String (t.index), "norms=" + norms.joinIntoString (","), "levels=-30,-24,-18,-12,-6", "hz=" + juce::String (b.centreHz, 0), "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" });
            if (r.kind == ChildResult::Kind::uiShown) { say ("MB: a window appeared; stopping"); return 5; }
            const auto L = deesser::ladderOf (sweep::parseSweep (r.cleanExit() ? r.out : juce::String()), b.centreHz);
            if (! L.ok) { say ("  band " + juce::String (b.index) + " (" + juce::String (b.centreHz, 0) + " Hz) " + t.name + ": " + L.why + " (" + r.describe() + ")"); continue; }
            ++measured;
            { roleevidence::Figure a, bb; for (const auto& c : L.cells) if (c.ok && std::abs (c.levelDbfs + 12.0) < 0.01) { roleevidence::Figure* f = c.norm < 0.01f ? &a : c.norm > 0.99f ? &bb : nullptr; if (f) { f->ok = true; f->grDb = c.grDb; } }
              roles.push_back (roleevidence::nominee (t.index, t.name, "band_threshold", roleevidence::signatureHolds ("band_threshold", a, bb))); }
            say ("  band " + juce::String (b.index) + " (" + juce::String (b.loHz, 0) + "-" + juce::String (b.hiHz, 0) + " Hz, tone " + juce::String (b.centreHz, 0) + ") " + t.name + ": " + L.why);
            juce::StringArray texts; for (const auto& c : L.cells) texts.addIfNotAlreadyThere (c.text);
            for (const auto& tx : texts) { juce::String line = "      '" + tx + "':"; for (const auto& c : L.cells) if (c.text == tx && c.ok) line << "  " << juce::String (c.levelDbfs, 0) << " dBFS GR " << juce::String (c.grDb, 2); say (line); }
            auto* lo = new juce::DynamicObject(); lo->setProperty ("band", b.index); lo->setProperty ("control", t.name); lo->setProperty ("index", t.index); lo->setProperty ("tone_hz", std::round (b.centreHz)); lo->setProperty ("max_gr_db", std::round (L.maxGrDb * 100.0) / 100.0); lo->setProperty ("note", L.why);
            juce::Array<juce::var> cells; for (const auto& c : L.cells) { auto* co = new juce::DynamicObject(); co->setProperty ("norm", c.norm); co->setProperty ("display", c.text); co->setProperty ("level_dbfs", c.levelDbfs); co->setProperty ("gr_db", std::round (c.grDb * 100.0) / 100.0); cells.add (juce::var (co)); } lo->setProperty ("pairing", pairNote[i]); lo->setProperty ("cells", cells); lv.add (juce::var (lo));
        }
        o->setProperty ("band_ladders", lv);
    }
    // 2. THE WHOLE UNIT on the vocal-shaped multitone: open (as instantiated) then the amount positions, five levels each
    auto response = [&] (const juce::String& tag, double levelDb, const juce::StringArray& sets) {
        juce::StringArray a { "--response", "ctl=" + juce::String (thresholds[0].index), "norms=current", "tones=121", "lo=20", "hi=20000", "shape=vocal", "db=" + juce::String (levelDb, 0), "hold=1.5", "discard=0.75" }; if (! sets.isEmpty()) a.add ("set=" + sets.joinIntoString (","));
        const auto r = run (tag, a); const auto p = eq::parseResponse (r.cleanExit() ? r.out : juce::String()); return p.ok && ! p.positions.empty() ? totalGainDb (p.positions[0]) : std::nullopt; };
    const std::vector<double> levels { -30.0, -24.0, -18.0, -12.0, -6.0 };
    std::map<double, double> openGain;
    for (double L : levels) if (const auto g = response ("open.L" + juce::String ((int) -L), L, {})) openGain[L] = *g;
    if (openGain.empty()) say ("  whole unit: the vocal-shaped response did not run at the instantiate state");
    else
    {
        juce::String line = "  whole unit, as instantiated (open): gain"; for (const auto& [L, g] : openGain) line << "  " << juce::String (L, 0) << " dBFS " << juce::String (g, 2); say (line);
        juce::Array<juce::var> pts;
        roleevidence::Figure globalA, globalB;
        if (globalIdx >= 0)
        {
            // (a) the global control walked at six norms
            for (int k = 0; k <= 5; ++k)
            {
                const double norm = k / 5.0; OffsetPoint pt; pt.offsetDb = norm;
                for (double L : levels) if (const auto g = response ("global" + juce::String (k) + ".L" + juce::String ((int) -L), L, { setOf (globalIdx, norm) })) if (openGain.count (L)) pt.grByLevel[L] = openGain[L] - *g;
                juce::String text; { const auto r = raw.getChildFile (stem + ".multiband.global" + juce::String (k) + ".L12.1.txt").loadFileAsString(); for (const auto& l : juce::StringArray::fromLines (r)) { const auto f = juce::StringArray::fromTokens (l, "\t", ""); if (f.size() > 2 && f[0] == "set" && f[1].getIntValue() == globalIdx) for (int i = 2; i + 1 < f.size(); ++i) if (f[i] == "text") text = f[i + 1]; } }
                pt.displays = text; if (! pt.grByLevel.empty()) ++measured;
                { roleevidence::Figure f; f.ok = true; if (pt.grByLevel.count (-12.0)) f.grDb = pt.grByLevel.at (-12.0); if (k == 0) globalA = f; if (k == 5) globalB = f; }
                juce::String l2 = "  " + globalName + " = '" + text + "' (norm " + juce::String (norm, 2) + "): whole-unit GR"; for (const auto& [L, g] : pt.grByLevel) l2 << "  " << juce::String (L, 0) << " dBFS " << juce::String (g, 2); say (l2);
                auto* po = new juce::DynamicObject(); po->setProperty ("norm", norm); po->setProperty ("display", text); auto* gr = new juce::DynamicObject(); for (const auto& [L, g] : pt.grByLevel) gr->setProperty (juce::String (L, 0), std::round (g * 100.0) / 100.0); po->setProperty ("gr_db_by_level", juce::var (gr)); pts.add (juce::var (po));
            }
            auto* m = new juce::DynamicObject(); m->setProperty ("topology", "multiband_global"); m->setProperty ("control", globalName); m->setProperty ("index", globalIdx); m->setProperty ("points", pts); o->setProperty ("amount", juce::var (m));
            roles.push_back (roleevidence::nominee (globalIdx, globalName, "global", roleevidence::signatureHolds ("global", globalA, globalB)));
        }
        else
        {
            // (b) a common dB offset on every band threshold, from each control's own dB range (both ends numeric) and instantiate value
            std::vector<DbRange> ranges; bool allOk = true;
            for (const auto& t : thresholds) { ranges.push_back (dbRangeOf (t.display0, t.display1)); if (! ranges.back().ok) { allOk = false; say ("  " + t.name + ": its ends '" + t.display0 + "' / '" + t.display1 + "' are not both numbers: no dB offset can be written to it"); } }
            if (allOk)
            {
                for (double off : { 0.0, -6.0, -12.0, -18.0, -24.0 })
                {
                    juce::StringArray sets; juce::StringArray displays;
                    for (size_t i = 0; i < thresholds.size(); ++i) { const auto inst = dbRangeOf (thresholds[i].instantiate, thresholds[i].display1); const double instDb = inst.ok ? inst.at0 : ranges[i].at0 + thresholds[i].instNorm * (ranges[i].at1 - ranges[i].at0); sets.add (setOf (thresholds[i].index, normForDb (ranges[i], instDb + off))); }
                    OffsetPoint pt; pt.offsetDb = off;
                    for (double L : levels) if (const auto g = response ("off" + juce::String ((int) -off) + ".L" + juce::String ((int) -L), L, sets)) if (openGain.count (L)) pt.grByLevel[L] = openGain[L] - *g;
                    { const auto r = raw.getChildFile (stem + ".multiband.off" + juce::String ((int) -off) + ".L12.1.txt").loadFileAsString(); for (const auto& l : juce::StringArray::fromLines (r)) { const auto f = juce::StringArray::fromTokens (l, "\t", ""); if (f.size() > 2 && f[0] == "set") for (int i = 2; i + 1 < f.size(); ++i) if (f[i] == "text") displays.add (f[i + 1]); } }
                    pt.displays = displays.joinIntoString (" / "); if (! pt.grByLevel.empty()) ++measured;
                    juce::String l2 = "  offset " + juce::String (off, 0) + " dB on every band (" + pt.displays + "): whole-unit GR"; for (const auto& [L, g] : pt.grByLevel) l2 << "  " << juce::String (L, 0) << " dBFS " << juce::String (g, 2); say (l2);
                    auto* po = new juce::DynamicObject(); po->setProperty ("offset_db", off); po->setProperty ("displays", pt.displays); auto* gr = new juce::DynamicObject(); for (const auto& [L, g] : pt.grByLevel) gr->setProperty (juce::String (L, 0), std::round (g * 100.0) / 100.0); po->setProperty ("gr_db_by_level", juce::var (gr)); pts.add (juce::var (po));
                }
                auto* m = new juce::DynamicObject(); m->setProperty ("topology", "multiband_offset"); juce::StringArray names; for (const auto& t : thresholds) names.add (t.name); m->setProperty ("controls", names.joinIntoString (", ")); m->setProperty ("points", pts); o->setProperty ("amount", juce::var (m));
            }
        }
        auto* og = new juce::DynamicObject(); for (const auto& [L, g] : openGain) og->setProperty (juce::String (L, 0), std::round (g * 100.0) / 100.0); o->setProperty ("open_gain_db_by_level", juce::var (og));
    }
    sayRoles (say, roles, "unnamed controls not probed in this mode (the pairing response per nominee is the measurement step)");
    setRoles (o, roles, "unnamed controls not probed; every nominated threshold got a pairing response");
    o->setProperty ("processes", processN); o->setProperty ("measuredAt", nowStamp());
    o->setProperty ("method", "bands from the crossover controls' instantiate displays (20 .. x1 .. xn .. 20000 Hz, geometric centres); per band: probe --sweep at the centre, that band's threshold at 6 norms, levels -30..-6, GR = open-end gain minus the position's gain; whole unit: probe --response shape=vocal (121 tones, pink below 1 kHz, -12 dB/oct below 100 Hz, -6 dB/oct more above 1 kHz) at five levels, gain = total output power over total input power, GR against the instantiate state; amount = the global control at 6 norms where one exists, else a common dB offset (0 .. -24) written to every band threshold from each control's own dB ends (linear in dB, said)");
    outDir.getChildFile (stem + ".multiband.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("MB: -> " + outDir.getChildFile (stem + ".multiband.json").getFullPathName() + " (" + juce::String (processN) + " processes)");
    return measured > 0 ? 0 : 4;
}

// THE PHASE B BATCH (5 Oct evening): --phaseb-all [--out <cert>] [--probe <path>] [--ejmap-ledger <dir>] [--category <name>]... [--only <product>]...
// and --phaseb-status [--out <cert>]. See EjmapPhaseB.h for the rules. Results: <cert>/phaseb/<category>/<mode>/<stem>.<mode>.json
// (the mode's record), <cert>/phaseb/<category>/raw/*.txt.gz (its traces, gzipped), <cert>/phaseb/<category>/<stem>.phaseb.json
// (the batch's row - THE DONE MARKER, written last by rename), <cert>/phaseb/progress.{json,txt}, <cert>/phaseb/summary.json.
using phaseb::writeAtomic; using phaseb::gzipInto;
struct PhaseBProduct { juce::String product, stem, category; juce::PluginDescription desc; juce::String recordFile; };
inline int runPhaseBAll (const SweepOptions& opt, const juce::StringArray& onlyCategories, const juce::StringArray& onlyProducts)
{
    using namespace phaseb;
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("PHASEB: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const auto phasebDir = opt.out.getChildFile ("phaseb"); phasebDir.createDirectory();
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    // DISCOVERY: installed AUs by category from the ledger; certified compressors from the cert folder; multibands from its outcomes
    const auto in = loadDiscoveryInputs (opt.ledger);
    const auto installed = installedAudioUnits();
    const auto scanStops = quarantinedBundles (opt.ledger);
    const auto outcomes = juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString());
    std::map<juce::String, std::vector<PhaseBProduct>> work;
    auto stemFor = [] (const juce::PluginDescription& d) { return "AudioUnit_" + juce::String::toHexString (d.uniqueId) + "_" + d.version; };
    for (const auto& cat : categories())
    {
        if (! onlyCategories.isEmpty() && ! onlyCategories.contains (cat.name)) continue;
        std::vector<PhaseBProduct> list;
        if (cat.name == "gaincal" || cat.name == "timing")
        {
            // every certified compressor: an exported row, or a record whose sweep is certified
            if (const auto* a = outcomes.getArray()) for (const auto& row : *a)
            {
                if (row.getProperty ("state", "").toString() != "exported") continue;
                const auto product = row.getProperty ("product", "").toString();
                for (const auto& ir : installed) if (ir.desc.name == product) { PhaseBProduct pp; pp.product = product; pp.stem = stemFor (ir.desc); pp.category = cat.name; pp.desc = ir.desc; pp.recordFile = latestRecordFor (opt.out.getChildFile ("fixtures"), product).getFullPathName(); list.push_back (pp); break; }
            }
        }
        else if (cat.name == "multiband")
        {
            if (const auto* a = outcomes.getArray()) for (const auto& row : *a)
            {
                if (row.getProperty ("state", "").toString() != "multiband") continue;
                const auto product = row.getProperty ("product", "").toString();
                for (const auto& ir : installed) if (ir.desc.name == product) { PhaseBProduct pp; pp.product = product; pp.stem = stemFor (ir.desc); pp.category = cat.name; pp.desc = ir.desc; list.push_back (pp); break; }
            }
        }
        else
        {
            for (const auto& ir : installed)
            {
                const auto it = in.categoryByUid.find (ir.uidKey); if (it == in.categoryByUid.end()) continue;
                if (! cat.ledgerCategories.contains (it->second)) continue;
                PhaseBProduct pp; pp.product = ir.desc.name; pp.stem = stemFor (ir.desc); pp.category = cat.name; pp.desc = ir.desc; list.push_back (pp);
            }
        }
        if (! onlyProducts.isEmpty()) { std::vector<PhaseBProduct> f; for (const auto& pp : list) if (onlyProducts.contains (pp.product)) f.push_back (pp); list = f; }
        std::sort (list.begin(), list.end(), [] (const PhaseBProduct& a, const PhaseBProduct& b) { return a.product.compareIgnoreCase (b.product) < 0; });
        work[cat.name] = list;
    }
    // PROGRESS: resumed from the file (the elapsed and the measured seconds carry over); the totals are tonight's discovery
    Progress prog = progressFromVar (juce::JSON::parse (phasebDir.getChildFile ("progress.json").loadFileAsString()));
    if (prog.startedAt.isEmpty()) prog.startedAt = nowStamp();
    const double elapsedBefore = prog.elapsedS; const auto t0 = juce::Time::getMillisecondCounterHiRes();
    int totalAll = 0;
    for (const auto& cat : categories()) if (work.count (cat.name))
    {
        auto& c = prog.cats[cat.name]; c.total = (int) work[cat.name].size(); c.done = 0; c.ok = c.timedOut = c.failed = c.skipped = 0;
        for (const auto& pp : work[cat.name]) if (isDone (phasebDir, cat.name, pp.stem)) { ++c.done; const auto row = juce::JSON::parse (rowFile (phasebDir, cat.name, pp.stem).loadFileAsString()); const auto oc = row.getProperty ("outcome", "").toString(); if (oc == "ok") ++c.ok; else if (oc == "timed_out") ++c.timedOut; else if (oc == "skipped") ++c.skipped; else ++c.failed; }
        totalAll += c.total;
    }
    auto saveProgress = [&] (const juce::String& current)
    {
        prog.current = current; prog.updatedAt = nowStamp(); prog.elapsedS = elapsedBefore + (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        writeAtomic (phasebDir.getChildFile ("progress.json"), juce::JSON::toString (progressVar (prog)));
        writeAtomic (phasebDir.getChildFile ("progress.txt"), progressText (prog));
    };
    say ("PHASEB: " + juce::String (totalAll) + " product(s) over " + juce::String ((int) work.size()) + " categor" + (work.size() == 1 ? "y" : "ies") + " -> " + phasebDir.getFullPathName() + "  (resumable; Ctrl-C any time; a product's result lands only when it is complete)");
    for (const auto& cat : categories()) if (work.count (cat.name)) say ("  " + cat.name.paddedRight (' ', 11) + juce::String ((int) work[cat.name].size()).paddedLeft (' ', 3) + " product(s), " + juce::String (prog.cats[cat.name].done) + " already done; hang guard " + juce::String (cat.guardS / 60.0, 0) + " min (" + cat.guardWhy + ")");
    saveProgress ({});
    // THE RUN: one child ejmap process per product into a temp folder, renamed into place when complete
    for (const auto& cat : categories())
    {
        if (! work.count (cat.name)) continue;
        for (const auto& pp : work[cat.name])
        {
            if (isDone (phasebDir, cat.name, pp.stem)) continue;
            auto& c = prog.cats[cat.name];
            const auto catDir = phasebDir.getChildFile (cat.name); catDir.createDirectory();
            const auto tmp = catDir.getChildFile (".tmp-" + pp.stem);
            // a half-done folder from an interrupted run is thrown away: the product starts again. A parent killed outright (SIGKILL,
            // a crash) leaves its child measuring into that folder: the child is named by the folder in its arguments, killed first.
            if (tmp.isDirectory()) { juce::ChildProcess pk; pk.start (juce::StringArray { "/usr/bin/pkill", "-f", tmp.getFullPathName() }); pk.waitForProcessToFinish (5000); }
            tmp.deleteRecursively(); tmp.createDirectory();
            saveProgress (cat.name + ": " + pp.product);
            auto* row = new juce::DynamicObject(); row->setProperty ("product", pp.product); row->setProperty ("identity", "AudioUnit|" + juce::String::toHexString (pp.desc.uniqueId) + "|" + pp.desc.version); row->setProperty ("category", cat.name); row->setProperty ("mode", cat.mode);
            juce::String outcome; double seconds = 0.0;
            // licence: the scan's stop, or a needs_licence row in this folder - never loaded
            if (const auto known = loop::knownLicenceStop (scanStops, outcomes, pp.product, false); known.isNotEmpty()) { outcome = "skipped"; row->setProperty ("reason", "licence: " + known); }
            else
            {
                if (cat.name == "timing" && pp.recordFile.isNotEmpty()) { tmp.getChildFile ("fixtures").createDirectory(); juce::File (pp.recordFile).copyFileTo (tmp.getChildFile ("fixtures").getChildFile (juce::File (pp.recordFile).getFileName())); }
                juce::StringArray args { exe.getFullPathName(), cat.mode, pp.product };
                if (cat.kindArg.isNotEmpty()) { args.add ("--kind"); args.add (cat.kindArg); }
                args.addArray ({ "--out", tmp.getFullPathName(), "--probe", opt.probe.getFullPathName(), "--ejmap-ledger", opt.ledger.getFullPathName() });
                const auto t1 = juce::Time::getMillisecondCounterHiRes();
                const auto r = runChild (args, (int) (cat.guardS * 1000.0));
                seconds = (juce::Time::getMillisecondCounterHiRes() - t1) / 1000.0;
                tmp.getChildFile ("log.txt").replaceWithText (r.out, false, false, "\n");
                const bool slept = r.sleptMs > kSleptMs;
                if (r.kind == ChildResult::Kind::timedOut) outcome = "timed_out";
                else if (r.kind == ChildResult::Kind::uiShown) outcome = "window";
                else if (r.kind == ChildResult::Kind::exited && (r.code == 0 || r.code == 4)) outcome = slept ? "slept" : "ok";   // 4 = the mode found nothing to measure (said in its log)
                else outcome = "failed";
                row->setProperty ("child", r.describe()); row->setProperty ("exit_code", r.code); row->setProperty ("slept_ms", r.sleptMs);
                if (slept) row->setProperty ("reason", "the Mac slept during the measurement (" + juce::String (r.sleptMs / 1000.0, 1) + " s): recorded, the data is not trusted; delete this row to re-run");
                // the mode's record(s) and raw traces, moved into place (the raw gzipped); the row is written LAST
                juce::StringArray records; int rawN = 0;
                for (const auto& d : tmp.findChildFiles (juce::File::findDirectories, false))
                {
                    if (d.getFileName() == "raw") { const auto rawDir = catDir.getChildFile ("raw"); rawDir.createDirectory(); for (const auto& f : d.findChildFiles (juce::File::findFiles, false)) { gzipInto (f, rawDir); ++rawN; } continue; }
                    if (d.getFileName() == "fixtures") continue;
                    const auto dst = catDir.getChildFile (d.getFileName()); dst.createDirectory();
                    for (const auto& f : d.findChildFiles (juce::File::findFiles, false, "*.json")) { const auto target = dst.getChildFile (f.getFileName()); target.deleteFile(); f.moveFileTo (target); records.add (d.getFileName() + "/" + f.getFileName()); }
                }
                { const auto lg = catDir.getChildFile ("logs"); lg.createDirectory(); const auto target = lg.getChildFile (pp.stem + ".log.txt"); target.deleteFile(); tmp.getChildFile ("log.txt").moveFileTo (target); }
                row->setProperty ("records", records); row->setProperty ("raw_files", rawN);
                if (outcome == "timed_out") row->setProperty ("reason", "hang guard " + juce::String (cat.guardS / 60.0, 0) + " min reached: partial data kept (" + juce::String (rawN) + " trace(s)), the record " + (records.isEmpty() ? juce::String ("not written") : juce::String ("written")));
            }
            row->setProperty ("outcome", outcome); row->setProperty ("seconds", std::round (seconds)); row->setProperty ("at", nowStamp()); row->setProperty ("probe", id.cdhash);
            writeAtomic (rowFile (phasebDir, cat.name, pp.stem), juce::JSON::toString (juce::var (row)));   // the DONE marker, whole or absent
            tmp.deleteRecursively();
            ++c.done; if (outcome == "ok") c.ok++; else if (outcome == "timed_out") c.timedOut++; else if (outcome == "skipped") c.skipped++; else c.failed++;
            if (outcome != "skipped") c.seconds.push_back (seconds);
            saveProgress ({});
            say (progressLine (prog, cat.name, pp.product, outcome, seconds));
        }
    }
    saveProgress ({});
    // THE SUMMARY: every row, by category
    { auto* sm = new juce::DynamicObject(); auto* cats = new juce::DynamicObject();
      for (const auto& cat : categories()) if (work.count (cat.name)) { juce::Array<juce::var> rows; for (const auto& pp : work[cat.name]) if (isDone (phasebDir, cat.name, pp.stem)) rows.add (juce::JSON::parse (rowFile (phasebDir, cat.name, pp.stem).loadFileAsString())); cats->setProperty (cat.name, rows); }
      sm->setProperty ("categories", juce::var (cats)); sm->setProperty ("progress", progressVar (prog)); sm->setProperty ("probe", id.cdhash); sm->setProperty ("writtenAt", nowStamp());
      writeAtomic (phasebDir.getChildFile ("summary.json"), juce::JSON::toString (juce::var (sm))); }
    say ("PHASEB: done - " + progressText (prog).upToFirstOccurrenceOf ("\n", false, false) + "; summary " + phasebDir.getChildFile ("summary.json").getFullPathName());
    return 0;
}
inline int runPhaseBStatus (const SweepOptions& opt)
{
    const auto f = opt.out.getChildFile ("phaseb").getChildFile ("progress.json");
    if (! f.existsAsFile()) { std::cout << "PHASEB STATUS: nothing yet at " << f.getFullPathName() << " (run --phaseb-all first)" << std::endl; return 2; }
    std::cout << phaseb::progressText (phaseb::progressFromVar (juce::JSON::parse (f.loadFileAsString())));
    return 0;
}

// THE INERT CHECK ON AN EXISTING RECORD (the follow-up, 4 Oct): a single-sweep record filed flat BECAUSE it passes audio
// through at its defaults, with no inert check yet, gets the check's processes now; inert -> the record's result and reason
// change, nothing else. Returns what happened, empty when the record is not that shape.
inline juce::String inertCheckOnRecord (const SweepOptions& opt, const juce::File& recordFile, bool deriveOnly)
{
    auto record = juce::JSON::parse (recordFile.loadFileAsString());
    auto sw = record.getProperty ("thresholdSweep", {});
    // any flat single sweep with no engage write found (V76U73's main sweep says flat without the passThroughAtDefaults flag,
    // which only its quick engage sweeps carried), not yet checked
    if (! sw.isObject() || sw.getProperty ("result", "").toString() != "flat" || (bool) sw.getProperty ("engageWrites", {}).getProperty ("found", false) || sw.hasProperty ("inertCheck")) return {};
    const auto product = record.getProperty ("product", "").toString();
    if (deriveOnly) return "would run the inert check (flat, no engage write found, not yet checked)";
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == product) hits.push_back (r);
    if (hits.size() != 1) return "inert check not run: '" + product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s)";
    const auto& desc = hits[0].desc;
    if (const auto vm = profile::versionMismatch (record.getProperty ("version", "").toString(), desc.version); vm.isNotEmpty()) return "inert check not run: " + vm;
    int thr = (int) sw.getProperty ("sweptControl", {}).getProperty ("index", (int) sw.getProperty ("thresholdPick", {}).getProperty ("index", -1));
    if (thr < 0) { const auto plan = sweep::planFromFixture (record); thr = plan.thr; }
    if (thr < 0) return "inert check not run: the record names no swept control";
    const auto norms = sw.getProperty ("positionNorms", {}); const float norm = norms.size() > 0 ? (float) (double) norms[0] : 0.0f;
    juce::StringArray sets; for (const auto& [i, n] : sidechaincheck::recordWrites (sw)) sets.add (juce::String (i) + ":" + juce::String (n, 6));
    const auto stem = recordFile.getFileNameWithoutExtension();
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory();
    const auto ir = runInertCheck (record, thr, norm, sets, [&] (const juce::String& tag, const juce::StringArray& a)
    {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (a);
        const auto r = runChild (args, opt.timeoutMs);
        raw.getChildFile (stem + ".followup." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n");
        return r;
    });
    auto* so = sw.getDynamicObject();
    auto* ic = new juce::DynamicObject(); ic->setProperty ("inert", ir.inert); ic->setProperty ("control_db", ir.controlDb); ic->setProperty ("reason", ir.reason); ic->setProperty ("readAt", juce::Time::getCurrentTime().toISO8601 (false));
    juce::Array<juce::var> tried;
    for (const auto& t : ir.tried) { auto* o = new juce::DynamicObject(); o->setProperty ("index", t.index); o->setProperty ("control", t.name); o->setProperty ("norm", t.norm); o->setProperty ("from", t.fromDisplay);
                                     if (t.ran) o->setProperty ("after_db", t.afterDb); else o->setProperty ("not_run", t.note); tried.add (juce::var (o)); }
    ic->setProperty ("tried", tried);
    so->setProperty ("inertCheck", juce::var (ic));
    if (ir.inert) { so->setProperty ("result", "inert"); so->setProperty ("reason", ir.reason); }
    record.getDynamicObject()->setProperty ("thresholdSweep", sw);
    recordFile.replaceWithText (juce::JSON::toString (record) + "\n", false, false, "\n");
    return (ir.inert ? "INERT: " : "inert check: ") + ir.reason;
}

struct SidechainCheckResult { bool inSet = false, resweep = false, ran = false; juce::String why; };
inline SidechainCheckResult sidechainPolicyCheck (const SweepOptions& opt, const juce::File& recordFile, bool deriveOnly)
{
    using namespace sidechaincheck;
    SidechainCheckResult res;
    auto record = juce::JSON::parse (recordFile.loadFileAsString());
    const auto product = record.getProperty ("product", "").toString();
    if (record.getProperty ("schema", "").toString() == sweep::kSchemaTuner) return res;
    // the sweep view and the trace prefix: a single sweep, or the decided / first certified / first candidate
    juce::var view = record.getProperty ("thresholdSweep", {}); juce::String prefix, candidateName;
    if (! view.isObject()) if (const auto* cs = record.getProperty ("thresholdCandidates", {}).getArray(); cs != nullptr && ! cs->isEmpty())
    {
        const int picked = (int) record.getProperty ("pickedCandidate", {}).getProperty ("index", -1);
        const juce::var* chosen = nullptr;
        for (const auto& c : *cs) if ((int) c.getProperty ("index", -1) == picked) chosen = &c;
        if (chosen == nullptr) for (const auto& c : *cs) if (chosen == nullptr && c.getProperty ("thresholdSweep", {}).getProperty ("result", "").toString() == "certified") chosen = &c;
        if (chosen == nullptr) chosen = &cs->getReference (0);
        view = chosen->getProperty ("thresholdSweep", {}); prefix = ".c" + juce::String ((int) chosen->getProperty ("index", -1)) + "."; candidateName = chosen->getProperty ("name", "").toString();
    }
    if (! view.isObject()) return res;
    // the record's own evidence FIRST (a record swept under the new policy, or with no second bus, is not in the set) - before
    // any verdict cached by an earlier run, which the re-sweep carries over and which would otherwise re-sweep the record every run
    if (sweptUnderPolicyNow (view)) return res;
    if (const auto sc = view.getProperty ("sidechain", {}); sc.isObject() && sc.getProperty ("extraInputBuses", {}).size() == 0) return res;
    // already decided for this policy on an earlier run
    if (const auto prev = record.getProperty ("sidechainPolicyCheck", {}); prev.isObject() && prev.getProperty ("policyNow", "").toString() == kPolicyNow)
    { res.inSet = true; res.resweep = prev.getProperty ("verdict", "").toString() == "resweep"; res.why = prev.getProperty ("why", "").toString() + " (from an earlier run)"; return res; }
    // IN THE SET (Kathy's ruling 2, 6 Oct): swept under an earlier policy with an extra input declared. The A/B itself runs
    // inside the tone check, at the check's own pick with its full write list (runToneCheck), and its verdict lands on the
    // record as sidechainPolicyCheck; the follow-up reads that verdict after the check and re-sweeps on "resweep".
    res.inSet = true;
    { const auto sc = view.getProperty ("sidechain", {}); juce::StringArray buses; if (const auto* eb = sc.getProperty ("extraInputBuses", {}).getArray()) for (const auto& b : *eb) buses.add (b.getProperty ("name", "").toString());
      const auto pol = sc.getProperty ("policy", "").toString();
      if (switchFor (pol).isEmpty() && pol.isNotEmpty()) { res.why = "sidechain policy: swept under '" + pol + "', which the probe cannot reproduce for an A/B; record kept"; return res; }
      res.why = "sidechain policy: swept under '" + (pol.isEmpty() ? juce::String (kPolicyOld) : pol) + "' (" + buses.joinIntoString (", ") + ") - the A/B runs at the tone check's pick, g = 2, under '" + kPolicyNow + "'" + (deriveOnly ? " (derive-only: not run)" : ""); }
    (void) deriveOnly; (void) opt; (void) product; (void) prefix; (void) candidateName;
    return res;
}

inline int runToneCheckAll (SweepOptions opt)
{
    const auto fixturesDir = opt.out.getChildFile ("fixtures");
    const auto outcomesFile = opt.out.getChildFile ("outcomes.json");
    auto outcomes = juce::JSON::parse (outcomesFile.loadFileAsString()); if (! outcomes.isArray()) outcomes = juce::Array<juce::var>();
    auto writeOutcomes = [&] { outcomesFile.replaceWithText (juce::JSON::toString (outcomes) + "\n", false, false, "\n"); };
    if (! opt.deriveOnly) { const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { std::cout << "TONECHECK-ALL: ABORTED BEFORE ANY PLUGIN - " << id.why << std::endl; return 3; } }
    const auto scanStops = quarantinedBundles (opt.ledger);
    const auto reviewPicks = juce::JSON::parse (opt.out.getChildFile ("review_picks.json").loadFileAsString());
    const auto ilokNow = iLokPresence();
    std::cout << "TONECHECK-ALL: " << opt.out.getFullPathName() << "  iLok " << ilokNow << (reviewPicks.isArray() ? "  review picks " + juce::String (reviewPicks.size()) : juce::String ("  no review_picks.json")) << std::endl;
    int done = 0, skipped = 0, licence = 0, failed = 0, noTraces = 0;
    // THE RE-SWEEP PASS (ruled 4 Oct): every record whose plan under this build differs from the plan it was swept under is
    // re-swept, through the batch's own per-product sweep, then finished (export + tone check) like any other; Sean never
    // names a product. In --derive-only the list is projected and nothing runs. Licence rows are --retry-licence's, not this.
    // Decided BEFORE the re-derive loop so a stale sweep is never tone-checked (or exported) minutes before its replacement.
    // Only a product's LATEST record speaks for it (an older record of a product that was later swept is history).
    juce::StringArray resweep; juce::StringArray resweepWhys; int scChecked = 0;
    {
        juce::StringArray products;
        for (const auto& recordFile : fixturesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
            if (! recordFile.getFileName().endsWith (".defaults.json")) products.addIfNotAlreadyThere (juce::JSON::parse (recordFile.loadFileAsString()).getProperty ("product", "").toString());
        for (const auto& product : products)
        {
            if (product.isEmpty() || (! opt.slice.isEmpty() && ! opt.slice.contains (product))) continue;
            auto r = juce::JSON::parse (latestRecordFor (fixturesDir, product).loadFileAsString());
            if (loop::outcomeForRecord (r).state == "needs_licence") continue;
            // THE LANDING READ FIRST (Sean's stepped rule, 4 Oct): a continuous-declared amount control is read at 41 norms once,
            // so the plan below sweeps a stepped control at its detents the first time, never at sixteen positions over six
            if (! opt.deriveOnly && ! r.hasProperty ("amountLanding") && r.getProperty ("schema", "").toString() != kSchemaTuner)
            {
                auto plan0 = sweep::planFromFixture (r);
                if (r.getProperty ("pickedCandidate", {}).isObject()) { const int want = (int) r.getProperty ("pickedCandidate", {}).getProperty ("index", -1); for (const auto& c : plan0.candidates) if (c.index == want) { auto cc = c; plan0 = plan0.forCandidate (cc); break; } }
                if (plan0.ok && plan0.thr >= 0 && ! sweep::isSteppedControl (sweep::findControl (r, plan0.thr)))
                {
                    std::vector<InstalledRecord> hits; for (const auto& ir : installedAudioUnits()) if (ir.desc.name == product) hits.push_back (ir);
                    if (hits.size() == 1 && profile::versionMismatch (r.getProperty ("version", "").toString(), hits[0].desc.version).isEmpty())
                    {
                        const auto recF = latestRecordFor (fixturesDir, product);
                        const auto lr = readAmountLanding (opt, hits[0].desc, r, recF, plan0.thr);
                        if (lr.ran) std::cout << "  " << product << ": landing [" << plan0.thr << "] " << plan0.thrName << ": " << lr.note << std::endl;
                        r = juce::JSON::parse (recF.loadFileAsString());
                    }
                }
            }
            const auto d = loop::planDiffers (r, sweep::planFromFixture (r));
            if (d.resweep) { resweep.add (product); resweepWhys.add (product + ": " + d.why); if (opt.projection) opt.projection->resweeps.push_back ({ product, d.why, false }); continue; }
            // THE SIDECHAIN POLICY (4 Oct): one reading under the new policy against the record's own, for every record swept
            // under the old policy that declares a second input bus; the re-sweep is by that evidence, never by assumption
            const auto sc = sidechainPolicyCheck (opt, latestRecordFor (fixturesDir, product), opt.deriveOnly);
            if (sc.inSet) { ++scChecked; std::cout << "  " << product << ": " << sc.why << std::endl; if (opt.projection) opt.projection->sidechain.push_back ({ product, sc.why, false }); }
            if (sc.resweep) { resweep.add (product); resweepWhys.add (product + ": " + sc.why); continue; }
            // THE INERT CHECK (4 Oct): a flat record that passes audio through at its defaults is asked whether anything moves its output
            if (const auto w = inertCheckOnRecord (opt, latestRecordFor (fixturesDir, product), opt.deriveOnly); w.isNotEmpty()) { std::cout << "  " << product << ": " << w << std::endl; if (opt.projection && w.startsWith ("would run")) opt.projection->inert.add (product); }
        }
    }
    std::cout << "SIDECHAIN POLICY CHECK: " << scChecked << " product(s) swept under " << sidechaincheck::kPolicyOld << " with a second input bus" << (opt.deriveOnly ? " (derive-only: nothing read)" : "") << std::endl;
    std::cout << "RE-SWEEP (the plan under this build differs from the plan the record was swept under): " << resweep.size() << " product(s) before the re-derive (a measured pick that still needs refinement joins after its re-derive)" << std::endl;
    for (const auto& w : resweepWhys) std::cout << "  " << w << std::endl;
    // EVERY RECORD IN THE FOLDER'S fixtures/ that is a certified profile (or Rule 1-decided), by its own file - never a path
    // from a row, which may have been written on another Mac (a zipped-back folder); imported traces (CL 1B, section 11) count.
    for (const auto& recordFile : fixturesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (recordFile.getFileName().endsWith (".defaults.json")) continue;
        auto rec0 = juce::JSON::parse (recordFile.loadFileAsString());
        const auto product = rec0.getProperty ("product", "").toString();
        if (! opt.slice.isEmpty() && ! opt.slice.contains (product)) continue;
        if (resweep.contains (product)) continue;   // its sweep is stale under this build: re-swept and finished below, not tone-checked here
        {
            // THE REVIEW PICK (ruled 4 Oct): applied to the record on disk before anything else reads it, so the re-derive carries it
            auto withPick = rec0; const auto what = loop::applyReviewPick (withPick, reviewPicks);
            if (what.isNotEmpty()) { std::cout << "  " << product << ": " << what << std::endl; if (what.startsWith ("review pick applied")) { recordFile.replaceWithText (juce::JSON::toString (withPick) + "\n", false, false, "\n"); rec0 = withPick; } }
        }
        // A CANDIDATES RECORD WITHOUT A PICK is re-derived too (4 Oct): the measured rules decide inside the re-derive, from the traces
        bool undecidedCandidates = false;
        if (const auto* cs = rec0.getProperty ("thresholdCandidates", {}).getArray(); cs != nullptr && ! rec0.getProperty ("pickedCandidate", {}).isObject())
            for (const auto& c : *cs) if (c.getProperty ("thresholdSweep", {}).getProperty ("result", "").toString() == "certified") undecidedCandidates = true;
        if (! loop::outcomeForRecord (rec0).exportPending && ! rec0.getProperty ("ruleDecided", {}).isObject() && ! undecidedCandidates) continue;   // not a certified profile: nothing to tone-check
        const auto stem0 = juce::File::createLegalFileName (product).replaceCharacter (' ', '_') + "_" + rec0.getProperty ("version", "").toString();
        const auto profileFile = opt.out.getChildFile ("profiles").getChildFile (stem0 + ".json");
        const auto tcFile = profileFile.getSiblingFile (profileFile.getFileNameWithoutExtension() + ".tonecheck.json");
        const auto tc = juce::JSON::parse (tcFile.loadFileAsString());
        // RESUME: a PASSED v1.7 check with its deep levels and L_ref is done (a needs_licence product has no result file, so
        // --retry-licence reaches exactly that set). A FAILED check is run again every time (6 Oct): the seven that failed on
        // Sean's Mac failed on writes the re-derive had lost, and the fix above changes the writes, not the check's file.
        if (! opt.deriveOnly && tc.getProperty ("spec", "").toString() == "v1.7" && tc.hasProperty ("deep_levels") && tc.hasProperty ("L_ref_dbfs") && (bool) tc.getProperty ("pass_within_0_5_db", true)
            && ! loop::sidechainAbOwed (rec0)) { ++skipped; continue; }
        if (! opt.deriveOnly && ! opt.retryLicence) if (const auto stop = loop::carriedLicenceStop (scanStops, product); stop) { std::cout << "  " << product << ": needs licence at the scan, not loaded" << std::endl; ++licence; continue; }
        std::cout << "\n=== tone checks: " << product << std::endl;
        // 1. RE-DERIVE from the traces (the deep points), carrying over what the traces do not hold
        const auto old = rec0;
        const auto stem = recordFile.getFileNameWithoutExtension();
        const auto processesJson = opt.out.getChildFile (stem + ".sweep.processes.json"), rawDir = opt.out.getChildFile ("raw");
        if (processesJson.existsAsFile() && rawDir.isDirectory())
        {
            const auto tmp = opt.out.getChildFile (stem + ".rederived.json");
            if (runSweepRederive (recordFile, processesJson, rawDir, tmp) == 0 && tmp.existsAsFile())
            {
                const auto fresh = loop::carryOverAfterRederive (old, juce::JSON::parse (tmp.loadFileAsString()));
                recordFile.replaceWithText (juce::JSON::toString (fresh) + "\n", false, false, "\n");
                tmp.deleteFile();
            }
            else std::cout << "  re-derivation did not complete; the existing record is used" << std::endl;
        }
        else { ++noTraces; std::cout << "  no traces for this record (" << processesJson.getFileName() << "): the existing points are used, no deep points can be derived" << std::endl; }
        if (undecidedCandidates)
        {
            const auto now = juce::JSON::parse (recordFile.loadFileAsString());
            if (! now.getProperty ("pickedCandidate", {}).isObject()) { std::cout << "  -> " << loop::outcomeForRecord (now).state << ": " << loop::outcomeForRecord (now).reason << std::endl; continue; }   // no rule decided: its row is re-filed below
        }
        // THE PICK THE RE-DERIVE JUST DECIDED may itself need a refinement round (UnFairchild: a linked pair whose leader reaches
        // 1 dB at 5 positions): the plan check runs again on the fresh record, and a re-sweep replaces the tone check
        {
            const auto now = juce::JSON::parse (recordFile.loadFileAsString());
            if (const auto d = loop::planDiffers (now, sweep::planFromFixture (now)); d.resweep)
            { resweep.add (product); resweepWhys.add (product + ": " + d.why); if (opt.projection) opt.projection->resweeps.push_back ({ product, d.why, true }); std::cout << "  -> re-sweep (after the re-derive): " << d.why << std::endl; continue; }
        }
        // 2. RE-EXPORT and 3. the tone checks, through the batch's own finish step (detector kept, pick by Rule 1 where decided)
        const auto newRow = finishRecord (opt, recordFile, rec0.getProperty ("category", "compressor").toString());
        outcomes = loop::mergeRow (outcomes, newRow); writeOutcomes();
        const auto st = newRow.getProperty ("state", "").toString();
        std::cout << "  -> " << st << ": " << newRow.getProperty ("reason", "").toString() << std::endl;
        // THE SIDECHAIN A/B VERDICT (ruled 6 Oct), taken inside the tone check at its pick: policy-sensitive -> re-swept below, and
        // the row just written is replaced by the re-sweep's own
        { const auto now = juce::JSON::parse (recordFile.loadFileAsString()); const auto ab = now.getProperty ("sidechainPolicyCheck", {});
          if (ab.isObject() && ab.getProperty ("policyNow", "").toString() == sidechaincheck::kPolicyNow && ab.getProperty ("verdict", "").toString() == "resweep" && ! resweep.contains (product))
          { resweep.add (product); resweepWhys.add (product + ": " + ab.getProperty ("why", "").toString()); if (opt.projection) opt.projection->resweeps.push_back ({ product, ab.getProperty ("why", "").toString(), true }); std::cout << "  -> re-sweep (the sidechain A/B at the pick): " << ab.getProperty ("why", "").toString() << std::endl; } }
        if (st == "exported" || newRow.getProperty ("reason", "").toString().startsWith ("export written (derive-only")) ++done; else if (st == "needs_licence") ++licence; else ++failed;
    }
    {
        std::cout << "\nRE-SWEEP: " << resweep.size() << " product(s)" << (opt.deriveOnly ? " (derive-only: projected, not run)" : "") << std::endl;
        for (const auto& w : resweepWhys) std::cout << "  " << w << std::endl;
        if (! resweep.isEmpty() && ! opt.deriveOnly)
        {
            SweepOptions so = opt; so.resweepProducts = resweep; so.profile = true; so.retryRefused = true; so.retryAll = true;
            // THE RE-SWEEP INHERITS THE BATCH'S PACE SETTING (Kathy's ruling 3, 6 Oct - Mike-E): with the iLok present a PACE-wrapped
            // product is measurable, as it was in the batch; the re-sweep used to hold it ("run with --include-pace") with the iLok in
            so.includePace = opt.includePace || ilokNow.startsWith ("present");
            if (so.includePace && ! opt.includePace) std::cout << "  (iLok " << ilokNow << ": the re-sweeps include PACE-wrapped products, as the batch did)" << std::endl;
            int k = 0;
            for (const auto& product : resweep)
            {
                std::cout << "\n=== re-sweep [" << ++k << "/" << resweep.size() << "] " << product << std::endl;
                so.product = product;
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                const bool tunerRecord = juce::JSON::parse (latestRecordFor (fixturesDir, product).loadFileAsString()).getProperty ("schema", "").toString() == kSchemaTuner;
                const int rc = tunerRecord ? runCertTuner (so) : runCertSweep (so);
                std::cout << "  wall " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 0) << " s, exit " << rc << std::endl;
                const auto rec = latestRecordFor (fixturesDir, product);
                if (rec.existsAsFile()) { const auto row = finishRecord (opt, rec, tunerRecord ? "pitch" : "compressor"); outcomes = loop::mergeRow (outcomes, row); writeOutcomes(); std::cout << "  -> " << row.getProperty ("state", "").toString() << ": " << row.getProperty ("reason", "").toString() << std::endl; }
            }
        }
    }
    // EVERY OTHER RECORD gets its row re-filed under the current rules too (licence, multiband, surround, candidate rules need a pick):
    // the projection for a zipped-back folder is the whole outcomes.json, not just the exports
    for (const auto& recordFile : fixturesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (recordFile.getFileName().endsWith (".defaults.json")) continue;
        const auto r = juce::JSON::parse (recordFile.loadFileAsString()); const auto product = r.getProperty ("product", "").toString();
        if (! r.hasProperty ("product") || (! opt.slice.isEmpty() && ! opt.slice.contains (product))) continue;
        const auto o = loop::outcomeForRecord (r);
        if (o.exportPending || r.getProperty ("ruleDecided", {}).isObject()) continue;   // handled above
        const auto identity = r.getProperty ("identity", "").toString();
        bool had = false; if (const auto* a = outcomes.getArray()) for (const auto& x : *a) if (x.getProperty ("identity", "").toString() == identity || x.getProperty ("product", "").toString() == product) { had = true; break; }
        outcomes = loop::mergeRow (outcomes, loop::makeRow (identity, product, r.getProperty ("category", "compressor").toString(), o, recordFile.getFullPathName(), {}, {}, nowStamp()));
        (void) had;
    }
    writeOutcomes();
    { const auto c = loop::count (outcomes); std::cout << "OUTCOMES (" << outcomesFile.getFullPathName() << "): " << c.rows << " rows - exported " << c.exported << ", recorded " << c.recorded << ", refused " << c.refused << ", held " << c.held << ", needs_review " << c.needsReview << ", quarantined_at_scan " << c.quarantined << ", needs_licence " << c.needsLicence << ", multiband " << c.multiband << ", surround " << c.surround << std::endl; }
    std::cout << "\nTONECHECK-ALL: " << done << " re-checked, " << skipped << " already at v1.7 (skipped), " << licence << " needs_licence, " << failed << " not exported now, " << noTraces << " without traces" << std::endl;
    { std::ofstream sheet (opt.out.getChildFile ("review_sheet.txt").getFullPathName().toStdString()); printReviewSheet (fixturesDir, sheet); }
    std::cout << "review sheet: " << opt.out.getChildFile ("review_sheet.txt").getFullPathName() << std::endl;
    return 0;
}

inline int runSweepAll (SweepOptions opt, const juce::StringArray& skip)
{
    const auto fixturesDir = opt.out.getChildFile ("fixtures"); fixturesDir.createDirectory();
    const auto outcomesFile = opt.out.getChildFile ("outcomes.json");
    auto outcomes = juce::JSON::parse (outcomesFile.loadFileAsString()); if (! outcomes.isArray()) outcomes = juce::Array<juce::var>();
    auto writeOutcomes = [&] { outcomesFile.replaceWithText (juce::JSON::toString (outcomes) + "\n", false, false, "\n"); };
    auto record = [&] (const juce::var& row) {
        const auto v = loop::rowViolation (row);
        if (v.isNotEmpty()) std::cout << "  OUTCOME INVARIANT BROKEN (" << v << "): " << juce::JSON::toString (row, true) << std::endl;
        outcomes = loop::mergeRow (outcomes, row); writeOutcomes();
        std::cout << "  -> " << row.getProperty ("state", "").toString() << ": " << row.getProperty ("reason", "").toString() << std::endl; };
    // THE OPENING: the iLok's presence and the census, on file (criteria A2, B).
    const auto ilok = iLokPresence();
    {
        std::ostringstream cap; auto* old = std::cout.rdbuf (cap.rdbuf());
        runSweepCensus (opt.fixtures, opt.ledger, opt.includePace, opt.retryRefused, opt.retryAll);
        std::cout.rdbuf (old);
        opt.out.getChildFile ("census.txt").replaceWithText ("batch " + nowStamp() + "  iLok " + ilok + "  profile " + (opt.profile ? "yes" : "no")
                                                             + (opt.slice.isEmpty() ? juce::String() : "  slice " + opt.slice.joinIntoString (", ")) + "\n" + cap.str() + "\n", false, false, "\n");
        std::cout << cap.str();
    }
    std::cout << "iLok: " << ilok << std::endl;
    juce::StringArray wl; std::vector<UnmappedProduct> unmapped;
    auto subjects = buildWorklist (opt.fixtures, opt.ledger, opt.includePace, wl, opt.retryRefused || opt.retryLicence, opt.retryAll, &unmapped, opt.retryLicence);
    std::cout << wl.joinIntoString ("\n") << std::endl;
    // THE SCAN'S LICENCE EVIDENCE, CARRIED FORWARD (ruled 2 Oct): a product whose bundle raised an activation window at
    // the scan is needs_licence here too and is NOT loaded again (a second window for a bundle we already know about is
    // the gratuitous dialog). --retry-licence re-checks just that set.
    const auto scanStops = quarantinedBundles (opt.ledger);
    juce::StringArray done, refused;
    int n = 0;
    const SleepGuard sleepGuard ("EJ Map certification batch");
    std::cout << sleepGuard.describe() << std::endl;
    for (const auto& s : subjects)
    {
        if (! opt.slice.isEmpty() && ! opt.slice.contains (s.product)) continue;
        const bool planLater = s.reach == Subject::Reach::unfixtured;
        const bool tuner = s.category == "pitch";
        const auto identity = "AudioUnit|" + s.uid + "|" + (s.desc.version.isNotEmpty() ? s.desc.version : s.version);
        if (skip.contains (s.product)) continue;
        auto withMap = [&] (juce::var row) { if (s.mapState.isNotEmpty()) if (auto* o = row.getDynamicObject()) o->setProperty ("map", s.mapState); return row; };
        if (! measurable (s, opt.includePace))
        {
            record (withMap (loop::makeRow (identity, s.product, s.category, loop::outcomeHeld (false, s.hardware, s.detail), {}, {}, {}, nowStamp())));
            continue;
        }
        if (! opt.retryLicence)
            if (const auto stop = loop::carriedLicenceStop (scanStops, s.product); stop)
            {
                auto row = withMap (loop::makeRow (identity, s.product, s.category, loop::outcomeCarriedLicence (*stop), {}, {}, {}, nowStamp()));
                if (auto* o = row.getDynamicObject()) o->setProperty ("scan_bundle", stop->bundle);     // the link to the scan's evidence
                record (row);
                continue;
            }
        if (! tuner && ! planLater && ! sweep::planFromFixture (s.pushed).ok)
        {
            loop::Outcome o; o.state = "needs_review"; o.reason = "no plan from the record's controls: " + sweep::planFromFixture (s.pushed).why;
            record (loop::makeRow (identity, s.product, s.category, o, s.fixtureFile.getFullPathName(), {}, {}, nowStamp()));
            continue;
        }
        ++n;
        std::cout << "\n=== [" << n << "] " << s.product << (tuner ? " (tuner)" : "") << std::endl;
        opt.product = s.product; opt.mapState = s.mapState;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const int rc = tuner ? runCertTuner (opt) : runCertSweep (opt);
        std::cout << "  wall " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 0) << " s, exit " << rc << std::endl;
        (rc == 0 ? done : refused).add (s.product + (rc == 0 ? juce::String() : " (exit " + juce::String (rc) + ")"));
        const auto rec = latestRecordFor (fixturesDir, s.product);
        if (rec.existsAsFile()) record (withMap (finishRecord (opt, rec, s.category)));
        else { loop::Outcome o; o.state = "refused"; o.reason = "stage unknown: the run wrote no record (exit " + juce::String (rc) + ")"; record (loop::makeRow (identity, s.product, s.category, o, {}, {}, {}, nowStamp())); }
    }
    // ONE ROW PER PRODUCT (ruled 2 Oct, evening): a bundle the scan stopped or quarantined gives a row only for the
    // products that are NOT subjects of the batch (a subject's own row carries the carried-forward state and names the
    // bundle); one row per product, never per bundle, so every count is per product (loop::bundleRows).
    { juce::StringArray subjectNames; for (const auto& s : subjects) subjectNames.add (s.product);
      for (const auto& row : loop::bundleRows (scanStops, subjectNames, nowStamp())) record (row); }
    // THE FINISH PASS: every record in the store without a row (resumed batch, or records from before the loop).
    int finished = 0;
    for (const auto& f : fixturesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (f.getFileName().endsWith (".defaults.json")) continue;
        const auto r = juce::JSON::parse (f.loadFileAsString());
        const auto identity = r.getProperty ("identity", f.getFileNameWithoutExtension()).toString();
        const auto existing = loop::findRow (outcomes, identity);
        if (existing.isObject() && existing.getProperty ("state", "").toString() != "held") continue;
        if (! opt.slice.isEmpty() && ! opt.slice.contains (r.getProperty ("product", "").toString())) continue;
        ++finished;
        std::cout << "\n=== finish " << r.getProperty ("product", "").toString() << std::endl;
        record (finishRecord (opt, f, r.getProperty ("category", "compressor").toString()));
    }
    const auto c = loop::count (outcomes);
    std::cout << "\nSWEEP-ALL: " << n << " attempted, " << done.size() << " swept to a fixture, " << refused.size() << " stopped, " << finished << " finished from the store\n";
    for (const auto& r : refused) std::cout << "  stopped: " << r << "\n";
    std::cout << "OUTCOMES (" << outcomesFile.getFullPathName() << "): " << c.rows << " rows - exported " << c.exported << ", recorded " << c.recorded
              << ", refused " << c.refused << ", held " << c.held << ", needs_review " << c.needsReview << ", quarantined_at_scan " << c.quarantined << ", needs_licence " << c.needsLicence << ", multiband " << c.multiband << ", surround " << c.surround << std::endl;
    int broken = 0; if (const auto* a = outcomes.getArray()) for (const auto& r : *a) if (loop::rowViolation (r).isNotEmpty()) ++broken;
    if (broken > 0) std::cout << "OUTCOME INVARIANT BROKEN on " << broken << " row(s)" << std::endl;
    std::cout << std::flush;
    return broken > 0 ? 2 : 0;
}

//==============================================================================
// TUNER CERTIFICATION (--cert-tuner, spec section 5, built 1 Oct). One product: resolve it in the AU registry, sample
// its defaults (the unseen-version path's probes), role its controls with the TUNER lexicon, then for every control
// holding the strength role run the probe's pitch sweep twice - static (strength) and square vibrato (speed) - and
// write <out>/tuners/<identity>.json with `pitchCandidates` (one record per candidate) and `pitchReview`. The probe
// hosts all positions of one generator in ONE process (no compression history to reset between positions). An
// ARA/offline-only tool is refused before any process by name.
inline bool araOnlyByName (const juce::String& product)
{
    return product.containsIgnoreCase ("melodyne") || product.containsIgnoreCase ("waves tune lt") || product.containsIgnoreCase ("waves tune (");
}

inline int runCertTuner (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const SleepGuard sleepGuard ("EJ Map tuner certification");
    auto outDir = opt.out.getChildFile ("fixtures");                 // ONE STORE (ruled 1 Oct): tuner records beside the compressors'
    if (! outDir.createDirectory()) { say ("TUNER: cannot create " + outDir.getFullPathName()); return 2; }
    const auto id = checkProbe (opt.probe, {}, {});
    if (! id.ok) { say ("TUNER: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const juce::String probeLabel = "signed EchoJayProbe (feat/ejmap-cert), team " + id.team + ", cdhash " + id.cdhash;
    // Resolve the installed component by product name.
    std::vector<InstalledRecord> hits;
    for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.empty()) { say ("TUNER: '" + opt.product + "' is not an installed AudioUnit"); return 2; }
    if (hits.size() > 1) { say ("TUNER: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " components; refused"); return 2; }
    Subject s; s.desc = hits[0].desc; s.product = s.desc.name; s.uid = juce::String::toHexString (s.desc.uniqueId).toLowerCase(); s.version = s.desc.version;
    s.reach = Subject::Reach::unfixtured; s.installedUnique = true; s.category = "pitch"; s.mapState = opt.mapState;
    // A refusal is a record here too: the identity with no controls, so the worklist stops offering it.
    auto identityOnly = [&] { auto* o = new juce::DynamicObject(); o->setProperty ("product", s.product); o->setProperty ("uid", s.uid);
                              o->setProperty ("version", s.version); o->setProperty ("format", "AudioUnit"); o->setProperty ("category", "pitch");
                              o->setProperty ("schema", kSchemaTuner); if (s.mapState.isNotEmpty()) o->setProperty ("mapState", s.mapState); return juce::var (o); };
    if (araOnlyByName (opt.product))
    {
        say ("TUNER: " + opt.product + " is an ARA/offline tool with no real-time pitch path: uncertifiable by any harness, refused before any process");
        writeRefusalRecord (outDir, identityOnly(), "ara_only", "ARA/offline-only pitch tool: no real-time path, uncertifiable by any harness", 0, 0, probeLabel, "EJ Map " + opt.hostVersion);
        return 4;
    }
    const auto stem = "AudioUnit_" + s.uid + "_" + s.version + ".tuner";
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory();
    auto ledger = opt.out.getChildFile ("run.jsonl");
    juce::Array<juce::var> processes; bool windowSeen = false; juce::StringArray windows;
    auto runProbe = [&] (const juce::String& tag, const juce::StringArray& extra) -> ChildResult
    {
        juce::StringArray args { opt.probe.getFullPathName(), s.desc.name, s.desc.fileOrIdentifier, juce::String::toHexString (s.desc.uniqueId) };
        args.addArray (extra);
        const auto a = runWithRetry (args, opt.timeoutMs, [&] (int attempt, const ChildResult& r)
        {
            const auto file = stem + "." + tag + "." + juce::String (attempt) + ".txt";
            raw.getChildFile (file).replaceWithText (r.out, false, false, "\n");
            auto* o = new juce::DynamicObject();
            o->setProperty ("tag", tag); o->setProperty ("attempt", attempt); o->setProperty ("file", file);
            o->setProperty ("outcome", r.describe()); o->setProperty ("clean", r.cleanExit()); o->setProperty ("ms", r.ms);
            processes.add (juce::var (o));
            ledger.appendText (juce::JSON::toString (juce::var (o), true) + "\n");
            if (r.kind == ChildResult::Kind::uiShown) { windowSeen = true; windows.addArray (r.windowsInTree); }
        });
        auto r = a.r; if (a.sleptTwice) r.kind = ChildResult::Kind::sleptTwice; return r;
    };
    const juce::String date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    auto refuseT = [&] (int rc, const juce::String& stage, const juce::String& reason) {
        say ("TUNER: " + s.product + " - " + reason);
        const auto rec = writeRefusalRecord (outDir, identityOnly(), stage, reason, processes.size(), 1, probeLabel, "EJ Map " + opt.hostVersion);
        say ("TUNER: refusal recorded at stage '" + stage + "' -> " + rec.getFileName());
        return rc; };
    auto lp = runProbe ("list-params", { "--list-params" });
    if (! lp.cleanExit()) return refuseT (1, "defaults", (windowSeen ? "UNLICENSED ON HOST: " : "ERROR: ") + juce::String ("defaults --list-params ") + lp.describe());
    auto ta = runProbe ("text-at", { "--text-at", "all" });
    if (! ta.cleanExit()) return refuseT (1, "defaults", (windowSeen ? "UNLICENSED ON HOST: " : "ERROR: ") + juce::String ("defaults --text-at all ") + ta.describe());
    auto base = composeFixture (s, parseListParams (lp.out), parseTextAt (ta.out), lp.code, ta.code, probeLabel, date);
    if (auto* o = base.getDynamicObject()) { o->setProperty ("category", "pitch"); o->setProperty ("schema", kSchemaTuner); }

    // ROLES with the tuner lexicon: strength candidates, the key/scale control.
    std::vector<roles::NamedControl> named;
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs) named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(), false });
    const auto cl = roles::classify (named, roles::Category::tuner);
    std::vector<const roles::ControlRole*> strength; int keyIndex = -1; juce::String keyText;
    for (const auto& r : cl.controls)
    {
        if (r.role == "strength") strength.push_back (&r);
        if (r.role == "key" && keyIndex < 0) { keyIndex = r.index; keyText = sweep::findControl (base, r.index).getProperty ("defaultOnInstantiate", {}).getProperty ("display", "").toString(); }
    }
    say ("TUNER: " + s.product + " " + s.version + ": " + juce::String ((int) strength.size()) + " strength candidate(s)"
         + (keyIndex >= 0 ? ", key/scale [" + juce::String (keyIndex) + "] as instantiated '" + keyText + "'" : juce::String (", no key/scale control roled")));
    juce::Array<juce::var> cands; int measuredAny = 0;
    juce::Array<juce::var> planCands;                 // THE PLAN THE RECORD WAS MEASURED UNDER (pitchPlan v2): planDiffers reads it
    if (strength.empty()) say ("TUNER: no control holds the strength role: nothing to sweep (recorded)");
    for (const auto* c : strength)
    {
        if (windowSeen) break;
        const auto ctrl = sweep::findControl (base, c->index);
        const juce::String ctl = juce::String (c->index);
        juce::StringArray norms, detentTexts; juce::String detentsBy;
        if (sweep::isSteppedControl (ctrl)) { detentsBy = "declared"; const int n = (int) ctrl.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) norms.add (juce::String ((float) k / (float) juce::jmax (1, n - 1), 6)); }
        else if (pitch::textsAreWords (ctrl))
        {
            // DETENTS BY EVIDENCE (4 Oct): a word-valued control is measured where its writes land, nowhere else
            juce::StringArray gs; for (int k = 0; k <= 32; ++k) gs.add (juce::String ((float) k / 32.0f, 6));
            const auto tg = runProbe ("c" + ctl + ".textgrid", { "--text-at-norms", ctl, gs.joinIntoString (",") });
            const auto detents = pitch::detentsFromTextGrid (pitch::parseTextGrid (tg.cleanExit() ? tg.out : juce::String()));
            if (detents.empty()) { detentsBy = "text_grid_unreadable"; for (int k = 0; k < 8; ++k) norms.add (juce::String ((float) k / 7.0f, 6)); say ("  [" + ctl + "] " + c->name + ": word-valued but its text grid gave no detents (" + tg.describe() + "); eight positions"); }
            else { detentsBy = "text"; for (const auto& [n, t] : detents) { norms.add (juce::String (n, 6)); detentTexts.add (t); } say ("  [" + ctl + "] " + c->name + ": " + juce::String ((int) detents.size()) + " detents by text (" + detentTexts.joinIntoString (" / ") + ")"); }
        }
        else { detentsBy = "even8"; for (int k = 0; k < 8; ++k) norms.add (juce::String ((float) k / 7.0f, 6)); }
        auto st = runProbe ("c" + ctl + ".static",  { "--sweep-pitch", "ctl=" + ctl, "norms=" + norms.joinIntoString (","), "gen=static",  "note=220", "cents=30", "hold=4", "db=-18" });
        const auto ms = pitch::parsePitch (st.cleanExit() ? st.out : juce::String ("refused " + st.describe()));
        // THE ADAPTIVE HALF PERIOD (4 Oct): 1 s for every position; the still-unsettled positions again at 2 s, then 4 s.
        // Every run keeps the FULL norms list so position indices line up; the later runs render only the unsettled norms
        // and the others are absent from them (pickSpeed skips a run that lacks the position).
        std::vector<pitch::SpeedRun> runs; juce::StringArray halfPeriodsTried;
        for (double hp : pitch::kSpeedHalfPeriodsS)
        {
            if (windowSeen) break;
            juce::StringArray runNorms = norms;
            if (! runs.empty())
            {
                const auto still = pitch::unsettledPositions (runs, (size_t) norms.size());
                if (still.empty()) break;
                runNorms.clear(); for (size_t i : still) runNorms.add (norms[(int) i]);
            }
            const double rate = 0.5 / hp, hold = 6.0 * hp;
            const auto tag = "c" + ctl + ".vibrato" + (hp == 1.0 ? juce::String() : "-hp" + juce::String (hp, 0));
            auto vb = runProbe (tag, { "--sweep-pitch", "ctl=" + ctl, "norms=" + runNorms.joinIntoString (","), "gen=vibrato", "shape=square", "rate=" + juce::String (rate, 4), "note=220", "cents=30", "hold=" + juce::String (hold, 0), "db=-18" });
            auto mv = pitch::parsePitch (vb.cleanExit() ? vb.out : juce::String ("refused " + vb.describe()));
            halfPeriodsTried.add (juce::String (hp, 0) + " s");
            if (! runs.empty() && mv.ok)
            {
                // re-seat the partial run's positions at their indices in the full list
                std::vector<pitch::PitchPosition> full ((size_t) norms.size());
                for (const auto& pp : mv.positions) for (int i = 0; i < norms.size(); ++i) if (std::abs (norms[i].getFloatValue() - pp.norm) < 1e-5f) full[(size_t) i] = pp;
                for (size_t i = 0; i < full.size(); ++i) if (full[i].k < 0) full[i].landed = false;    // absent from this run
                // a position absent from this run must not read as "write did not land": mark it so pickSpeed skips it
                mv.positions = full;
                for (size_t i = 0; i < mv.positions.size(); ++i) if (mv.positions[i].k < 0) mv.positions[i].windows.clear();
            }
            runs.push_back ({ hp, mv });
            if (! mv.ok) break;
        }
        auto rec = pitch::composePitchSweep (ms, runs, keyIndex, keyText);
        if (auto* o = rec.getDynamicObject())
        {
            o->setProperty ("index", c->index); o->setProperty ("name", c->name);
            if (! ms.ok) o->setProperty ("staticRefused", ms.refused);
            if (! runs.empty() && ! runs.front().vib.ok) o->setProperty ("vibratoRefused", runs.front().vib.refused);
            o->setProperty ("detentsBy", detentsBy); if (! detentTexts.isEmpty()) { juce::Array<juce::var> dt; for (const auto& t : detentTexts) dt.add (t); o->setProperty ("detentTexts", dt); }
            o->setProperty ("speedHalfPeriodsTried", halfPeriodsTried.joinIntoString (", "));
        }
        { auto* pc = new juce::DynamicObject(); pc->setProperty ("index", c->index); pc->setProperty ("name", c->name); pc->setProperty ("detentsBy", detentsBy);
          juce::Array<juce::var> nv; for (const auto& n : norms) nv.add (n.getDoubleValue()); pc->setProperty ("norms", nv); planCands.add (juce::var (pc)); }
        measuredAny += (int) rec.getProperty ("strengthMeasured", 0) + (int) rec.getProperty ("speedMeasured", 0);
        say ("  [" + ctl + "] " + c->name + ": strength measured at " + rec.getProperty ("strengthMeasured", 0).toString() + " position(s), speed at " + rec.getProperty ("speedMeasured", 0).toString());
        cands.add (rec);
    }
    // THE v0.1 MEASUREMENTS (docs/TUNER_PROFILE_SPEC_v0_1.md section 4, A5, a PROPOSAL): flex tolerance, humanize, key/scale read-backs
    auto* extras = new juce::DynamicObject();
    {
        // the same position rule as the strength candidates: declared detents, text-grid detents for a word-valued control, else eight
        auto normsFor = [&] (const juce::var& ctrl) {
            juce::StringArray norms;
            const juce::String ctl = juce::String ((int) ctrl.getProperty ("index", -1));
            if (sweep::isSteppedControl (ctrl)) { const int n = (int) ctrl.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) norms.add (juce::String ((float) k / (float) juce::jmax (1, n - 1), 6)); }
            else if (pitch::textsAreWords (ctrl))
            {
                juce::StringArray gs; for (int k = 0; k <= 32; ++k) gs.add (juce::String ((float) k / 32.0f, 6));
                const auto tg = runProbe ("x" + ctl + ".textgrid", { "--text-at-norms", ctl, gs.joinIntoString (",") });
                for (const auto& [n, t] : pitch::detentsFromTextGrid (pitch::parseTextGrid (tg.cleanExit() ? tg.out : juce::String()))) norms.add (juce::String (n, 6));
                if (norms.isEmpty()) for (int k = 0; k < 8; ++k) norms.add (juce::String ((float) k / 7.0f, 6));
            }
            else for (int k = 0; k < 8; ++k) norms.add (juce::String ((float) k / 7.0f, 6));
            return norms; };
        juce::Array<juce::var> flex, humanize; auto* readbacks = new juce::DynamicObject();
        if (const auto* cs = base.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs)
            {
                if (windowSeen) break;
                const int idx = (int) c.getProperty ("index", -1); const auto name = c.getProperty ("name", "").toString(); const juce::String ctl = juce::String (idx);
                bool isStrength = false; for (const auto* sc : strength) if (sc->index == idx) isStrength = true;
                if (pitch::flexName (name) && ! isStrength)
                {
                    const auto norms = normsFor (c);
                    std::map<double, pitch::PitchMeasured> runs;
                    for (double d : pitch::kFlexDetunesCents)
                    {
                        if (windowSeen) break;
                        auto r = runProbe ("f" + ctl + ".static-" + juce::String (d, 0), { "--sweep-pitch", "ctl=" + ctl, "norms=" + norms.joinIntoString (","), "gen=static", "note=220", "cents=" + juce::String (d, 0), "hold=4", "db=-18" });
                        runs[d] = pitch::parsePitch (r.cleanExit() ? r.out : juce::String ("refused " + r.describe()));
                    }
                    auto* fo = new juce::DynamicObject(); fo->setProperty ("index", idx); fo->setProperty ("name", name);
                    juce::Array<juce::var> positions;
                    for (int k = 0; k < norms.size(); ++k)
                    {
                        auto* po = new juce::DynamicObject(); po->setProperty ("norm", norms[k].getDoubleValue());
                        std::map<double, double> strengthBy; auto* by = new juce::DynamicObject(); juce::String display;
                        for (const auto& [d, m] : runs)
                            if (m.ok && (size_t) k < m.positions.size())
                            {
                                if (display.isEmpty()) display = m.positions[(size_t) k].text;
                                const auto sr = pitch::deriveStrength (m.positions[(size_t) k], d);
                                if (sr.result == "measured") { strengthBy[d] = sr.strength; by->setProperty (juce::String (d, 0), sr.strength); } else by->setProperty (juce::String (d, 0), "refused: " + sr.reason);
                            }
                        po->setProperty ("display", display); po->setProperty ("strength_by_detune", juce::var (by));
                        const auto tol = pitch::toleranceFrom (strengthBy);
                        if (tol.ok) po->setProperty ("window_cents", tol.none ? juce::var ("none") : tol.over ? juce::var ("over_" + juce::String (tol.cents, 0)) : juce::var (tol.cents));
                        po->setProperty ("window_note", tol.reason);
                        positions.add (juce::var (po));
                    }
                    fo->setProperty ("positions", positions); flex.add (juce::var (fo));
                    say ("  flex [" + ctl + "] " + name + ": window measured at " + juce::String (norms.size()) + " position(s) x " + juce::String ((int) pitch::kFlexDetunesCents.size()) + " detunes");
                }
                else if (pitch::humanizeName (name) && ! isStrength)
                {
                    const auto norms = normsFor (c);
                    auto held = runProbe ("h" + ctl + ".held",  { "--sweep-pitch", "ctl=" + ctl, "norms=" + norms.joinIntoString (","), "gen=static", "note=220", "cents=30", "hold=4", "db=-18" });
                    auto shrt = runProbe ("h" + ctl + ".notes", { "--sweep-pitch", "ctl=" + ctl, "norms=" + norms.joinIntoString (","), "gen=notes", "note=220", "cents=30", "hold=4", "note_ms=200", "gap_ms=100", "db=-18" });
                    const auto mh = pitch::parsePitch (held.cleanExit() ? held.out : juce::String ("refused " + held.describe()));
                    const auto mn = pitch::parsePitch (shrt.cleanExit() ? shrt.out : juce::String ("refused " + shrt.describe()));
                    auto* ho = new juce::DynamicObject(); ho->setProperty ("index", idx); ho->setProperty ("name", name);
                    juce::Array<juce::var> positions;
                    for (int k = 0; k < norms.size(); ++k)
                    {
                        auto* po = new juce::DynamicObject(); po->setProperty ("norm", norms[k].getDoubleValue());
                        if (mh.ok && (size_t) k < mh.positions.size()) { po->setProperty ("display", mh.positions[(size_t) k].text); const auto sr = pitch::deriveStrength (mh.positions[(size_t) k], 30.0); po->setProperty ("held_note_correction", sr.result == "measured" ? juce::var (sr.strength) : juce::var ("refused: " + sr.reason)); }
                        if (mn.ok && (size_t) k < mn.positions.size()) { const auto sn = pitch::deriveShortNotes (mn.positions[(size_t) k], 30.0); po->setProperty ("short_note_correction", sn.result == "measured" ? juce::var (sn.strength) : juce::var ("refused: " + sn.reason)); po->setProperty ("short_notes", sn.windowsUsed); }
                        if (po->getProperty ("held_note_correction").isDouble() && po->getProperty ("short_note_correction").isDouble() && (double) po->getProperty ("short_note_correction") > 0.05)
                            po->setProperty ("held_over_short", std::round ((double) po->getProperty ("held_note_correction") / (double) po->getProperty ("short_note_correction") * 1000.0) / 1000.0);
                        positions.add (juce::var (po));
                    }
                    ho->setProperty ("positions", positions); humanize.add (juce::var (ho));
                    say ("  humanize [" + ctl + "] " + name + ": held vs short notes at " + juce::String (norms.size()) + " position(s)");
                }
                else if ((pitch::keyName (name) || pitch::scaleName (name)) && ! nametokens::controlAnswersTerm (name, "learn") && ! nametokens::controlAnswersTerm (name, "midi"))   // not 'Learn Scale from MIDI'
                {
                    juce::StringArray gs;
                    if (sweep::isSteppedControl (c)) { const int n = (int) c.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) gs.add (juce::String ((float) k / (float) juce::jmax (1, n - 1), 6)); }
                    else for (int k = 0; k <= 32; ++k) gs.add (juce::String ((float) k / 32.0f, 6));
                    const auto tg = runProbe ("k" + ctl + ".textgrid", { "--text-at-norms", ctl, gs.joinIntoString (",") });
                    auto* ro = new juce::DynamicObject(); ro->setProperty ("index", idx); ro->setProperty ("name", name); ro->setProperty ("kind", pitch::scaleName (name) ? "scale" : "key");
                    auto* values = new juce::DynamicObject(); int n = 0;
                    for (const auto& [norm, text] : pitch::detentsFromTextGrid (pitch::parseTextGrid (tg.cleanExit() ? tg.out : juce::String()))) { values->setProperty (text, norm); ++n; }
                    ro->setProperty ("values", juce::var (values)); ro->setProperty ("count", n);
                    if (n == 0) ro->setProperty ("note", "no landed text read back (" + tg.describe() + ")");
                    readbacks->setProperty (juce::String (idx), juce::var (ro));
                    say ("  readback [" + ctl + "] " + name + ": " + juce::String (n) + " value(s)");
                }
            }
        extras->setProperty ("flex", flex); extras->setProperty ("humanize", humanize); extras->setProperty ("readbacks", juce::var (readbacks));
        extras->setProperty ("spec", "TUNER_PROFILE_SPEC_v0_1 section 4 - PROPOSAL, not for publication");
    }
    opt.out.getChildFile (stem + ".processes.json").replaceWithText (juce::JSON::toString (juce::var (processes)) + "\n", false, false, "\n");
    auto f = sweep::stripPrivate (base);
    if (auto* o = f.getDynamicObject())
    {
        o->setProperty ("pitchCandidates", cands);
        o->setProperty ("pitchExtras", juce::var (extras));
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("version", pitch::kPitchPlanVersion); pp->setProperty ("candidates", planCands);
          juce::Array<juce::var> hp; for (double h : pitch::kSpeedHalfPeriodsS) hp.add (h); pp->setProperty ("speedHalfPeriodsS", hp); o->setProperty ("pitchPlan", juce::var (pp)); }
        auto* rv = new juce::DynamicObject();
        rv->setProperty ("candidates", cands.size()); rv->setProperty ("measured", measuredAny);
        rv->setProperty ("windowSeen", windowSeen);
        rv->setProperty ("note", "every control holding the strength role was swept with a static detuned note and a square vibrato; a control whose static residual moves across positions is a strength control, one whose transition duration moves is a speed control; the server half reads both");
        o->setProperty ("pitchReview", juce::var (rv));
    }
    auto outFile = outDir.getChildFile ("AudioUnit_" + s.uid + "_" + s.version + ".json");
    outFile.replaceWithText (juce::JSON::toString (f) + "\n", false, false, "\n");
    say ("TUNER: " + s.product + " -> " + outFile.getFileName() + (windowSeen ? "  (A WINDOW APPEARED: " + windows.joinIntoString (", ") + ")" : juce::String()));
    return windowSeen ? 5 : 0;
}

//==============================================================================
// EXPORT (--export-profile <record> <out.json> | --export-profiles <store> <outDir>): every record through the one
// exporter; a refusal is printed with its reason and nothing is written for it. The report is the per-product line Sean
// asked for: profile emitted / emitted with fit over 1.5 / not possible and why.
inline int runExportProfiles (const juce::File& in, const juce::File& out, bool whole, const juce::String& candidate = {})
{
    juce::Array<juce::File> files;
    if (whole) files = in.findChildFiles (juce::File::findFiles, false, "*.json"); else files.add (in);
    if (whole) out.createDirectory();
    int emitted = 0, over = 0, refused = 0;
    for (const auto& f : files)
    {
        if (f.getFileName().endsWith (".defaults.json")) continue;
        auto rec = juce::JSON::parse (f.loadFileAsString());
        if (candidate.isNotEmpty()) { juce::String why; auto v = profile::candidateAsSingle (rec, candidate, why); if (v.isVoid()) { std::cout << "NOT POSSIBLE   " << f.getFileName() << ": " << why << std::endl; ++refused; continue; } rec = v; }
        const auto e = profile::exportCompProfile (rec);
        const auto name = rec.getProperty ("product", f.getFileNameWithoutExtension()).toString();
        if (! e.ok) { ++refused; std::cout << "NOT POSSIBLE   " << name << ": " << e.refused << std::endl; continue; }
        const auto dst = whole ? out.getChildFile (f.getFileName()) : out;
        dst.replaceWithText (juce::JSON::toString (e.profile) + "\n", false, false, "\n");
        const bool big = e.fitMaxErrorDb > 1.5;
        if (big) ++over; else ++emitted;
        std::cout << (big ? "EMITTED, FIT OVER 1.5  " : "EMITTED        ") << name << ": " << e.points << " curve points, fit max error "
                  << juce::String (e.fitMaxErrorDb, 2) << " dB -> " << dst.getFileName() << std::endl;
    }
    std::cout << "EXPORT: " << emitted << " emitted, " << over << " emitted with fit over 1.5, " << refused << " not possible" << std::endl;
    return 0;
}

//==============================================================================
// HIS SECTION 8 TONE CHECK (v1.2), run here (--cert-tone-check): L = -18 dBFS RMS, g = 2. Pick the position exactly as
// his section 6 says from the EXPORTED profile, write it plus engage + neutral + the reference ratio, render 997 Hz at L
// through the signed probe in one fresh process (with -54/-48 for the position's own quiet reference), measure GR. Pass
// is within 0.5 dB of g. The ratio NORM is not in his profile (only the control name and the value), so it comes from
// our record's preconditions - said in the result, because it is a gap in the contract.
inline int runToneCheck (const SweepOptions& opt, const juce::File& profileFile, const juce::File& recordFile, double Lrms, double g, const juce::String& candidate)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {});
    if (! id.ok) { say ("TONE: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    auto profile = juce::JSON::parse (profileFile.loadFileAsString());
    auto record  = juce::JSON::parse (recordFile.loadFileAsString());
    if (! profile.isObject() || ! record.isObject()) { say ("TONE: cannot read the profile or the record"); return 2; }
    if (candidate.isNotEmpty()) { juce::String why; record = profile::candidateAsSingle (record, candidate, why); if (record.isVoid()) { say ("TONE: " + why); return 2; } }
    const auto product = profile.getProperty ("plugin", {}).getProperty ("name", "").toString();
    std::vector<InstalledRecord> hits;
    for (const auto& r : installedAudioUnits()) if (r.desc.name == product) hits.push_back (r);
    if (hits.size() != 1) { say ("TONE: '" + product + "' resolves to " + juce::String ((int) hits.size()) + " component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    // SECTION 11: the installed version must be the record's
    if (const auto vm = profile::versionMismatch (record.getProperty ("version", "").toString(), desc.version); vm.isNotEmpty()) { say ("TONE: " + product + " - " + vm); return kToneVersionExit; }
    // DO NOT LOAD A PRODUCT THE SESSION KNOWS NEEDS A LICENCE THAT IS NOT PRESENT (ruled 3 Oct): the scan's licence stops
    // in this ledger, or a needs_licence row in this cert folder; --retry-licence is the only way past.
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), product, opt.retryLicence); known.isNotEmpty())
    { say ("TONE: " + product + " - " + known); return kToneLicenceKnownExit; }
    // THE WRITES (2 Oct): engage + neutral + ratio, every one from the exported profile, resolved to indices through the
    // record's controls (profile::toneWrites, pinned); then the section 6 pick. The check rehearses the server's writes.
    juce::StringArray sets; juce::Array<juce::var> writes; juce::String ratioNote;
    if (const auto why = profile::toneWrites (profile, record, sets, writes, ratioNote); why.isNotEmpty()) { say ("TONE: " + why); return 2; }
    auto plan = sweep::planFromFixture (record);
    if (! plan.ok) { say ("TONE: the record has no plan: " + plan.why); return 4; }
    // THE DUAL-MONO TWIN (ruled 6 Oct): written with the amount at the pick's norm in every process below; both channels within 0.5 dB
    const int pairIdx = (int) profile.getProperty ("amount", {}).getProperty ("pair_with", {}).getProperty ("index", -1);
    if (pairIdx >= 0) say ("TONE: " + product + " - dual-mono pair: [" + juce::String (pairIdx) + "] " + profile.getProperty ("amount", {}).getProperty ("pair_with", {}).getProperty ("control", "").toString() + " is written with the amount at every pick; the check is gated on the worse channel");
    if (record.getProperty ("pickedCandidate", {}).isObject())
    { const int want = (int) record.getProperty ("pickedCandidate", {}).getProperty ("index", -1); std::optional<sweep::Plan::Candidate> pc; for (const auto& c : plan.candidates) if (c.index == want) pc = c; if (pc) plan = plan.forCandidate (*pc); plan.candidates.clear(); }   // copy first: the assignment destroys the vector being walked
    if (plan.thr < 0) { say ("TONE: the record has several threshold candidates; pass --candidate NAME"); return 4; }
    // STEPPED BY EVIDENCE (ruled 4 Oct, Lindell 254E): before any level, the amount control is written at 41 norms (k/40) in
    // one probe process and where each write LANDED is read back; writes that land only on N values make it stepped with
    // those N detents - the evidence goes on the record (amountLanding), the profile is re-exported stepped when the swept
    // positions are those detents, and the levels below then pick detents. On-grid writes alone prove nothing.
    if (! profile.getProperty ("amount", {}).getProperty ("stepped", false) && ! record.hasProperty ("amountLanding"))
    {
        const auto lr = readAmountLanding (opt, desc, record, recordFile, plan.thr);
        if (lr.window) return kToneWindowExit;
        if (lr.detents)
        {
            const auto e2 = profile::exportCompProfile (candidate.isNotEmpty() ? profile::candidateAsSingle (record, candidate, ratioNote) : record);
            if (e2.ok) { profile = e2.profile; profileFile.replaceWithText (juce::JSON::toString (profile) + "\n", false, false, "\n"); }
            say ("TONE: " + product + " - [" + juce::String (plan.thr) + "] " + plan.thrName + " is STEPPED BY EVIDENCE: " + juce::String (*lr.detents) + " detents (k/" + juce::String (*lr.detents - 1) + "); profile re-exported "
                 + (profile.getProperty ("amount", {}).getProperty ("stepped", false) ? juce::String ("stepped - the levels below pick detents") : juce::String ("still continuous: the swept positions are not those detents (the follow-up re-sweeps it at the detents)")));
        }
        else say ("TONE: " + product + " - [" + juce::String (plan.thr) + "] " + plan.thrName + " landing at 41 norms: " + lr.note);
    }
    // the whole reference ladder below L, quiet to loud, so the picked position gets the same reference rule as the sweep
    juce::String toneLevels; { std::vector<double> q; for (const auto& [lo, hi] : sweep::kQuietLadder) { q.push_back (lo); q.push_back (hi); } std::sort (q.begin(), q.end()); for (double L : q) toneLevels << juce::String ((int) L) << ","; }

    // ONE LEVEL (v1.7 section 8, L per level ruled 3 Oct): the test L for this g by profile::toneLevelFor (or the caller's
    // override), the pick there, the writes, one fresh process at L, the GR against the pick's expected g within 0.5 dB.
    struct LevelResult { double g = 0, Lrms = 0, Lpeak = 0, Lref = 0, gapDb = 0; bool ran = false, pass = false, quietOk = false, window = false, noValidL = false; std::optional<double> gr; std::vector<double> chGrDb; profile::Pick pick; juce::String why, rule; };
    auto checkAt = [&] (double gg, const juce::String& tag) -> LevelResult
    {
        LevelResult lr; lr.g = gg;
        if (Lrms > kToneLevelByRule + 1.0) { lr.Lrms = Lrms; lr.pick = profile::pickPosition (profile, Lrms, gg); lr.rule = "L given by the caller (" + juce::String (Lrms, 2) + ")"; }
        else
        {
            const auto tl = profile::toneLevelFor (profile, gg);
            lr.rule = tl.rule; lr.Lref = tl.Lref; lr.gapDb = tl.gapDb;
            if (! tl.ok) { lr.noValidL = true; lr.why = tl.reason; say ("TONE: " + product + " - " + juce::String (gg, 1) + " dB: " + lr.why); return lr; }
            lr.Lrms = tl.L; lr.pick = tl.pick;
        }
        lr.Lpeak = lr.Lrms + profile::kPeakToSineRmsDb;
        const double Lpeak = lr.Lpeak;
        if (! lr.pick.ok) { lr.why = "section 6 picks nothing at " + juce::String (gg, 1) + " dB: " + lr.pick.refused; say ("TONE: " + product + " - " + lr.why); return lr; }
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId),
                                 "--sweep", "thr=" + juce::String (plan.thr), "norms=" + juce::String (lr.pick.norm, 6),
                                 "levels=" + toneLevels + juce::String (Lpeak, 4), "hz=997", "hold=2.5", "discard=2.2", "win=0.3", "ref=0", "moving_db=0.1", "reset=0" };
        { juce::StringArray all = sets; if (pairIdx >= 0) all.add (juce::String (pairIdx) + ":" + juce::String (lr.pick.norm, 6)); if (! all.isEmpty()) args.add ("set=" + all.joinIntoString (",")); }
        say ("TONE: " + product + " - L " + juce::String (lr.Lrms, 2) + " dBFS RMS (" + juce::String (Lpeak, 2) + " peak), g " + juce::String (gg, 1)
             + "; section 6 picks norm " + juce::String (lr.pick.norm, 4) + (lr.pick.i1 >= 0 ? " between points " + juce::String (lr.pick.i0) + " and " + juce::String (lr.pick.i1) : " at point " + juce::String (lr.pick.i0))
             + " (in_at_gr at g: " + juce::String (lr.pick.inAtG0, 2) + (lr.pick.i1 >= 0 ? " / " + juce::String (lr.pick.inAtG1, 2) : juce::String()) + "; pick's 1 dB point " + juce::String (lr.pick.pickOneDb, 2) + ")"
             + (lr.pick.note.isNotEmpty() ? "; " + lr.pick.note : juce::String()) + "; " + ratioNote);
        const auto r = runChild (args, opt.timeoutMs);
        auto raw = opt.out.getChildFile ("raw"); raw.createDirectory();
        raw.getChildFile (profileFile.getFileNameWithoutExtension() + ".tonecheck" + tag + ".1.txt").replaceWithText (r.out, false, false, "\n");
        if (r.kind == ChildResult::Kind::uiShown) { lr.window = true; lr.why = "the probe " + r.describe(); say ("TONE: " + lr.why); return lr; }
        if (! r.cleanExit()) { lr.why = "the probe " + r.describe(); say ("TONE: " + lr.why); return lr; }
        // A WRITE THAT DID NOT LAND IS NOT A FAILED CHECK (found 4 Oct on Lindell 254E: its continuous-declared Threshold snaps to
        // 1/15 steps, so an interpolated pick between detents never lands and the process renders nothing): said by name, with the
        // value the control snapped to, so the record reads "pick between the unit's real detents", not "quiet check failed"
        for (const auto& line : juce::StringArray::fromLines (r.out))
            if (line.startsWith ("pos\t") && line.contains ("write_unlanded"))
            {
                const auto f = juce::StringArray::fromTokens (line, "\t", ""); const int k = f.indexOf ("getValue");
                lr.why = "the amount write did not land: norm " + juce::String (lr.pick.norm, 4) + " snapped to " + (k >= 0 && k + 1 < f.size() ? f[k + 1] : juce::String ("?")) + " (a control declared continuous that steps - the pick fell between its real detents; no reading)";
                say ("TONE: " + product + " - " + lr.why); return lr;
            }
        lr.ran = true;
        sweep::ProcessOut po { r.out, true, r.describe(), (float) lr.pick.norm };
        const auto d = sweep::derive (sweep::mergeProcesses ({ juce::String(), true, "none", -1.0f }, { po }), { Lpeak }, plan.ratioIndex, true);
        const auto key = sweep::levelKey (Lpeak);
        lr.gr = d.reduction.count (key) && ! d.reduction.at (key).empty() ? d.reduction.at (key)[0] : std::nullopt;
        lr.quietOk = ! d.quietCheckDb.empty() && d.quietCheckDb[0] && std::abs (*d.quietCheckDb[0]) <= sweep::kQuietTolDb;
        // BOTH OUTPUT CHANNELS within 0.5 dB of g (ruled 4 Oct, with the pair rules): the probe's per-channel levels at the test hold
        // give each channel's GR as GR + (level - channel); a twin write that mirrors back onto the amount shows here as one channel off
        lr.chGrDb.clear();
        for (const auto& line : juce::StringArray::fromLines (r.out))
        {
            if (! line.startsWith ("hold\t")) continue;
            const auto f = juce::StringArray::fromTokens (line, "\t", "");
            if (f.size() < 3 || std::abs (f[2].getDoubleValue() - Lpeak) > 0.01) continue;
            const int kl = f.indexOf ("level_db"), kc = f.indexOf ("ch"); if (kl < 0 || kc < 0 || kc + 1 >= f.size() || ! lr.gr) continue;
            const double level = f[kl + 1].getDoubleValue();
            for (const auto& c : juce::StringArray::fromTokens (f[kc + 1], ",", "")) lr.chGrDb.push_back (*lr.gr + (level - c.getDoubleValue()));
        }
        bool channelsOk = true; for (double g2 : lr.chGrDb) if (std::abs (g2 - lr.pick.expectedGrDb) > 0.5) channelsOk = false;
        lr.pass = lr.gr && lr.quietOk && std::abs (*lr.gr - lr.pick.expectedGrDb) <= 0.5 && channelsOk;
        if (! channelsOk) say ("TONE: " + product + " - the output channels disagree: per-channel GR " + [&] { juce::StringArray a; for (double g2 : lr.chGrDb) a.add (juce::String (g2, 2)); return a.joinIntoString (" / "); }() + " dB against " + juce::String (lr.pick.expectedGrDb, 1) + " (a twin write mirroring onto the amount, or independent channels)");
        say ("TONE: " + product + " - GR " + (lr.gr ? juce::String (*lr.gr, 2) : juce::String ("unreadable")) + " dB at L (target " + juce::String (lr.pick.expectedGrDb, 1) + ", quiet check "
             + (lr.quietOk ? "ok" : "FAILED") + ") -> " + (lr.pass ? "PASS" : "FAIL") + " (within 0.5 dB)");
        return lr;
    };
    auto pickVar = [] (const profile::Pick& pk) { auto* p = new juce::DynamicObject(); p->setProperty ("norm", pk.norm); p->setProperty ("point", pk.i0); if (pk.i1 >= 0) p->setProperty ("point_next", pk.i1);
        p->setProperty ("in_at_g", pk.inAtG0); p->setProperty ("stepped", pk.stepped); p->setProperty ("pick_one_db", pk.pickOneDb); p->setProperty ("clamp_db", pk.clampDb); p->setProperty ("expected_gr_db", pk.expectedGrDb); if (pk.stepped) p->setProperty ("expected_extrapolated", pk.expectedExtrapolated);
        p->setProperty ("filled_across_norm", pk.filledAcrossNorm); p->setProperty ("fell_back_to_measured", pk.fellBackToMeasured); if (pk.note.isNotEmpty()) p->setProperty ("note", pk.note); return juce::var (p); };

    const auto main = checkAt (g, "");
    if (main.window) return kToneWindowExit;
    // THE WRITE FAULT (Kathy's ruling 2, 6 Oct): a reading ~0 where the curve predicts compression is first asked whether the
    // check wrote what the sweep wrote - its writes against the sweep trace's set lines; a difference is a WRITE FAULT (fix the
    // writes, re-check: never a re-sweep). Zip, 5 Oct: ratio [5] check 0.0000, sweep 0.4219.
    juce::String writeFaultNote;
    const auto viewForAb = candidate.isNotEmpty() ? profile::candidateAsSingle (record, candidate, ratioNote).getProperty ("thresholdSweep", {}) : record.getProperty ("thresholdSweep", {});
    if (main.ran && sidechaincheck::nearZeroWherePredicted (main.gr, main.pick.expectedGrDb))
    {
        std::vector<std::pair<int, double>> checkWrites; for (const auto& w : sets) checkWrites.push_back ({ w.upToFirstOccurrenceOf (":", false, false).getIntValue(), w.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
        const auto stem = recordFile.getFileNameWithoutExtension(); const juce::String prefix = candidate.isNotEmpty() ? ".c" + juce::String (plan.thr) + "." : juce::String();
        std::vector<std::pair<int, double>> traceSets; juce::String traceName;
        for (const auto& f : opt.out.getChildFile ("raw").findChildFiles (juce::File::findFiles, false, stem + ".sweep.*pos00.1.txt"))
        {
            const auto tag = f.getFileName().fromFirstOccurrenceOf (".sweep.", false, false);
            if (tag.startsWith ("r2.")) continue;
            if (prefix.isNotEmpty() && ! (".sweep." + tag).contains (prefix)) continue;
            if (prefix.isEmpty() && tag.matchesWildcard ("c*.pos*", true)) continue;
            traceSets = sidechaincheck::parseTrace (f.loadFileAsString()).sets; traceName = f.getFileName(); break;
        }
        if (traceName.isEmpty()) writeFaultNote = "near zero where " + juce::String (main.pick.expectedGrDb, 1) + " dB was predicted; no sweep trace to compare the writes against";
        else { const auto wf = sidechaincheck::writeFault (checkWrites, traceSets, plan.thr); writeFaultNote = wf.isNotEmpty() ? wf + " (sweep trace " + traceName + ")" : "near zero where " + juce::String (main.pick.expectedGrDb, 1) + " dB was predicted; the writes agree with the sweep trace " + traceName + " (not a write fault)"; }
        say ("TONE: " + product + " - " + writeFaultNote);
    }
    // THE SIDECHAIN A/B AT THE TONE CHECK'S OWN PICK (Kathy's ruling 2 and the re-verify plan, 6 Oct): a record swept under an
    // earlier sidechain policy whose unit declares an extra input gets ONE more process - the same pick, the same writes, the
    // record's own policy through the probe's test switch - and the two readings are compared: > 0.1 dB apart is policy-
    // sensitive and the record is re-swept; within it the record stands. The verdict goes on the record (sidechainPolicyCheck).
    {
        const auto sc = viewForAb.getProperty ("sidechain", {});
        const auto policyBefore = sc.getProperty ("policy", "").toString();
        const bool extraBus = sc.isObject() && sc.getProperty ("extraInputBuses", {}).size() > 0;
        const auto sw = sidechaincheck::switchFor (policyBefore);
        if (main.ran && main.gr && extraBus && policyBefore != sidechaincheck::kPolicyNow && sw.isNotEmpty() && ! writeFaultNote.startsWith ("write fault"))
        {
            juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId),
                                     "--sweep", "thr=" + juce::String (plan.thr), "norms=" + juce::String (main.pick.norm, 6),
                                     "levels=" + toneLevels + juce::String (main.Lpeak, 4), "hz=997", "hold=2.5", "discard=2.2", "win=0.3", "ref=0", "moving_db=0.1", "reset=0" };
            { juce::StringArray all = sets; if (pairIdx >= 0) all.add (juce::String (pairIdx) + ":" + juce::String (main.pick.norm, 6)); if (! all.isEmpty()) args.add ("set=" + all.joinIntoString (",")); }
            args.add ("sidechain=" + sw);
            const auto r = runChild (args, opt.timeoutMs);
            auto raw = opt.out.getChildFile ("raw"); raw.createDirectory();
            raw.getChildFile (profileFile.getFileNameWithoutExtension() + ".tonecheck.ab." + sw + ".1.txt").replaceWithText (r.out, false, false, "\n");
            auto* o = new juce::DynamicObject();
            o->setProperty ("policyBefore", policyBefore); o->setProperty ("policyNow", sidechaincheck::kPolicyNow); o->setProperty ("at", "the tone check's pick, g " + juce::String (g, 1) + ", the full write list");
            o->setProperty ("norm", main.pick.norm); o->setProperty ("level_dbfs", main.Lpeak); o->setProperty ("after_db", *main.gr);
            if (r.kind == ChildResult::Kind::uiShown) { o->setProperty ("verdict", "window"); o->setProperty ("why", "sidechain A/B: a window appeared under '" + policyBefore + "' (" + r.describe() + "); record kept"); }
            else if (! r.cleanExit()) { o->setProperty ("verdict", "crashed"); o->setProperty ("why", "sidechain A/B: the probe " + r.describe() + " under '" + policyBefore + "'; record kept"); }
            else
            {
                sweep::ProcessOut po { r.out, true, r.describe(), (float) main.pick.norm };
                const auto d = sweep::derive (sweep::mergeProcesses ({ juce::String(), true, "none", -1.0f }, { po }), { main.Lpeak }, plan.ratioIndex, true);
                const auto key = sweep::levelKey (main.Lpeak);
                const std::optional<double> before = d.reduction.count (key) && ! d.reduction.at (key).empty() ? d.reduction.at (key)[0] : std::nullopt;
                if (! before) { o->setProperty ("verdict", "not_run"); o->setProperty ("why", "sidechain A/B: no reading under '" + policyBefore + "'; record kept"); }
                else
                {
                    const double delta = *main.gr - *before;
                    o->setProperty ("before_db", *before); o->setProperty ("delta_db", delta);
                    const bool rs = std::abs (delta) > sidechaincheck::kSameDb;
                    o->setProperty ("verdict", rs ? "resweep" : "same");
                    o->setProperty ("why", juce::String ("sidechain A/B at the pick (norm ") + juce::String (main.pick.norm, 3) + ", " + juce::String (main.Lpeak, 1) + " dBFS): GR " + juce::String (*before, 2) + " dB under '" + policyBefore + "' -> " + juce::String (*main.gr, 2) + " dB under '" + sidechaincheck::kPolicyNow + "'"
                                           + (rs ? ": policy-sensitive, re-sweep" : ": the same within 0.1 dB, the record stands"));
                }
            }
            o->setProperty ("readAt", juce::Time::getCurrentTime().toISO8601 (false));
            say ("TONE: " + product + " - " + o->getProperty ("why").toString());
            { auto disk = juce::JSON::parse (recordFile.loadFileAsString()); if (auto* ro = disk.getDynamicObject()) { ro->setProperty ("sidechainPolicyCheck", juce::var (o)); recordFile.replaceWithText (juce::JSON::toString (disk) + "\n", false, false, "\n"); } }   // the record on disk, never the view
        }
    }
    // THE RANGE RE-SAMPLE (ruled 4 Oct): this product is loaded in this session anyway, so every control whose range is partial
    // is read at 21 norms now (one probe process per control, seconds) and the corrected control data written to cert/controls/.
    {
        juce::Array<juce::var> corrected; int ran = 0;
        if (const auto* cs = record.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs)
            {
                if (! c.getProperty ("range", {}).hasProperty ("range_partial") && ! c.getProperty ("range", {}).hasProperty ("endsNotNumeric")) continue;
                juce::StringArray gs; for (double n : profile::resampleNorms()) gs.add (juce::String (n, 2));
                { const auto dn = c.getProperty ("defaultOnInstantiate", {}).getProperty ("normalised", {}); if (dn.isDouble() && ! gs.contains (juce::String ((double) dn, 2))) gs.add (juce::String ((double) dn, 3)); }   // the instantiate point itself, so its text is read too
                const auto r = runChild ({ opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId), "--text-at-norms", c.getProperty ("index", -1).toString(), gs.joinIntoString (",") }, opt.timeoutMs);
                ++ran;
                if (r.kind == ChildResult::Kind::uiShown) return kToneWindowExit;
                if (! r.cleanExit()) { say ("RESAMPLE: [" + c.getProperty ("index", -1).toString() + "] " + c.getProperty ("name", "").toString() + " - the probe " + r.describe()); continue; }
                std::vector<profile::ResampleRow> rows;
                for (const auto& line : juce::StringArray::fromLines (r.out))
                {
                    const auto f = juce::StringArray::fromTokens (line, "\t", "");
                    if (f.size() >= 3 && f[0] == "at" && f.indexOf ("text") >= 0) { profile::ResampleRow row; row.norm = f[1].getDoubleValue(); row.text = f[f.indexOf ("text") + 1]; if (auto v = fixtureunit::leadingNumber (row.text)) row.value = v->value; rows.push_back (row); }
                }
                corrected.add (profile::foldResample (c, rows));
                say ("RESAMPLE: [" + c.getProperty ("index", -1).toString() + "] " + c.getProperty ("name", "").toString() + " read at " + juce::String ((int) rows.size()) + " norms: " + juce::JSON::toString (corrected[corrected.size() - 1].getProperty ("range_resampled", {}).getProperty ("min", {})) + ".." + juce::JSON::toString (corrected[corrected.size() - 1].getProperty ("range_resampled", {}).getProperty ("max", {})));
            }
        if (ran > 0)
        {
            auto dir = opt.out.getChildFile ("controls"); dir.createDirectory();
            auto* o = new juce::DynamicObject();
            o->setProperty ("identity", record.getProperty ("identity", "")); o->setProperty ("product", product); o->setProperty ("version", record.getProperty ("version", "")); o->setProperty ("map_fp", record.getProperty ("map_fp", ""));
            o->setProperty ("resampledAt", nowStamp()); o->setProperty ("note", "corrected control data for the server (range re-sampled at 21 norms where the three samples left a gap); nothing was published from EJ Map - Kathy passes this to Sean");
            o->setProperty ("controls", corrected);
            dir.getChildFile (recordFile.getFileNameWithoutExtension() + ".controls.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
        }
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("product", product); o->setProperty ("map_fp", profile.getProperty ("plugin", {}).getProperty ("map_fp", ""));
    o->setProperty ("L_rms_dbfs", main.Lrms); o->setProperty ("L_peak_dbfs", main.Lpeak); o->setProperty ("g_db", g); o->setProperty ("L_rule", main.rule); { juce::Array<juce::var> ch; for (double x : main.chGrDb) ch.add (std::round (x * 100.0) / 100.0); o->setProperty ("gr_per_channel_db", ch); } o->setProperty ("L_ref_dbfs", main.Lref); o->setProperty ("L_gap_db", std::round (main.gapDb * 100.0) / 100.0);
    o->setProperty ("pick", main.pick.ok ? pickVar (main.pick) : juce::var());
    o->setProperty ("writes", writes);
    o->setProperty ("writes_source", "exported profile: engage[], neutral[], ratio.curve[0], then the section 6 pick");
    o->setProperty ("quiet_check_ok", main.quietOk);
    o->setProperty ("gr_measured_db", main.gr ? juce::var (std::round (*main.gr * 100.0) / 100.0) : juce::var());
    o->setProperty ("pass_within_0_5_db", main.pass);
    if (writeFaultNote.isNotEmpty()) o->setProperty ("write_fault", writeFaultNote);
    if (! main.ran) o->setProperty ("why_not_run", main.why);
    o->setProperty ("probe", id.cdhash); o->setProperty ("measuredAt", juce::Time::getCurrentTime().toISO8601 (false));
    o->setProperty ("ratio_norm_source", ratioNote); o->setProperty ("spec", "v1.7");
    // THE DEEP LEVELS (v1.7 section 8): every deep level the profile carries on any position, same tolerance; a level that
    // FAILS is null across ALL positions of the exported profile and never fails the profile.
    juce::Array<juce::var> deepArr, nulled; juce::StringArray toneNullLines;
    for (int t : profile::deepLevelsCarried (profile))
    {
        const auto lr = checkAt ((double) t, ".g" + juce::String (t));
        if (lr.window) return kToneWindowExit;
        auto* dl = new juce::DynamicObject(); dl->setProperty ("g_db", (double) t); dl->setProperty ("ran", lr.ran); dl->setProperty ("gr_measured_db", lr.gr ? juce::var (std::round (*lr.gr * 100.0) / 100.0) : juce::var());
        dl->setProperty ("L_rms_dbfs", lr.ran || lr.pick.ok ? juce::var (lr.Lrms) : juce::var()); dl->setProperty ("L_rule", lr.rule); { juce::Array<juce::var> ch; for (double x : lr.chGrDb) ch.add (std::round (x * 100.0) / 100.0); dl->setProperty ("gr_per_channel_db", ch); } dl->setProperty ("L_ref_dbfs", lr.Lref); dl->setProperty ("L_gap_db", lr.ran || lr.pick.ok ? juce::var (std::round (lr.gapDb * 100.0) / 100.0) : juce::var());
        dl->setProperty ("quiet_check_ok", lr.quietOk); dl->setProperty ("pass_within_0_5_db", lr.pass); dl->setProperty ("pick", lr.pick.ok ? pickVar (lr.pick) : juce::var()); if (! lr.ran) dl->setProperty ("why_not_run", lr.why);
        // WHY A LEVEL IS NULLED (ruled 3 Oct): a failed check at the recorded L, or no valid L on the curve
        dl->setProperty ("null_reason", lr.pass ? juce::var() : lr.ran ? juce::var ("failed_check_at_L") : lr.noValidL ? juce::var ("no_valid_L") : juce::var ("could_not_run"));
        deepArr.add (juce::var (dl));
        if (! lr.pass)
        {
            nulled.add ((double) t);
            // THE NOTE LINE (v1.8 notes, ruled 3 Oct): one per (level, reason), the positions that carried the level by norm
            juce::StringArray carried; { const auto cv = profile.getProperty ("amount", {}).getProperty ("curve", {}); for (int i = 0; i < cv.size(); ++i) if (profile::inAtGr (cv[i], (double) t)) carried.add (juce::String ((double) cv[i].getProperty ("norm", 0.0), 4)); }
            const juce::String why = lr.ran ? "tone check failed (GR " + (lr.gr ? juce::String (*lr.gr, 2) : juce::String ("unreadable")) + " dB against " + juce::String (lr.pick.expectedGrDb, 2) + " expected at L " + juce::String (lr.Lrms, 2) + " dBFS RMS)"
                                   : lr.noValidL ? juce::String ("tone level untestable (no valid L in the measured range)")
                                   : "tone check could not run (" + lr.why + ")";
            toneNullLines.add ("deep null " + juce::String (t) + " dB - " + why + ": positions " + carried.joinIntoString (", "));
            profile::nullLevelAcrossPositions (profile, t);
            say ("TONE: " + product + " - the " + juce::String (t) + " dB level " + (lr.ran ? "FAILED its tone check at L " + juce::String (lr.Lrms, 2) : lr.noValidL ? juce::String ("has no valid L in the measured range") : juce::String ("could not be checked")) + ": null across all positions (the profile stands)");
        }
    }
    o->setProperty ("deep_levels", deepArr); o->setProperty ("deep_levels_nulled", nulled);
    if (! nulled.isEmpty())
    {
        if (auto* po = profile.getDynamicObject())
        {
            auto n = profile.getProperty ("notes", {});
            for (const auto& line : toneNullLines) n = profile::notesAppend (n, line);
            po->setProperty ("notes", n);
            profileFile.replaceWithText (juce::JSON::toString (profile) + "\n", false, false, "\n");
        }
    }
    const auto out = profileFile.getSiblingFile (profileFile.getFileNameWithoutExtension() + ".tonecheck.json");
    out.replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("TONE: " + product + " - g " + juce::String (g, 1) + " " + (main.pass ? "PASS" : "FAIL") + "; deep levels checked " + juce::String (deepArr.size()) + ", nulled " + juce::String (nulled.size()) + " -> " + out.getFileName());
    return profile::toneExitCode (main.pass, nulled.size());
}

//==============================================================================
// THE DETECTOR MEASUREMENT (--cert-detector <record> [--product]): one compressing position (the one whose 2 dB point is
// nearest -18 dBFS RMS), two sweeps in fresh processes - the sine and the two-tone at the same RMS - over the profile grid
// with the quiet reference; the level where each reaches 2 dB; f = shift / 3.01. Written INTO the record's thresholdSweep
// as `detector {fraction, sine_in_at_2db, twotone_in_at_2db, hz2, position_norm}`; the exporter reads it.
inline int runDetector (const SweepOptions& opt, const juce::File& recordFile, const juce::String& candidate)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {});
    if (! id.ok) { say ("DETECTOR: ABORTED - " + id.why); return 3; }
    auto record = juce::JSON::parse (recordFile.loadFileAsString());
    // A multi-threshold record: measure the detector on the picked candidate, and write the result INTO that candidate.
    int candIndex = -1;
    if (candidate.isNotEmpty() && ! record.getProperty ("thresholdSweep", {}).isObject())
    {
        juce::String why; const auto view = profile::candidateAsSingle (record, candidate, why);
        if (view.isVoid()) { say ("DETECTOR: " + why); return 2; }
        candIndex = (int) view.getProperty ("pickedCandidate", {}).getProperty ("index", -1);
    }
    auto sweepVar = candIndex >= 0 ? [&] { for (const auto& c : *record.getProperty ("thresholdCandidates", {}).getArray()) if ((int) c.getProperty ("index", -1) == candIndex) return c.getProperty ("thresholdSweep", {}); return juce::var(); }()
                                   : record.getProperty ("thresholdSweep", {});
    if (! sweepVar.isObject()) { say ("DETECTOR: the record has no thresholdSweep (candidates: pass --candidate NAME; or a refusal)"); return 2; }
    const auto product = record.getProperty ("product", "").toString();
    std::vector<InstalledRecord> hits;
    for (const auto& r : installedAudioUnits()) if (r.desc.name == product) hits.push_back (r);
    if (hits.size() != 1) { say ("DETECTOR: '" + product + "' resolves to " + juce::String ((int) hits.size()) + " component(s)"); return 2; }
    // the same two guards as the tone check, before any load: section 11 and the known licence (ruled 3 Oct)
    if (const auto vm = profile::versionMismatch (record.getProperty ("version", "").toString(), hits[0].desc.version); vm.isNotEmpty()) { say ("DETECTOR: " + product + " - " + vm); return kToneVersionExit; }
    if (const auto known = loop::knownLicenceStop (quarantinedBundles (opt.ledger), juce::JSON::parse (opt.out.getChildFile ("outcomes.json").loadFileAsString()), product, opt.retryLicence); known.isNotEmpty())
    { say ("DETECTOR: " + product + " - " + known); return kToneLicenceKnownExit; }
    auto plan = sweep::planFromFixture (record);
    if (! plan.ok) { say ("DETECTOR: no plan: " + plan.why); return 4; }
    if (candIndex >= 0) { std::optional<sweep::Plan::Candidate> pc; for (const auto& c : plan.candidates) if (c.index == candIndex) pc = c; if (pc) plan = plan.forCandidate (*pc); plan.candidates.clear(); }   // copy first (see the exporter)
    if (plan.thr < 0) { say ("DETECTOR: several threshold candidates; pass --candidate NAME"); return 4; }
    plan.makeProfile();
    // the position: numeric 2 dB point nearest -18 dBFS RMS (= -14.99 peak)
    const auto norms = sweepVar.getProperty ("positionNorms", {}); const auto inAt = sweepVar.getProperty ("inAtGr", {});
    int best = -1; double bestD = 1e9;
    for (int i = 0; i < inAt.size(); ++i) { const auto v = inAt[i].getProperty ("2", {}); if (v.isDouble() || v.isInt()) { const double d = std::abs ((double) v - (-18.0 + profile::kPeakToSineRmsDb)); if (d < bestD) { bestD = d; best = i; } } }
    if (best < 0) { say ("DETECTOR: no position has a measured 2 dB point"); return 4; }
    const float norm = (float) (double) norms[best];
    juce::StringArray sets; if (const auto* pre = sweepVar.getProperty ("preconditions", {}).getArray()) for (const auto& x : *pre) sets.add (x.getProperty ("index", -1).toString() + ":" + juce::String ((double) x.getProperty ("norm", 0.0), 6));
    if (const auto* ws = sweepVar.getProperty ("engageWrites", {}).getProperty ("writes", {}).getArray()) for (const auto& w : *ws) sets.add (w.getProperty ("index", -1).toString() + ":" + juce::String ((double) w.getProperty ("norm", 0.0), 6));
    juce::StringArray levelList; for (double L : plan.probeLevels()) levelList.add (juce::String ((int) L));
    bool twoToneSilent = false;   // every two-tone hold read silence (-999): a pitch-tracking unit that mutes an unpitched signal (Auto-Tune Vocal Compressor, ruled 6 Oct)
    auto run = [&] (bool twoTone) -> std::optional<double>
    {
        juce::StringArray args { opt.probe.getFullPathName(), hits[0].desc.name, hits[0].desc.fileOrIdentifier, juce::String::toHexString (hits[0].desc.uniqueId),
                                 "--sweep", "thr=" + juce::String (plan.thr), "norms=" + juce::String (norm, 6), "levels=" + levelList.joinIntoString (","), "hz=997",
                                 "hold=2.5", "discard=2.2", "win=0.3", "ref=0", "moving_db=0.1", "reset=0" };
        if (twoTone) args.add ("hz2=1201");
        if (! sets.isEmpty()) args.add ("set=" + sets.joinIntoString (","));
        const auto r = runChild (args, opt.timeoutMs);
        auto raw = opt.out.getChildFile ("raw"); raw.createDirectory();
        raw.getChildFile (recordFile.getFileNameWithoutExtension() + (twoTone ? ".detector.twotone.1.txt" : ".detector.sine.1.txt")).replaceWithText (r.out, false, false, "\n");
        if (! r.cleanExit()) { say ("DETECTOR: the " + juce::String (twoTone ? "two-tone" : "sine") + " process " + r.describe()); return std::nullopt; }
        if (twoTone) { int holds = 0, silent = 0; for (const auto& line : juce::StringArray::fromLines (r.out)) if (line.startsWith ("hold\t")) { ++holds; const auto f = juce::StringArray::fromTokens (line, "\t", ""); const int k = f.indexOf ("level_db"); if (k >= 0 && k + 1 < f.size() && f[k + 1].getDoubleValue() <= -900.0) ++silent; } twoToneSilent = holds > 0 && silent == holds; }
        sweep::ProcessOut po { r.out, true, r.describe(), norm };
        const auto d = sweep::derive (sweep::mergeProcesses ({ juce::String(), true, "none", -1.0f }, { po }), plan.testLevels(), plan.ratioIndex, true);
        if (d.inAtGr.empty()) { say ("DETECTOR: the " + juce::String (twoTone ? "two-tone" : "sine") + " process derived no curve: " + d.result + " - " + d.reason); return std::nullopt; }
        const auto v = d.inAtGr[0].at.count (2) ? d.inAtGr[0].at.at (2) : juce::var();
        say ("DETECTOR: " + juce::String (twoTone ? "two-tone" : "sine    ") + " at norm " + juce::String (norm, 4) + ": 2 dB reached at " + juce::JSON::toString (v, true) + " dBFS peak-equivalent (" + d.result + ")");
        return (v.isDouble() || v.isInt()) ? std::optional<double> ((double) v) : std::nullopt;
    };
    const auto sine = run (false), two = run (true);
    if (sine && ! two && twoToneSilent)
    {
        // UNMEASURABLE BY THIS METHOD (ruled 6 Oct, Auto-Tune Vocal Compressor): the sine reached 2 dB, the two-tone produced NO output at any
        // level - a pitch-tracking unit mutes an unpitched signal. Recorded as such; the export assumes rms (f = 0) and says so.
        auto* det = new juce::DynamicObject();
        det->setProperty ("fraction", juce::var()); det->setProperty ("sine_in_at_2db", *sine); det->setProperty ("twotone_in_at_2db", juce::var()); det->setProperty ("hz2", 1201.0); det->setProperty ("position_norm", norm);
        det->setProperty ("unmeasurable", "the two-tone (997 + 1201 Hz, equal RMS) produced no output at any level while the sine reached 2 dB at " + juce::String (*sine, 2) + " dBFS: a pitch-tracking unit mutes an unpitched signal; the detector fraction cannot be measured by the two-tone method");
        det->setProperty ("measuredAt", juce::Time::getCurrentTime().toISO8601 (false));
        det->setProperty ("rule", "shift between the sine's and the equal-RMS two-tone's 2 dB levels, over 3.01 dB; 0 = rms detector, 1 = peak detector");
        sweepVar.getDynamicObject()->setProperty ("detector", juce::var (det));
        recordFile.replaceWithText (juce::JSON::toString (record) + "\n", false, false, "\n");
        say ("DETECTOR: " + product + " - sine 2 dB at " + juce::String (*sine, 2) + ", the two-tone SILENT at every level: unmeasurable by this method (a pitch-tracking unit), recorded as such -> written into " + recordFile.getFileName());
        return 0;
    }
    if (! sine || ! two) { say ("DETECTOR: " + product + " - a 2 dB point was not reached on one signal; nothing recorded"); return 1; }
    const double f = profile::detectorFraction (*sine, *two);
    auto* det = new juce::DynamicObject();
    det->setProperty ("fraction", std::round (f * 100.0) / 100.0); det->setProperty ("sine_in_at_2db", *sine); det->setProperty ("twotone_in_at_2db", *two);
    det->setProperty ("hz2", 1201.0); det->setProperty ("position_norm", norm); det->setProperty ("measuredAt", juce::Time::getCurrentTime().toISO8601 (false));
    det->setProperty ("rule", "shift between the sine's and the equal-RMS two-tone's 2 dB levels, over 3.01 dB; 0 = rms detector, 1 = peak detector");
    sweepVar.getDynamicObject()->setProperty ("detector", juce::var (det));
    recordFile.replaceWithText (juce::JSON::toString (record) + "\n", false, false, "\n");
    say ("DETECTOR: " + product + " - sine 2 dB at " + juce::String (*sine, 2) + ", two-tone at " + juce::String (*two, 2) + ": shift " + juce::String (*sine - *two, 2)
         + " dB -> f = " + juce::String (f, 2) + " (" + profile::detectorWord (f) + ") -> written into " + recordFile.getFileName());
    return 0;
}

//==============================================================================
// THE SWEEP CENSUS (--cert-sweep-census): which fixtures a sweep can run on HERE, read-only, nothing
// instantiated. A product is runnable when it is measurable (the version guard is on comparison, not
// measurement) AND its plan finds exactly one threshold. For an unseen version the plan is predicted from
// the pushed fixture's controls; the real plan is made from the defaults sampled at the installed version.
inline int runSweepCensus (const juce::File& fixturesDir, const juce::File& ledgerRoot, bool includePace, bool retryRefused, bool retryAll)
{
    juce::StringArray wl;
    auto subjects = buildWorklist (fixturesDir, ledgerRoot, includePace, wl, retryRefused, retryAll);
    int runnable = 0, runnableNewIdentity = 0, runnableDiscovered = 0;
    std::map<juce::String, juce::StringArray> notRunnable;
    juce::StringArray rows;
    for (const auto& s : subjects)
    {
        const bool discovered = s.reach == Subject::Reach::unfixtured;
        const auto plan = sweep::planFromFixture (s.pushed);
        if (! discovered && ! plan.ok)
        {
            notRunnable[plan.why.contains ("unity") ? "ratio instantiates at 1:1 - the section 4.2 raise to 4:1 is not built (class " + plan.cls + ")"
                                                    : "no single threshold (" + plan.cls + ") - deferred to review after the sweep"].add (s.product);
            continue;
        }
        if (! measurable (s, includePace))
        {
            juce::String why = reachName (s.reach);
            if (s.reach == Subject::Reach::versionMismatch)
                why = ! s.installedUnique ? why + " - several versions installed"
                    : s.hardware ? "CONDITIONAL - " + s.detail.fromFirstOccurrenceOf ("; ", false, false) + " (one re-run once attached)"
                    : why + " - licence-bound";
            if (s.reach == Subject::Reach::requiresHardware) why = "CONDITIONAL - " + s.detail.upToFirstOccurrenceOf (";", false, false) + " (one re-run once attached)";
            if (s.reach == Subject::Reach::unfixtured)
                why = s.hardware ? "discovered, CONDITIONAL - " + s.detail.fromFirstOccurrenceOf ("; ", false, false) + " (one re-run once attached)"
                                 : juce::String ("discovered, licence-bound (run with --include-pace)");
            notRunnable[why].add (s.product);
            continue;
        }
        ++runnable;
        const bool fresh = s.reach == Subject::Reach::versionMismatch;
        if (fresh) ++runnableNewIdentity;
        if (discovered) ++runnableDiscovered;
        rows.add ("  " + s.product.paddedRight (' ', 40)
                  + (! plan.candidates.empty() ? juce::String ((int) plan.candidates.size()) + " CANDIDATE thresholds (" + plan.cls + "), each swept and labelled for review; " : juce::String())
                  + (discovered ? "DISCOVERED at " + s.desc.version + ", " + s.detail + " (defaults first; plan after sampling)"
                     : fresh ? "NEW IDENTITY at installed " + s.desc.version + " (defaults first)" : juce::String ("at the fixture's version"))
                  + (s.licenceBound ? "  [licence-bound]" : ""));
    }
    std::cout << "SWEEP CENSUS - PACE " << (includePace ? "included" : "held") << "\n" << wl.joinIntoString ("\n") << "\n"
              << "RUNNABLE: " << runnable << " (" << runnableNewIdentity << " as a new identity at the installed version, "
              << runnableDiscovered << " discovered with no fixture at all)\n"
              << rows.joinIntoString ("\n") << "\nNOT RUNNABLE, BY REASON\n";
    for (const auto& [why, names] : notRunnable)
        std::cout << "  " << why << " (" << names.size() << "): " << names.joinIntoString (", ") << "\n";
    {
        const auto qb = quarantinedBundles (ledgerRoot);
        int certCats = 0, lic = 0, licCert = 0; for (const auto& b : qb) { if (b.licence) { ++lic; if (loop::certificationCategory (b.category)) ++licCert; } else if (loop::certificationCategory (b.category)) ++certCats; }
        std::cout << "QUARANTINED AT SCAN (never reach this census): " << (int) qb.size() - lic << ", of which compressors or tuners: " << certCats << "\n";
        for (const auto& b : qb) if (! b.licence)
            std::cout << "  " << b.products.joinIntoString (", ") << "  [" << b.category << "]  " << b.reason << " at " << b.stage << (b.vst3 ? "  (VST3 bundle; the AudioUnit is unaffected if it scanned)" : "") << "\n";
        std::cout << "NEEDS LICENCE (activation window at scan; --scan --retry-licence once the licence is back): " << lic << ", of which compressors or tuners: " << licCert << "\n";
        for (const auto& b : qb) if (b.licence)
            std::cout << "  " << b.products.joinIntoString (", ") << "  [" << b.category << "]  " << b.reason << "  " << b.at << (b.vst3 ? "  (VST3 bundle; the AudioUnit is unaffected if it scanned)" : "") << "\n";
    }
    std::cout << std::flush;
    return 0;
}

//==============================================================================
// ONE PROBE INVOCATION through the shipped machinery (--cert-probe-once): the same
// signature gate, the same runChild, the default PRODUCTION window watch, and NO
// retry. It exists for the step-2 question, which is one plugin and one mode, not a
// fixture. A window is a result here, exactly as in --cert-defaults.
inline int runProbeOnce (const juce::File& probe, const juce::StringArray& probeArgs, int timeoutMs)
{
    const auto id = checkProbe (probe, {}, {});
    if (! id.ok) { std::cout << "PROBE-ONCE: ABORTED BEFORE ANY PLUGIN - " << id.why << std::endl; return 3; }
    std::cout << "probe: team " << id.team << ", cdhash " << id.cdhash << std::endl;
    juce::StringArray args { probe.getFullPathName() };
    args.addArray (probeArgs);
    const auto r = runChild (args, timeoutMs);
    const auto rows = parseListParams (r.out);
    std::cout << "outcome: " << r.describe() << "\n"
              << "windows in the probe's tree: " << (r.windowsInTree.isEmpty() ? juce::String ("none") : r.windowsInTree.joinIntoString (", ")) << "\n"
              << "parameter rows: " << (int) rows.size() << "\n"
              << "---- probe output ----\n" << r.out << "---- end ----" << std::endl;
    return r.cleanExit() ? 0 : 1;
}

//==============================================================================
// SELF-TEST OF THE WINDOW WATCH (--cert-watch-selftest <helper>). The helper orders
// in one 10x10 borderless window 20,000 px off every display and waits 20 s. It is
// invisible to the user, but CoreGraphics lists it as ON SCREEN (measured 28 Sep),
// so the test runs the PRODUCTION setting. Three cases:
//   - the helper as the direct child: caught, killed at once, not a timeout
//   - the helper as a GRANDCHILD (via /bin/sh, forced to fork): PACE's activation
//     UI is a child of the probe, one level below what the driver spawns
//   - a child that shows no window: must exit cleanly, untouched
// THE SCAN WINDOW WATCH SELF-TEST (ruled 2 Oct): the child arms the watchdog at the scan stage for a fake bundle,
// shows a window (off-screen, so nothing flashes on the desktop; the watch is told so), and must be killed by the watch
// within seconds with the licence-stops entry on disk; a child that shows no window must come back clean. The parent
// judges both. Run by the rehearsal's pre-flight, and whenever the watch changes.
inline int runScanWatchSelfTestChild (const juce::File& root, bool showWindow)
{
    juce::ScopedJuceInitialiser_GUI gui;
    root.createDirectory();
    Ledger ledger (root);
    Watchdog dog (ledger);
    dog.setWindowWatch (true, /*onScreenOnly*/ false);
    std::unique_ptr<juce::DocumentWindow> w;
    {
        Watchdog::Scope guard (dog, "findAllTypesForFile", "/selftest/FakeLicence.vst3", "FakeLicence", "VST3", "scan", 10000);
        if (showWindow)
        {
            w = std::make_unique<juce::DocumentWindow> ("PACE selftest window", juce::Colours::black, 0);
            w->setBounds (-10000, -10000, 200, 100);   // off-screen: a window to the window server, nothing on the desktop
            w->setVisible (true);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (3000);
    }
    w.reset();
    return 0;   // reached only when the watch did NOT fire
}
inline int runScanWatchSelfTest (const juce::File& executable)
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ejmap-scanwatch-selftest-" + juce::Uuid().toDashedString());
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto shown = runChild ({ executable.getFullPathName(), "--scan-watch-selftest-child", root.getFullPathName(), "window" }, 15000, WatchOptions { false });
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    const auto stops = juce::JSON::parse (root.getChildFile ("licence-stops.json").loadFileAsString());
    const bool stopped = stops.isArray() && stops.size() == 1 && stops[0].getProperty ("plugin_id", "").toString() == "/selftest/FakeLicence.vst3"
                         && stops[0].getProperty ("state", "").toString() == "needs_licence";
    const auto quiet = runChild ({ executable.getFullPathName(), "--scan-watch-selftest-child", root.getChildFile ("quiet").getFullPathName(), "none" }, 15000, WatchOptions { false });
    std::cout << "window shown:     " << shown.describe() << "  (" << juce::String (ms / 1000.0, 1) << " s to the kill; licence-stops entry " << (stopped ? "written" : "MISSING") << ")" << std::endl;
    std::cout << "no window (ctrl): " << quiet.describe() << std::endl;
    const bool ok = shown.kind == ChildResult::Kind::exited && shown.code == kLicenceStopExitCode && ms < 6000.0 && stopped && quiet.cleanExit();
    std::cout << (ok ? "SCAN WATCH SELFTEST: GREEN" : "SCAN WATCH SELFTEST: RED") << std::endl;
    root.deleteRecursively();
    return ok ? 0 : 1;
}

inline int runWatchSelfTest (const juce::File& helper)
{
    auto direct = runChild ({ helper.getFullPathName() }, 15000);
    std::cout << "direct child:     " << direct.describe() << std::endl;
    auto grand = runChild ({ "/bin/sh", "-c", "'" + helper.getFullPathName() + "'; true" }, 15000);
    std::cout << "grandchild:       " << grand.describe() << std::endl;
    auto quiet = runChild ({ "/bin/sleep", "1" }, 15000);
    std::cout << "no window (ctrl): " << quiet.describe() << std::endl;
    const bool ok = direct.kind == ChildResult::Kind::uiShown && direct.ms < 5000.0
                 && grand.kind == ChildResult::Kind::uiShown && grand.ms < 5000.0 && quiet.cleanExit();
    std::cout << (ok ? "WATCH SELFTEST: GREEN" : "WATCH SELFTEST: RED") << std::endl;
    return ok ? 0 : 1;
}


//==============================================================================
// THE ZIP REVIEW (5 Oct, R1): --cert-review-zip <zip|folder> [--against <zip|folder>]. Unzips both to a scratch folder
// under the temp directory, reads the follow-up as data, runs the follow-up's own decision pass derive-only over a COPY of
// the baseline for the projection, and prints one report (EjmapCertReview.h). Loads nothing, sends nothing, never
// touches ~/Library/ejmap.
inline juce::File reviewCertRoot (const juce::File& unpacked)
{
    if (unpacked.getChildFile ("cert").getChildFile ("outcomes.json").existsAsFile()) return unpacked.getChildFile ("cert");
    if (unpacked.getChildFile ("outcomes.json").existsAsFile()) return unpacked;
    // a zip whose cert/ sits one folder down (zipped from a parent): the first outcomes.json found
    for (const auto& f : unpacked.findChildFiles (juce::File::findFiles, true, "outcomes.json")) return f.getParentDirectory();
    return {};
}
// A zip is uncompressed; a folder is copied (as cert/ whatever it was called) so the projection's writes never reach the
// caller's folder. entries lists what the ZIP held (hygiene reads the zip's own names, never the unpacked tree).
inline bool unpackForReview (const juce::File& src, const juce::File& dst, juce::StringArray& entries, juce::String& why)
{
    dst.createDirectory();
    if (src.isDirectory())
    {
        const auto cert = src.getChildFile ("outcomes.json").existsAsFile() ? src : src.getChildFile ("cert");
        if (! cert.getChildFile ("outcomes.json").existsAsFile()) { why = "no outcomes.json in " + src.getFullPathName() + " or its cert/"; return false; }
        if (! cert.copyDirectoryTo (dst.getChildFile ("cert"))) { why = "could not copy " + cert.getFullPathName(); return false; }
        for (const auto& f : cert.findChildFiles (juce::File::findFiles, true)) entries.add ("cert/" + f.getRelativePathFrom (cert));
        return true;
    }
    if (! src.existsAsFile()) { why = "not found: " + src.getFullPathName(); return false; }
    juce::ZipFile zip (src);
    if (zip.getNumEntries() == 0) { why = "not a zip, or empty: " + src.getFullPathName(); return false; }
    for (int i = 0; i < zip.getNumEntries(); ++i) entries.add (zip.getEntry (i)->filename);
    const auto r = zip.uncompressTo (dst, true);
    if (r.failed()) { why = "unzip failed: " + r.getErrorMessage(); return false; }
    return true;
}
inline std::map<juce::String, juce::var> reviewRecords (const juce::File& cert, std::map<juce::String, juce::var>* byStem = nullptr)
{
    std::map<juce::String, juce::var> out;
    for (const auto& f : cert.getChildFile ("fixtures").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (f.getFileName().endsWith (".defaults.json")) continue;
        const auto r = juce::JSON::parse (f.loadFileAsString()); if (! r.isObject() || ! r.hasProperty ("product")) continue;
        const auto product = r.getProperty ("product", "").toString();
        if (byStem) (*byStem)[f.getFileNameWithoutExtension()] = r;
        const auto it = out.find (product);
        if (it == out.end() || review::recordStamp (r) > review::recordStamp (it->second)) out[product] = r;   // the latest record speaks for a product
    }
    return out;
}
inline int runReviewZip (const juce::File& subject, const juce::File& against, const juce::File& scratchRoot, bool keepScratch = false)
{
    using namespace review;
    ReportInput in;
    const auto scratch = scratchRoot.getChildFile ("ejmap-review-" + nowStamp());
    in.subjectName = subject.getFullPathName(); in.scratchDir = scratch.getFullPathName();
    juce::StringArray entries; juce::String why;
    if (! unpackForReview (subject, scratch.getChildFile ("subject"), entries, why)) { std::cout << "ZIP REVIEW: cannot read the subject - " << why << std::endl; return 2; }
    const auto cert = reviewCertRoot (scratch.getChildFile ("subject"));
    if (cert == juce::File()) { std::cout << "ZIP REVIEW: no outcomes.json anywhere in " << subject.getFullPathName() << std::endl; return 2; }
    in.hygiene = hygieneOf (entries); in.hygieneKnown = true;
    const auto outcomes = juce::JSON::parse (cert.getChildFile ("outcomes.json").loadFileAsString());
    std::map<juce::String, juce::var> byStem; const auto records = reviewRecords (cert, &byStem);
    auto rowFor = [&] (const juce::String& product) -> juce::var { if (const auto* a = outcomes.getArray()) for (const auto& r : *a) if (r.getProperty ("product", "").toString() == product) return r; return {}; };
    auto recordFor = [&] (const std::map<juce::String, juce::var>& m, const juce::String& product) -> juce::var { const auto it = m.find (product); return it == m.end() ? juce::var() : it->second; };
    // the log, the probes
    if (const auto log = cert.getChildFile ("tonecheck.log"); log.existsAsFile())
    { in.logPresent = true; for (const auto& l : juce::StringArray::fromLines (log.loadFileAsString())) if (l.trim().isNotEmpty()) in.logLastLine = l.trim().substring (0, 160); }
    // THE BASELINE: its outcomes and records first (the projection pass rewrites the copy), then the projection
    juce::var baseOutcomes; std::map<juce::String, juce::var> baseRecords; FollowUpProjection proj; int baseRunLines = 0;
    if (against != juce::File())
    {
        juce::StringArray bEntries; juce::String bWhy;
        if (! unpackForReview (against, scratch.getChildFile ("baseline"), bEntries, bWhy)) { std::cout << "ZIP REVIEW: cannot read the baseline - " << bWhy << std::endl; return 2; }
        const auto bcert = reviewCertRoot (scratch.getChildFile ("baseline"));
        if (bcert == juce::File()) { std::cout << "ZIP REVIEW: no outcomes.json anywhere in " << against.getFullPathName() << std::endl; return 2; }
        in.baselineName = against.getFullPathName(); in.baselineKnown = true;
        baseOutcomes = juce::JSON::parse (bcert.getChildFile ("outcomes.json").loadFileAsString());
        baseRecords = reviewRecords (bcert);
        for (const auto& l : juce::StringArray::fromLines (bcert.getChildFile ("run.jsonl").loadFileAsString())) if (l.trim().isNotEmpty()) ++baseRunLines;
        in.states = diffOutcomes (baseOutcomes, outcomes);
        // the projection: the follow-up's decision pass, derive-only, over the baseline COPY; its output is kept in the scratch folder
        SweepOptions o; o.out = bcert; o.deriveOnly = true; o.projection = &proj; o.ledger = scratch.getChildFile ("empty-ledger"); o.ledger.createDirectory();
        std::ostringstream cap; auto* old = std::cout.rdbuf (cap.rdbuf());
        const int rc = runToneCheckAll (o);
        std::cout.rdbuf (old);
        scratch.getChildFile ("projection.log").replaceWithText (cap.str(), false, false, "\n");
        in.projectionKnown = rc == 0;
        if (rc != 0) in.note = "the projection pass over the baseline returned " + juce::String (rc) + " (see projection.log in the scratch folder)";
        juce::StringArray projected;
        for (const auto& it : proj.resweeps) { projected.add (it.product); in.resweeps.push_back (resweepStatus (it.product, it.why, it.afterRederive, recordFor (baseRecords, it.product), recordFor (records, it.product), rowFor (it.product))); }
        in.unprojected = unprojectedResweeps (baseRecords, records, projected);
        for (const auto& it : proj.sidechain) in.sidechain.push_back (sidechainStatus (it.product, recordFor (records, it.product), rowFor (it.product)));
    }
    else
    {
        if (const auto* a = outcomes.getArray()) for (const auto& r : *a) ++in.states.after[r.getProperty ("state", "").toString()];
        // no baseline: the sidechain set is every follow-up record that carries a reading
        for (const auto& [product, r] : records) if (r.getProperty ("sidechainPolicyCheck", {}).isObject()) in.sidechain.push_back (sidechainStatus (product, r, rowFor (product)));
    }
    // review picks
    if (const auto pf = cert.getChildFile ("review_picks.json"); pf.existsAsFile())
    {
        in.picksFilePresent = true;
        const auto picks = juce::JSON::parse (pf.loadFileAsString());
        if (const auto* a = picks.getArray()) for (const auto& p : *a) { ++in.pickEntries; in.picks.push_back (pickStatus (p, recordFor (records, p.getProperty ("product", "").toString()), rowFor (p.getProperty ("product", "").toString()))); }
    }
    // tone checks and deep points, from the profiles folder by file (never a path from a row)
    for (const auto& f : cert.getChildFile ("profiles").findChildFiles (juce::File::findFiles, false, "*.tonecheck.json"))
    {
        const auto tc = juce::JSON::parse (f.loadFileAsString());
        in.tones.push_back (toneSummary (tc.getProperty ("product", f.getFileNameWithoutExtension()).toString(), tc));
        const auto probe = tc.getProperty ("probe", "").toString(); if (probe.isNotEmpty()) ++in.probesSeen[probe.substring (0, 8)];
    }
    for (const auto& f : cert.getChildFile ("profiles").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (f.getFileName().endsWith (".tonecheck.json")) continue;
        const auto p = juce::JSON::parse (f.loadFileAsString());
        in.deep.push_back (deepPointsOf (p.getProperty ("plugin", {}).getProperty ("name", f.getFileNameWithoutExtension()).toString(), p));
    }
    auto byProduct = [] (auto& v) { std::sort (v.begin(), v.end(), [] (const auto& a, const auto& b) { return a.product.compareIgnoreCase (b.product) < 0; }); };
    byProduct (in.tones); byProduct (in.deep);
    // inert, licence, crashes
    in.inert = inertLines (records);
    if (const auto* a = outcomes.getArray()) for (const auto& r : *a) if (r.getProperty ("state", "").toString() == "needs_licence")
    {
        const auto product = r.getProperty ("product", "").toString(), category = r.getProperty ("category", "").toString();
        if (records.count (product) || category == "compressor" || category == "pitch") in.licence.push_back ({ product, r.getProperty ("reason", "").toString() });
        else ++in.licenceOther[category.isEmpty() ? juce::String ("unknown") : category];
    }
    const auto runText = cert.getChildFile ("run.jsonl").loadFileAsString();
    int runLines = 0; for (const auto& l : juce::StringArray::fromLines (runText)) if (l.trim().isNotEmpty()) ++runLines;
    in.runLinesAdded = juce::jmax (0, runLines - baseRunLines);
    in.crashes = crashesIn (runText, baseRunLines, outcomes, byStem);
    std::sort (in.resweeps.begin(), in.resweeps.end(), [] (const ResweepLine& a, const ResweepLine& b) { return a.product.compareIgnoreCase (b.product) < 0; });
    std::sort (in.sidechain.begin(), in.sidechain.end(), [] (const SidechainLine& a, const SidechainLine& b) { return a.word == b.word ? a.product.compareIgnoreCase (b.product) < 0 : a.word < b.word; });
    if (! keepScratch) in.scratchDir = {};   // deleted below: two unzipped folders are ~800 MB; --keep leaves them and says where
    std::cout << render (in);
    if (! keepScratch) scratch.deleteRecursively();
    return 0;
}

} // namespace ejmap::cert
