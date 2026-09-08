/*  TWO-PROCESS SOLO LEGS, MAIN SIDE (8 Sep 2026 ruling). Pairs with link_capacity_test solofabric2, which
    holds Links A, B, C in the same private root and measures. Modes, each with its negative control:
      solo           the new press: setLinkSolo(A, true) -> every other Link muted on the ctrl-cmd path; release after 3 s
      oldpath        NEGATIVE CONTROL = the shipped main's behaviour byte for byte: write ctrl-cmd soloOn=1 to A and nothing
                     else (sendLinkMuteSoloCommand's file), release the same way. The Link side must FAIL the 100 ms bar.
      borrow         engage a borrow on A (in-context capable), then solo B: the borrow must be RELEASED at once (lease
                     file gone, borrowActive false) - no path through solo silences every channel
      oldpath_borrow NEGATIVE CONTROL: borrow on A, old-path soloOn to B: the borrow stays and the injection is
                     suppressed while A's channel is session-muted = the edited channel silent everywhere -> FAIL
      lamp           setLinkSolo(A): the S lamp state must be PENDING immediately, SOLID once the Links answered, never off on press
      oldpath_lamp   NEGATIVE CONTROL: after the old-path press the only lamp source (muteSoloSnaps_[A].soloOn) stays
                     false for > 500 ms: no immediate indication -> FAIL                                                       */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "PluginProcessor.h"
#include "LinkShm.h"
#if __has_include("legs_new_api.h")
#include "legs_new_api.h"
#endif
#include <cstdio>

static void pump (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); }
static juce::String dirOf() { int err = 0; return LinkShm::resolveDir (err); }
static void stampFile (const char* name) { juce::File (dirOf() + name).replaceWithText (juce::String (juce::Time::currentTimeMillis())); }
static void oldPathSolo (const juce::String& uid, bool on)   // == EchoJayEditor::sendLinkMuteSoloCommand(uid, true, on) in the shipped main
{
    auto* cmd = new juce::DynamicObject(); cmd->setProperty ("v", 1); cmd->setProperty ("seq", LinkShm::nextCtrlSeq()); cmd->setProperty ("soloOn", on);
    juce::File (dirOf() + "ctrl-ack-" + uid + ".json").deleteFile();
    juce::File (dirOf() + "ctrl-cmd-" + uid + ".json").replaceWithText (juce::JSON::toString (juce::var (cmd), true));
}
struct TestPlayHead : public juce::AudioPlayHead
{
    bool playing = true; juce::int64 samples = 0;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setIsPlaying (playing); q.setTimeInSamples (samples); q.setTimeInSeconds ((double) samples / 48000.0); return q; }
};
static juce::StringArray linkState() { juce::StringArray t; t.addTokens (juce::File (dirOf() + "solo_state.txt").loadFileAsString(), " ", ""); return t; }   // A B C muteWanted

struct Control
{
    juce::String mode; std::unique_ptr<EchoJayProcessor> p; juce::String A, B, C; int result = 1;
    bool waitReady()
    {
        std::printf ("  main link dir: '%s'\n", dirOf().toRawUTF8()); std::fflush (stdout);
        for (int i = 0; i < 1200; ++i)
        {
            juce::File f (dirOf() + "solo_ready.txt");
            if (i % 40 == 0) { std::printf ("  waiting for %s (exists=%d) t=%d s\n", f.getFullPathName().toRawUTF8(), (int) f.existsAsFile(), i / 20); std::fflush (stdout); }
            if (f.existsAsFile()) { juce::StringArray u; u.addLines (f.loadFileAsString()); if (u.size() >= 3) { A = u[0].trim(); B = u[1].trim(); C = u[2].trim(); break; } }
            pump (50);
        }
        if (A.isEmpty()) { std::printf ("no Links appeared\n"); return false; }
        // the main's registry pass must list all three (heartbeat seen to climb)
        for (int i = 0; i < 100; ++i) { p->refreshLinkRegistry(); pump (100); int n = 0; for (const auto& si : p->getLinkSlotInfos()) if (si.uid == A || si.uid == B || si.uid == C) ++n; if (n == 3) { std::printf ("  main lists A, B, C\n"); return true; } }
        std::printf ("main never listed all three\n"); return false;
    }
    void run()
    {
        p = std::make_unique<EchoJayProcessor>(); p->prepareToPlay (48000.0, 512);
        if (! waitReady()) { result = 2; return; }
        const bool old = mode.startsWith ("oldpath");
        if (mode == "solo" || mode == "oldpath")
        {
            stampFile ("solo_t0.txt"); if (old) oldPathSolo (A, true);
#ifdef EJ_LEGS_NEW_API
            else p->setLinkSolo (A, true);
#endif
            for (int i = 0; i < 60; ++i) { pump (50);
#ifdef EJ_LEGS_NEW_API
                p->pollSoloAcks();
#endif
            }
            const auto st = linkState(); std::printf ("  3 s after the press: A/B/C muteWanted = %s (want 0 1 1)\n", st.joinIntoString ("/").toRawUTF8());
            stampFile ("solo_t1.txt"); if (old) oldPathSolo (A, false);
#ifdef EJ_LEGS_NEW_API
            else p->setLinkSolo (A, false);
#endif
            for (int i = 0; i < 80; ++i) { pump (50);
#ifdef EJ_LEGS_NEW_API
                p->pollSoloAcks();
#endif
            }
            const auto st2 = linkState(); std::printf ("  4 s after the release: A/B/C muteWanted = %s (want 0 0 0)\n", st2.joinIntoString ("/").toRawUTF8());
            result = (st.size() == 3 && st[0] == "0" && st[1] == "1" && st[2] == "1" && st2.size() == 3 && st2[1] == "0" && st2[2] == "0") ? 0 : 1;
            std::printf ("MAIN %s: %s (the 100 ms verdict is the Link side's)\n", mode.toRawUTF8(), result == 0 ? "end state PASS" : "end state FAIL");
        }
        else if (mode == "borrow" || mode == "oldpath_borrow")
        {
            juce::File (dirOf() + "solo_mode.txt").replaceWithText ("borrow");   // the Link side asserts B AUDIBLE in this mode
            p->borrowEngageBegin (A, A + "-rack-" + juce::String (juce::Time::currentTimeMillis()), false, true);
            pump (600);
            const bool engaged = p->borrowActive();
            juce::File lease (dirOf() + "lease-" + A + ".json");
            std::printf ("  borrow on A engaged=%d lease file=%d\n", (int) engaged, (int) lease.existsAsFile());
            stampFile ("solo_t0.txt"); if (old) oldPathSolo (B, true);
#ifdef EJ_LEGS_NEW_API
            else p->setLinkSolo (B, true);
#endif
            const juce::int64 t0 = juce::Time::currentTimeMillis(); double tRel = -1;
            for (int i = 0; i < 60; ++i) { pump (50);
#ifdef EJ_LEGS_NEW_API
                p->pollSoloAcks();
#endif
                if (tRel < 0 && ! p->borrowActive()) tRel = (double) (juce::Time::currentTimeMillis() - t0); }
            const bool suppressed = p->borrowSoloSuppressInj_.load();
            const auto st = linkState();
            std::printf ("  3 s after solo B: borrowActive=%d (released after %.0f ms) lease=%d injectionSuppressed=%d A/B/C muteWanted=%s\n", (int) p->borrowActive(), tRel, (int) lease.existsAsFile(), (int) suppressed, st.joinIntoString ("/").toRawUTF8());
            // THE INVARIANT: no path through solo silences every channel. Observable: NOT (edited channel session-muted AND injection suppressed).
            const bool bAudible = st.size() == 3 && st[1] == "0";
            const bool editedSilentEverywhere = p->borrowActive() && suppressed;
            result = (engaged && bAudible && ! editedSilentEverywhere) ? 0 : 1;
            std::printf ("BORROW %s: B audible=%d editedSilentEverywhere=%d -> %s\n", mode.toRawUTF8(), (int) bAudible, (int) editedSilentEverywhere, result == 0 ? "PASS" : "FAIL");
            stampFile ("solo_t1.txt"); if (old) oldPathSolo (B, false);
#ifdef EJ_LEGS_NEW_API
            else p->setLinkSolo (B, false);
#endif
            pump (1500); if (p->borrowActive()) p->borrowRelease (false);
        }
        else if (mode == "buildb2")
        {
            // BUILD B, the live race modelled with a REAL Link: the main prepared (budget stored OFF) before any Link existed;
            // the Link's sidecar then makes WANTED ON, but nothing commits until a STOPPED block. Engage now -> refused on the
            // budget (12:46:51 live). A STOPPED block commits ON -> Build B re-evaluates the live session -> lease muteOut Y.
            juce::File (dirOf() + "solo_mode.txt").replaceWithText ("borrow");   // Link side: nobody soloed, B audible -> harmless PASS
            TestPlayHead ph; p->setPlayHead (&ph);
            p->borrowEngageBegin (A, A + "-rack-bb", false, true);
            pump (500);
            auto leaseMuteOut = [&]{ const auto v = juce::JSON::parse (juce::File (LinkShm::leasePath (dirOf(), A))); return v.isObject() && (bool) v.getProperty ("muteOut", false); };
            const bool before = leaseMuteOut(); const bool okBefore = p->borrowActive();
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; ph.playing = false; p->processBlock (buf, midi);   // the STOPPED block: the budget commits ON
            double tOk = -1; const juce::int64 t0 = juce::Time::currentTimeMillis();
            for (int i = 0; i < 300 && tOk < 0; ++i) { pump (20); if (leaseMuteOut()) tOk = (double) (juce::Time::currentTimeMillis() - t0); }
            std::printf ("  engaged=%d; lease muteOut before the STOPPED-block commit: %d; after: %s (%.0f ms)\n", (int) okBefore, (int) before, tOk >= 0 ? "Y" : "N", tOk);
            const bool ok = okBefore && ! before && tOk >= 0;
            std::printf ("BUILDB2: refused-on-budget then restored by the commit -> %s\n", ok ? "PASS" : "FAIL");
            stampFile ("solo_t0.txt"); pump (300); stampFile ("solo_t1.txt");
            p->borrowRelease (false); result = ok ? 0 : 1;
        }
#ifdef EJ_LEGS_NEW_API
        else if (mode == "lamp" || mode == "oldpath_lamp")
        {
            stampFile ("solo_t0.txt");
            EchoJayProcessor::SoloLamp atPress;
            if (old) { oldPathSolo (A, true); atPress = EchoJayProcessor::SoloLamp::off; }
            else { p->setLinkSolo (A, true); atPress = p->soloLampState (A); }
            const bool immediate = old ? (p->muteSoloSnaps_.count (A) && p->muteSoloSnaps_[A].soloOn) : atPress != EchoJayProcessor::SoloLamp::off;
            double tSolid = -1; const juce::int64 t0 = juce::Time::currentTimeMillis();
            for (int i = 0; i < 100; ++i) { pump (10); p->pollSoloAcks(); p->refreshLinkRegistry(); if (old ? (p->muteSoloSnaps_.count (A) && p->muteSoloSnaps_[A].soloOn) : p->soloLampState (A) == EchoJayProcessor::SoloLamp::solid) { tSolid = (double) (juce::Time::currentTimeMillis() - t0); break; } }
            std::printf ("  at press: %s; solid after %.0f ms\n", immediate ? (atPress == EchoJayProcessor::SoloLamp::pending ? "PENDING" : "lit") : "NOTHING", tSolid);
            stampFile ("solo_t1.txt"); if (old) oldPathSolo (A, false);
#ifdef EJ_LEGS_NEW_API
            else p->setLinkSolo (A, false);
#endif
            pump (1500);
            result = (immediate && tSolid >= 0 && tSolid <= 1000) ? 0 : 1;
            std::printf ("LAMP %s: immediate=%d solid=%.0f ms -> %s\n", mode.toRawUTF8(), (int) immediate, tSolid, result == 0 ? "PASS" : "FAIL");
        }
#endif
        for (int i = 0; i < 100 && ! juce::File (dirOf() + "solo_done.txt").existsAsFile(); ++i) pump (100);
    }
};

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("solo_legs_test.cpp");
    juce::ScopedJuceInitialiser_GUI init;
    Control c; c.mode = argc > 1 ? argv[1] : "solo";
    bool done = false; int rc = 1;
    juce::MessageManager::callAsync ([&] { c.run(); rc = c.result; c.p.reset(); done = true; });
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 240000;
    while (! done && juce::Time::getMillisecondCounterHiRes() < deadline) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
    if (! done) { std::printf ("WATCHDOG\n"); return 3; }
    return rc;
}
