/*  V2 READ-AND-LOCK LEGS (9 Sep 2026 ruling). Main archive only. A hand-built FAKE LINK: registry
    rows + rack sidecars planted directly, and a responder thread that answers ctrl-cmd pull requests
    in ~200 ms (the measured Link latency). No LinkProcessor - the leg asserts what is OURS, the V2
    read loop and the lock, never a real Link's behaviour. Isolated root required.
      seed  - select A (6 slots), 500 ms later select B (0 slots) mid-read. Assert B's session
              engages with 0 slots and A's read is abandoned. Build F seeds B with A's six - FAILS.
      loops - one read loop per rack: count the pull REQUESTS made for A during one read of six
              slots. G: 6 (or 7 with the one re-request). Build F: the 1 Hz feeder restarts the
              loop, so the count climbs far past 6.
      lock  - the lock belongs to the session. Viewing A (no click) holds no lock file; a click and
              the live session hold it; session end releases it. Build F holds it for the view - FAILS.
    Scale: N extra Links planted (default 40, Sean's size) so the registry pass and feeder run at scale. */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LinkShm.h"
#include <cstdio>
#include <thread>
#include <atomic>
#include <map>
#if __has_include("legs_link_v9.h")
  #define EJ_LEGS_V2_G 1
#endif

static void pump (int iters) { for (int t = 0; t < iters; ++t) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
static void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }

struct EchoJayTabStripTestAccess
{
    static void click (EchoJayEditor& e, const juce::String& uid) { e.pendingSelectionIsUser_ = true; e.openChannelByUid (uid); }
    static void view  (EchoJayEditor& e, const juce::String& uid) { e.pendingSelectionIsUser_ = false; e.openChannelByUid (uid); }
    static void feed  (EchoJayEditor& e) { e.refreshLinkRackCache (true); }
    static void showChain (EchoJayEditor& e) { e.currentTab = EchoJayEditor::Tab::Chain; e.chainListPanel.setVisible (true); }
};
using ED = EchoJayTabStripTestAccess;

// Plant one registry row; returns its index. heartbeat left at 1, bumped by the caller.
static int plantRow (void* reg, const juce::String& uid, const juce::String& name)
{
    auto* slots = LinkShm::regSlots (reg);
    for (int i = 0; i < kRegMaxSlots; ++i)
        if (LinkShm::loadAcquire (&slots[i].inUse) == 0)
        {
            std::memset (&slots[i], 0, sizeof (RegistrySlot));
            std::strncpy (slots[i].displayName, name.toRawUTF8(), 39);
            std::strncpy (slots[i].instanceUid, uid.toRawUTF8(), 11);
            std::strncpy (slots[i].audioFile, ("audio_" + uid + ".bin").toRawUTF8(), 47);
            slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2; slots[i].placement = 0;
            LinkShm::storeRelease (&slots[i].heartbeat, 1u);
            LinkShm::storeRelease (&slots[i].inUse, 1u);
            return i;
        }
    return -1;
}
static void writeSidecar (const juce::String& dir, const juce::String& uid, const juce::String& name, int nSlots)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("v", 1); o->setProperty ("uid", uid); o->setProperty ("name", name);
    o->setProperty ("revision", 2); o->setProperty ("masterWet", 1.0);
    o->setProperty ("borrowCapable", true); o->setProperty ("structureEditCapable", true);
    o->setProperty ("inContextCapable", true); o->setProperty ("muteSoloCapable", true);
    o->setProperty ("ackPerSeq", true);   // v9 reads this; Build F ignores it and uses the legacy file
    o->setProperty ("publisherPid", (int) getpid()); o->setProperty ("hostPid", (int) getpid());
    juce::Array<juce::var> arr;
    for (int i = 0; i < nSlots; ++i)
    {
        auto* s = new juce::DynamicObject();
        s->setProperty ("name", "EchoJay EQ"); s->setProperty ("format", juce::String());   // built-in: loads with no scan
        s->setProperty ("settings", juce::String()); s->setProperty ("bypassed", false); s->setProperty ("wet", 1.0);
        s->setProperty ("fp", juce::String()); s->setProperty ("version", "1.0");
        arr.add (juce::var (s));
    }
    o->setProperty ("slots", arr);
    juce::File (LinkShm::rackSidecarPath (dir, uid)).replaceWithText (juce::JSON::toString (juce::var (o), true));
}

// The fake Link: answer ctrl-cmd-<uid>.json pull requests in ~200 ms, writing BOTH the legacy ack
// and the per-seq ack so either binary reads it. Runs on its own thread for the leg's lifetime.
struct FakeLink
{
    juce::String dir; std::vector<juce::String> uids; std::atomic<bool> stop { false }; std::thread th;
    std::map<juce::String, int> lastSeq; std::map<juce::String, double> dueAt; std::map<juce::String,int> totalReqs;
    void start (const juce::String& d, std::vector<juce::String> u)
    {
        dir = d; uids = std::move (u);
        th = std::thread ([this]{
            while (! stop.load())
            {
                const double now = juce::Time::getMillisecondCounterHiRes();
                for (const auto& uid : uids)
                {
                    juce::File cmd (dir + "ctrl-cmd-" + uid + ".json");
                    if (! cmd.existsAsFile()) continue;
                    auto v = juce::JSON::parse (cmd.loadFileAsString());
                    auto* o = v.getDynamicObject(); if (! o) continue;
                    const int seq = (int) o->getProperty ("seq");
                    if (! o->hasProperty ("pullSlotState")) continue;
                    if (lastSeq.count (uid) && lastSeq[uid] == seq) { if (now >= dueAt[uid]) answer (uid, seq); continue; }
                    lastSeq[uid] = seq; dueAt[uid] = now + 200.0; totalReqs[uid]++;   // 200 ms latency, like the real Link
                }
                juce::Thread::sleep (10);
            }
        });
    }
    void answer (const juce::String& uid, int seq)
    {
        auto* a = new juce::DynamicObject();
        a->setProperty ("v", 1); a->setProperty ("seq", seq);
        a->setProperty ("pulledState", true);
        a->setProperty ("slotState", juce::Base64::toBase64 ("STATE"));
        const juce::String txt = juce::JSON::toString (juce::var (a), true);
        juce::File (dir + "ctrl-ack-" + uid + ".json").replaceWithText (txt);
        juce::File (dir + "ctrl-ack-" + uid + "-" + juce::String (seq) + ".json").replaceWithText (txt);
        dueAt[uid] = juce::Time::getMillisecondCounterHiRes() + 1e9;   // answered; wait for the next seq
    }
    ~FakeLink() { stop.store (true); if (th.joinable()) th.join(); }
};

struct World
{
    juce::String dir, A, B; void* reg = nullptr; int fd = -1; std::vector<int> rows;
    std::unique_ptr<EchoJayProcessor> main; EchoJayEditor* ed = nullptr; FakeLink link;
    void stand (int extra)
    {
        int err = 0; dir = LinkShm::resolveDir (err); int rerr = 0; reg = LinkShm::openRegistry (dir, fd, rerr);
        A = "a10000a001"; B = "b20000b002";
        rows.push_back (plantRow (reg, A, "Aitch Lead Vocal"));
        rows.push_back (plantRow (reg, B, "Nafe Lead Vocal"));
        writeSidecar (dir, A, "Aitch Lead Vocal", 6);
        writeSidecar (dir, B, "Nafe Lead Vocal", 2);
        for (int i = 0; i < extra; ++i) { const juce::String u = "e" + juce::String::toHexString (0x30000 + i).paddedLeft ('0', 9).substring (0, 9);
            rows.push_back (plantRow (reg, u, "T" + juce::String (i + 1))); writeSidecar (dir, u, "T" + juce::String (i + 1), 0); }
        link.start (dir, { A, B });
        main = std::make_unique<EchoJayProcessor>(); main->prepareToPlay (48000.0, 512);
        for (int t = 0; t < 10; ++t) { auto* sl = LinkShm::regSlots (reg); for (int r : rows) LinkShm::storeRelease (&sl[r].heartbeat, (uint32_t) (2 + t)); main->refreshLinkRegistry(); pump (3); }
        ed = dynamic_cast<EchoJayEditor*> (main->createEditor());
        ED::showChain (*ed);   // the Chain tab is what Sean was on; Build F derives its lock from this
        for (int i = 0; i < 60; ++i) { ED::feed (*ed); pump (3);
            if (main->linkRackCache.count (A) && main->linkRackCache[A].rack.valid && (int) main->linkRackCache[A].rack.slots.size() == 6) break; }
        std::printf ("  world: %d rows, A cache valid=%d slots=%d borrowCapable=%d\n", (int) rows.size(),
                     (int) main->linkRackCache[A].rack.valid, (int) main->linkRackCache[A].rack.slots.size(), (int) main->linkRackCache[A].rack.borrowCapable);
    }
    bool lockFile (const juce::String& uid) const { return juce::File (LinkShm::racklockPath (dir, uid)).existsAsFile(); }
    int  sessionSlots() { auto* h = main->borrowHostIfActiveFor (main->borrowUid()); return h ? h->getNumSlots() : -1; }
    void keepAlive() { auto* sl = LinkShm::regSlots (reg); static uint32_t hb = 20; for (int r : rows) LinkShm::storeRelease (&sl[r].heartbeat, hb); ++hb; }
    ~World() { if (ed) delete ed; main.reset(); if (reg) LinkShm::closeRegistry (reg, fd); }
};

static void driveFor (World& w, int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { w.keepAlive(); ED::feed (*w.ed); pumpMs (60); } }
// wait for the session, driving the feeder (which runs borrowSelectionTick) the whole time
static bool waitEngaged (World& w, int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (! w.main->borrowActive() && juce::Time::getMillisecondCounterHiRes() < end) { w.keepAlive(); ED::feed (*w.ed); pumpMs (40); } return w.main->borrowActive(); }

static int legSeed (int extra)
{
    World w; w.stand (extra);
    ED::click (*w.ed, w.A);                          // A's read starts (6 slots at ~250 ms each => ~1.5 s)
    driveFor (w, 500);
    ED::click (*w.ed, w.B);                          // the switch, mid-read
    // Capture at the FIRST engage after the switch. The fake Link produces no audio ring, so the
    // in-context session self-releases ~3 s later; the seed correctness is settled at engage.
    const bool engaged = waitEngaged (w, 10000);
    const juce::String su = w.main->borrowUid(); const int n = w.sessionSlots();
    // B's own rack has 2 slots. If A's stale read had seeded THIS session it would hold A's 6 (or 8).
    const bool pass = engaged && su == w.B && n == 2;
    std::printf ("seed: first session after the switch uid=%s (B=%s) slots=%d (B has 2; A had 6) -> %s\n",
                 su.toRawUTF8(), w.B.toRawUTF8(), n, pass ? "PASS" : (n == 6 || n == 8 ? "FAIL (A's read seeded B's session)" : "FAIL"));
    return pass ? 0 : 1;
}
static int legLoops (int extra)
{
    World w; w.stand (extra);
    ED::click (*w.ed, w.A);
    waitEngaged (w, 12000);
    driveFor (w, 1500);
    const int handled = (int) w.link.lastSeq.size() ? 0 : 0;   // placeholder; real count below
    // Count distinct seqs the fake Link saw for A: the responder tracked lastSeq, but we want the
    // TOTAL requests. Re-derive from a request counter kept on the FakeLink.
    const bool engaged = w.main->borrowActive() && w.main->borrowUid() == w.A && w.sessionSlots() == 6;
    const int reqs = w.link.totalReqs.count (w.A) ? w.link.totalReqs[w.A] : -1;
    const bool pass = engaged && reqs >= 6 && reqs <= 8;
    std::printf ("loops: engaged=%d session slots=%d, pull requests for A=%d (one read = 6, +1 re-request allowed) -> %s\n",
                 (int) engaged, w.sessionSlots(), reqs, pass ? "PASS" : "FAIL");
    (void) handled; return pass ? 0 : 1;
}
static int legLock (int extra)
{
    World w; w.stand (extra); bool ok = true;
    ED::view (*w.ed, w.A); driveFor (w, 2500);
    const bool viewHolds = w.lockFile (w.A);
    std::printf ("lock: viewing A without a click -> lock file=%d (must be 0)\n", (int) viewHolds); ok = ok && ! viewHolds;
    ED::click (*w.ed, w.A);
    waitEngaged (w, 12000);
    driveFor (w, 500);
    const bool sessionHolds = w.lockFile (w.A);
    std::printf ("lock: session engaged=%d -> lock file=%d (must be 1,1)\n", (int) w.main->borrowActive(), (int) sessionHolds);
    ok = ok && w.main->borrowActive() && sessionHolds;
    w.main->borrowRelease (false); driveFor (w, 2500);
    const bool afterEnd = w.lockFile (w.A);
    std::printf ("lock: session ended -> lock file=%d (must be 0)\n", (int) afterEnd); ok = ok && ! afterEnd;
    std::printf ("lock: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("v2_readlock_legs_test.cpp");
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    const juce::String mode = argc > 1 ? argv[1] : "seed"; const int extra = argc > 2 ? atoi (argv[2]) : 40;
#ifdef EJ_LEGS_V2_G
    std::printf ("binary: V2 Build G API (read-and-lock)\n");
#else
    std::printf ("binary: Build F (control)\n");
#endif
    if (mode == "seed")  return legSeed (extra);
    if (mode == "loops") return legLoops (extra);
    if (mode == "lock")  return legLock (extra);
    return 2;
}
