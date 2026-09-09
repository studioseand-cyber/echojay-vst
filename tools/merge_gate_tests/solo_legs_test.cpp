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
#if __has_include("legs_build_d.h")
#include "legs_build_d.h"
#endif
#if __has_include("legs_build_e.h")
#include "legs_build_e.h"
#endif
#if __has_include("legs_build_f.h")
#include "legs_build_f.h"
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
    bool playing = true; juce::int64 samples = 0;   // `samples` kept for the legs that count blocks; the position itself is wall-clock on the Links' epoch
    double epochMs = 0;
    juce::Optional<PositionInfo> getPosition() const override
    { PositionInfo q; q.setIsPlaying (playing); const double s = epochMs > 0 ? (juce::Time::getMillisecondCounterHiRes() - epochMs) * 48.0 : (double) samples; q.setTimeInSamples ((juce::int64) s); q.setTimeInSeconds (s / 48000.0); return q; }
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
#if defined(EJ_LEGS_NEW_API) && !defined(EJ_LEGS_BUILD_F)
                p->pollSoloAcks();
#endif
            }
            const auto st = linkState(); std::printf ("  3 s after the press: A/B/C muteWanted = %s (want 0 1 1)\n", st.joinIntoString ("/").toRawUTF8());
            stampFile ("solo_t1.txt"); if (old) oldPathSolo (A, false);
#ifdef EJ_LEGS_NEW_API
            else p->setLinkSolo (A, false);
#endif
            for (int i = 0; i < 80; ++i) { pump (50);
#if defined(EJ_LEGS_NEW_API) && !defined(EJ_LEGS_BUILD_F)
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
#if defined(EJ_LEGS_NEW_API) && !defined(EJ_LEGS_BUILD_F)
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
        else if (mode == "additive")
        {
            // ============ ADDITIVE SOLO, RENDERED, WITH TOPOLOGY (9 Sep 2026 ruling). The Link side holds A (220 Hz) and B
            // (330 Hz) feeding a Bus Link (A+B), plus extra tracks (440 Hz) at scale. The main's INPUT is an un-Linked
            // channel at 1 kHz. Goertzel on the main's output decides what is heard. Assertions: solo A -> 220 only; solo Bus
            // -> 220+330, not 1 k; solo off -> 1 k back; NO Link muted at any point; spec 6.1: with A borrowed in-context,
            // solo A takes the processed ring (source = borrowed rack) and 220 is heard.
            juce::File (dirOf() + "solo_mode.txt").replaceWithText ("borrow");
            TestPlayHead ph; ph.epochMs = juce::File (dirOf() + "solo_epoch.txt").loadFileAsString().trim().getDoubleValue(); p->setPlayHead (&ph);
            juce::StringArray uids; uids.addLines (juce::File (dirOf() + "solo_ready.txt").loadFileAsString()); uids.removeEmptyStrings();
            const juce::String Bus = uids.size() > 2 ? uids[2].trim() : juce::String();
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; double phIn = 0;
            auto goertzel = [](const float* x, int n, double hz) { const double w = 2.0 * juce::MathConstants<double>::pi * hz / 48000.0, c = 2.0 * std::cos (w); double s0 = 0, s1 = 0, s2 = 0; for (int i = 0; i < n; ++i) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; } return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (n * 0.5); };
            double nextMs = juce::Time::getMillisecondCounterHiRes();
            struct Tones { double a = 0, b = 0, in = 0; };
            juce::AudioBuffer<float> win (1, 4096);
            auto render = [&](int blocks) -> Tones { Tones t; int n = 0; for (int k = 0; k < blocks; ++k) { while (juce::Time::getMillisecondCounterHiRes() < nextMs) pump (1); nextMs += 512000.0 / 48000.0; if (juce::Time::getMillisecondCounterHiRes() - nextMs > 200) nextMs = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < 512; ++i) { const float v = std::sin ((float) phIn) * 0.25f; phIn += 2.0 * juce::MathConstants<double>::pi * 1000.0 / 48000.0; buf.setSample (0, i, v); buf.setSample (1, i, v); }
                p->processBlock (buf, midi);
                if (k >= blocks - 8) { win.copyFrom (0, (k - (blocks - 8)) * 512, buf, 0, 0, 512); ++n; } }
                // one 4096-sample window (bins 11.7 Hz apart): a 512-sample window leaks a 0.5 tone at 220 Hz into the 330 Hz bin at ~0.07
                if (n == 8) { const float* x = win.getReadPointer (0); t.a = goertzel (x, 4096, 220); t.b = goertzel (x, 4096, 330); t.in = goertzel (x, 4096, 1000); } return t; };
            auto muted = [&]{ const auto st = linkState(); int m = 0; for (const auto& tok : st) { if (tok == "|") break; if (tok == "1") ++m; } return m; };
            auto say = [&](const char* label, const Tones& t) { std::printf ("  %-42s 220:%.3f 330:%.3f 1k:%.3f  Links muted: %d\n", label, t.a, t.b, t.in, muted()); };
            for (int i = 0; i < 30; ++i) { p->refreshLinkRegistry(); pump (100); }   // rings connect
            const auto t0 = render (60); say ("no solo (the mix = the 1 kHz un-Linked channel)", t0);
            p->setLinkSolo (A, true); const auto tA = render (80); say ("solo A", tA);
            const bool okA = tA.a > 0.15 && tA.b < 0.03 && tA.in < 0.03 && muted() == 0;
            p->setLinkSolo (Bus, true); const auto tBus = render (80); say ("solo Bus (last press wins: moved from A)", tBus);
            const bool okBus = tBus.a > 0.15 && tBus.b > 0.15 && tBus.in < 0.03 && muted() == 0;
            p->setLinkSolo (Bus, false); const auto tOff = render (80); say ("solo off", tOff);
            const bool okOff = tOff.in > 0.15 && tOff.a < 0.03 && muted() == 0;
            // spec 6.1: A borrowed in-context (a STOPPED block commits the budget), then solo A -> the processed ring is the source
            ph.playing = false; buf.clear(); p->processBlock (buf, midi); ph.playing = true; render (30);
            p->borrowEngageBegin (A, A + "-rack-add", false, true); render (140);
            p->setLinkSolo (A, true); const auto tA2 = render (80);
#ifdef EJ_LEGS_BUILD_F
            const bool srcRack = p->soloSourceIsBorrowedRack();
#else
            const bool srcRack = false;
#endif
            say ("solo A while A is the borrowed rack", tA2); std::printf ("  source is the borrowed (processed) rack: %d\n", (int) srcRack);
            const bool ok61 = tA2.a > 0.15 && tA2.in < 0.03 && srcRack && muted() <= 1;   // the borrowed rack's raw channel is session-muted by spec 6.1; nothing else may be
            p->setLinkSolo (A, false); if (p->borrowActive()) p->borrowRelease (false);
            const bool ok = okA && okBus && okOff && ok61 && t0.in > 0.15;
            std::printf ("ADDITIVE: baseline %s  solo-A %s  solo-Bus %s  off %s  spec6.1 %s -> %s\n", t0.in > 0.15 ? "PASS" : "FAIL", okA ? "PASS" : "FAIL", okBus ? "PASS" : "FAIL", okOff ? "PASS" : "FAIL", ok61 ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
            juce::File (dirOf() + "solo_done.txt").replaceWithText ("1"); result = ok ? 0 : 1;
        }
        // The subtractive-solo legs (render, select, clause3, lamp, lamp3, diehard) were retired with the broadcast on
        // 9 Sep 2026; they live in git history (commit 1a2bd93 and earlier) and in results_2026-09-06/buildE_*.
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
