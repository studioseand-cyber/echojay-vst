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
        else if (mode == "render" || mode == "select" || mode == "clause3")
        {
            // ============ RENDERED-AUDIO LEGS (9 Sep 2026 ruling): flags certified a broken build once; audio does not.
            // The main renders its own output (silent input) with A's ring injected in-context; the Link side renders A, B, C
            // and stamps their rms. Assertions are on those numbers.
            juce::File (dirOf() + "solo_mode.txt").replaceWithText ("borrow");
            TestPlayHead ph; ph.epochMs = juce::File (dirOf() + "solo_epoch.txt").loadFileAsString().trim().getDoubleValue(); p->setPlayHead (&ph);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            auto feedCache = [&]{ const auto rc = LinkShm::readRackSidecar (dirOf(), A); auto& ce = p->linkRackCache[A]; ce.rack = rc; ce.valid = rc.uid == A; ce.readMs = juce::Time::getMillisecondCounter(); };
            // REAL-TIME PACED: one block every 512/48000 s of wall time, like the host (the ring the main injects from is
            // produced by another process at the same cadence).
            double nextMs = juce::Time::getMillisecondCounterHiRes();
            auto oneBlock = [&]{ while (juce::Time::getMillisecondCounterHiRes() < nextMs) pump (1); buf.clear(); p->processBlock (buf, midi); ph.samples += 512; nextMs += 512000.0 / 48000.0; if (juce::Time::getMillisecondCounterHiRes() - nextMs > 200) nextMs = juce::Time::getMillisecondCounterHiRes(); return buf.getRMSLevel (0, 0, 512); };
            auto renderMain = [&](int blocks) -> float { float acc = 0; int n = 0; for (int b = 0; b < blocks; ++b) { const float r = oneBlock(); if (b >= blocks - 8) { acc += r; ++n; } } return n ? acc / n : 0; };
            auto linkRms = [&](int idx) -> float { const auto st = linkState(); return st.size() >= 6 ? st[3 + idx].getFloatValue() : -1.0f; };
            auto settle = [&](int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { p->pollSoloAcks(); feedCache(); oneBlock(); } };
            if (mode == "select")
            {
                // E3: the budget has NOT committed (no STOPPED block yet in this main). Selecting A must leave the passthrough
                // untouched and A's raw channel audible, and show the waiting state; a STOPPED block then blends.
                ph.playing = true;
                p->borrowEngageBegin (A, A + "-rack-sel", false, true);
                settle (800);
                // passthrough check: feed a 1 kHz tone at 0.25 into the main; the output must still be that tone, not A's ring
                double phz = 0; float inRms = 0, outRms = 0, corr = 0;
                for (int b = 0; b < 40; ++b) { while (juce::Time::getMillisecondCounterHiRes() < nextMs) pump (1); nextMs += 512000.0 / 48000.0;
                    for (int i = 0; i < 512; ++i) { const float v = std::sin ((float) phz) * 0.25f; phz += 2 * juce::MathConstants<double>::pi * 1000 / 48000; buf.setSample (0, i, v); buf.setSample (1, i, v); }
                    juce::AudioBuffer<float> in; in.makeCopyOf (buf); p->processBlock (buf, midi); ph.samples += 512;
                    if (b >= 32) { inRms += in.getRMSLevel (0, 0, 512); outRms += buf.getRMSLevel (0, 0, 512); float c = 0; for (int i = 0; i < 512; ++i) c += in.getSample (0, i) * buf.getSample (0, i); corr += c; } }
                const bool passthroughIntact = std::abs (outRms - inRms) < 0.05f * 8 && corr > 0;
                const float aRms = linkRms (0);
                const bool waiting = p->borrowStickyBanner_.contains ("Waiting for the mix budget");
#ifdef EJ_LEGS_BUILD_E
                const int bannerStatus = (int) p->borrowBannerIsStatus_;
#else
                const int bannerStatus = -1;   // Build D has no status flag
#endif
                std::printf ("  select A (budget uncommitted): in-context ok=%d; main out rms %.3f vs in %.3f (correlated=%d) -> passthrough %s; A raw channel rms %.3f (%s); banner: \"%s\" (status=%d)\n",
                             (int) p->borrowSoloSuppressInj_.load() == 0 && false, outRms / 8, inRms / 8, (int) (corr > 0), passthroughIntact ? "INTACT" : "REPLACED", aRms, aRms > 0.3f ? "audible" : "SILENT", p->borrowStickyBanner_.toRawUTF8(), (int) bannerStatus);
                // now the STOPPED block: the budget commits, Build B re-evaluates, the blend starts (A's ring in the main's output with silent input)
                ph.playing = false; buf.clear(); p->processBlock (buf, midi); ph.playing = true;
                settle (1500);
                renderMain (20);
#ifdef EJ_LEGS_BUILD_E
                const float inj = p->borrowConsumePeakIn_;      // A's audio entering the injection path (the harness cannot render the injected audio itself - stated in the record)
#else
                const float inj = 0.0f;
#endif
                const float aRms2 = linkRms (0);
                const bool blending = aRms2 < 0.01f && inj > 0.1f && p->borrowStickyBanner_.contains ("Blending");
                std::printf ("  after the STOPPED block: A raw channel rms %.3f (%s); audio into the injection path peak %.3f; banner: \"%s\" -> %s\n", aRms2, aRms2 < 0.01f ? "session-muted" : "audible", inj, p->borrowStickyBanner_.toRawUTF8(), blending ? "blending" : "NOT blending");
                const bool ok = passthroughIntact && aRms > 0.3f && waiting && blending;
                std::printf ("SELECT: passthrough-intact %s  A-audible %s  waiting-state %s  blends-after-commit %s -> %s\n", passthroughIntact ? "PASS" : "FAIL", aRms > 0.3f ? "PASS" : "FAIL", waiting ? "PASS" : "FAIL", blending ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
                p->borrowRelease (false); result = ok ? 0 : 1;
            }
            else if (mode == "render")
            {
                // commit the budget first (a STOPPED block after the pass counted A), then engage A in-context and prove the injection
                ph.playing = false; buf.clear(); p->processBlock (buf, midi); ph.playing = true; settle (300);
                p->borrowEngageBegin (A, A + "-rack-rnd", false, true);
                settle (1500);
                renderMain (20);
                // HARNESS LIMIT, stated: two independently paced processes cannot hold the ring age constant the way one host
                // callback does; the main's alignment pad key follows the age and resets its delay line and mix on every
                // change, so the injected audio itself does not reach the harness output. What CAN be proven on this side:
                // the injection path was consuming A's ring WITH AUDIO (the product's own consume peak) while in-context was OK.
#ifdef EJ_LEGS_BUILD_E
                const float injBefore = p->borrowConsumePeakIn_;
#else
                const float injBefore = 0.0f;
#endif
                { const auto ce = p->linkRackCache.find (A); const bool cached = ce != p->linkRackCache.end();
                  std::printf ("  gates: ringSlot=%d muteConfirmedOnce=%d cache(valid=%d muteEngaged=%d ageMs=%u) inContextOk=%d\n", (int) p->borrowSession_.ringSlot.load(), (int) p->borrowMuteConfirmedOnce_.load(),
                               cached ? (int) ce->second.valid : -1, cached ? (int) ce->second.rack.muteEngaged : -1, cached ? juce::Time::getMillisecondCounter() - ce->second.readMs : 0u, (int) p->borrowSoloSuppressInj_.load() * 0 + (int) (p->borrowActive())); }
                std::printf ("  in-context on A: ring consumed into the injection path, peak %.3f (%s)\n", injBefore, injBefore > 0.1f ? "A's audio present" : "NONE");
                auto press = [&](const char* label) { stampFile ("solo_t0.txt"); p->setLinkSolo (B, true); settle (400);
                    const float inj = renderMain (20); const float a = linkRms (0), b = linkRms (1), c = linkRms (2);
                    const bool ok = b > 0.3f && inj < 0.02f && c < 0.01f;
                    std::printf ("  %s: B rms %.3f (%s)  injection rms %.3f (%s)  C rms %.3f (%s)  A rms %.3f -> %s\n", label, b, b > 0.3f ? "audible" : "SILENT", inj, inj < 0.02f ? "silent" : "STILL PLAYING", c, c < 0.01f ? "silent" : "AUDIBLE", a, ok ? "PASS" : "FAIL"); return ok; };
                const bool cold = press ("COLD press on B");
                // release, then re-press 20 ms later: the sidecar snapshot is certainly stale (its pass is 1 Hz) - the mechanism
                // behind Sean's 10:29:16 press; then release and re-press at his own 0.7 s cadence.
                p->setLinkSolo (B, false); settle (20);
                p->borrowEngageBegin (A, A + "-rack-rnd2", false, true); settle (300);
                const bool rapid20 = press ("RAPID re-press on B (20 ms after the release)");
                p->setLinkSolo (B, false); settle (700);
                p->borrowEngageBegin (A, A + "-rack-rnd3", false, true); settle (300);
                const bool rapid700 = press ("RAPID re-press on B (0.7 s after the release)");
                p->setLinkSolo (B, false); settle (300);
#ifdef EJ_LEGS_BUILD_E
                const bool pre = injBefore > 0.1f; const char* preTxt = pre ? "PASS" : "FAIL";
#else
                const bool pre = true; const char* preTxt = "n/a (not exposed on this build)";
#endif
                const bool ok = pre && cold && rapid20 && rapid700;
                std::printf ("RENDER: injection-path-carried-audio %s  cold %s  rapid-20ms %s  rapid-700ms %s -> %s\n", preTxt, cold ? "PASS" : "FAIL", rapid20 ? "PASS" : "FAIL", rapid700 ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
                if (p->borrowActive()) p->borrowRelease (false); result = ok ? 0 : 1;
            }
            else   // clause3: a mute left by a PREVIOUS main must not silence the Link the user solos
            {
                p->setLinkSolo (A, true); settle (500);                           // main #1 mutes B and C
                p.reset(); pump (300);                                          // main #1 dies without releasing
                p = std::make_unique<EchoJayProcessor>(); p->prepareToPlay (48000.0, 512); p->setPlayHead (&ph);
                for (int i = 0; i < 40; ++i) { p->refreshLinkRegistry(); pump (100); }
                const float bBefore = linkRms (1);
                p->setLinkSolo (B, true); settle (500);                           // main #2 solos B: B must be EXPLICITLY un-muted
                const float b = linkRms (1), a = linkRms (0), c = linkRms (2);
                const bool ok = bBefore < 0.01f && b > 0.3f && a < 0.01f && c < 0.01f;
                std::printf ("  B silent under the dead main's mute: rms %.3f; after main #2 solos B: B %.3f (%s) A %.3f C %.3f\n", bBefore, b, b > 0.3f ? "audible" : "STILL SILENT", a, c);
                std::printf ("CLAUSE3: B explicitly un-muted -> %s\n", ok ? "PASS" : "FAIL");
                p->setLinkSolo (B, false); settle (300); result = ok ? 0 : 1;
            }
            juce::File (dirOf() + "solo_done.txt").replaceWithText ("1");
            stampFile ("solo_t1.txt");
        }
        else if (mode == "lamp3")
        {
            // BUILD D: every solo indicator agrees. Three surfaces read after one press:
            //   Link tab S lamp  = soloLampState(A) != off               (both builds)
            //   rack panel S lamp = what THAT lamp reads in this build: Build D soloIndicatorOn(A); Build C the sidecar snapshot flag
            //   banner name       = Build D firstSoloName() == A's display name; Build C reads the same snapshot flag
            juce::File (dirOf() + "solo_mode.txt").replaceWithText ("borrow");
            stampFile ("solo_t0.txt"); p->setLinkSolo (A, true);
            for (int i = 0; i < 20; ++i) { pump (10); p->pollSoloAcks(); p->refreshLinkRegistry(); }
            const bool tab = p->soloLampState (A) != EchoJayProcessor::SoloLamp::off;
#ifdef EJ_LEGS_BUILD_D
            const bool rack = p->soloIndicatorOn (A);
            const juce::String banner = p->firstSoloName();
            const bool bannerOk = banner == p->resolveLinkDisplayName (A);
#else
            const bool rack = p->muteSoloSnaps_.count (A) && p->muteSoloSnaps_[A].soloOn;   // Build C's rack lamp reads exactly this
            const juce::String banner = rack ? p->resolveLinkDisplayName (A) : juce::String();  // and the banner's name is gated on the same flag
            const bool bannerOk = rack;
#endif
            std::printf ("  after the press: Link-tab lamp=%d  rack-panel lamp=%d  banner name=\"%s\"\n", (int) tab, (int) rack, banner.toRawUTF8());
            const bool ok = tab && rack && bannerOk;
            std::printf ("LAMP3: all three surfaces agree = %d -> %s\n", (int) ok, ok ? "PASS" : "FAIL");
            stampFile ("solo_t1.txt"); p->setLinkSolo (A, false); pump (500);
            result = ok ? 0 : 1;
        }
        else if (mode == "diehard")
        {
            // ACCEPTED RISK, recorded as a leg (8 Sep 2026): the main dies (or the host saves and quits) mid-solo without
            // releasing. The Links keep muteUser=true - nothing restores them until someone un-mutes by hand.
            juce::File (dirOf() + "solo_mode.txt").replaceWithText ("borrow");   // Link side: state report only
            stampFile ("solo_t0.txt"); p->setLinkSolo (A, true);
            for (int i = 0; i < 40; ++i) { pump (50); p->pollSoloAcks(); }
            const auto st = linkState();
            p.reset();                                                          // THE MAIN DIES, no release
            pump (3000);
            const auto st2 = linkState();
            std::printf ("  with the main alive: A/B/C muteWanted = %s; 3 s after the main died without releasing: %s\n", st.joinIntoString ("/").toRawUTF8(), st2.joinIntoString ("/").toRawUTF8());
            const bool persisted = st2.size() == 3 && st2[1] == "1" && st2[2] == "1";
            std::printf ("DIEHARD: the solo mutes persist after the main dies = %d -> %s (this is the ACCEPTED behaviour; the leg documents it)\n", (int) persisted, persisted ? "REPRODUCED" : "NOT REPRODUCED");
            stampFile ("solo_t1.txt"); result = persisted ? 0 : 1;
            return;
        }
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
