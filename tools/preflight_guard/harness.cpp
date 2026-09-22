// preflight_guard (17 Sep 2026 ruling, the AVOX SYBIL hang) on the REAL ChainHost.
// FIXTURES (flagged): helper STAND-INS through the preflightCommand seam - /bin/sleep 30 (an initialiser that never
// returns) and /usr/bin/false (exits non-zero) - not AU bundles (a real .component fixture would need an installed, signed
// component in the isolated HOME's Components folder; the seam runs the same ChildProcess + 10 s poll the shipped
// EchoJayProbe helper goes through).
//   (a) sleeps 30 s -> the probe TIMES OUT at 10 s (killed), the identity is marked "hangs-on-load" in the
//       disabled-set note, the build SUBSTITUTES the built-in of its role (row/card say why), and the message thread
//       was never blocked (the longest gap between 20 ms timer ticks during the wait stays under 250 ms).
//       RED today: no pre-flight - the in-host create would run the hang on the message thread.
//   (b) exits non-zero -> NOT marked, the in-host create is attempted (the fake description reaches the format
//       manager and fails as a create, not as a substitution).
//   (c) a known-good identity -> no probe spawned.
//   (d) the helper's location: <bundle>/Contents/MacOS/EchoJayProbe beside the plugin binary.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EJDisableReasons.h"
#include "PluginCatalog.h"
#include "EedCompressorProcessor.h"   // force-link the built-in's registrar (static-lib dead stripping)
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <cstdlib>
#ifdef __has_include
#if __has_include("EJPaceCheck.h")
#include "EJPaceCheck.h"
#endif
#endif
// (g) 18 Sep 2026 - the pre-flight LIVENESS leg runs the REAL build entry (EchoJayEditor::sendChainToLink) on a
// real editor with a real engaged borrowed host; the three names resolve through the primary host's recommendable
// list (the friend below puts them there, as buildRecommendable would from a scan).
struct EchoJayTabStripTestAccess { static void build (EchoJayEditor& e, const juce::String& uid, const juce::String& json) { e.sendChainToLink (uid, json); }
                                   static juce::String status (EchoJayEditor& e) { return e.chainListPanel.statusText; } };
// The build's tab switch rebuilds recommendable_ from the (empty) scan, so the names must ALSO be in the known-plugin
// list, which descriptionsForNames falls back to through resolveByName - the same list a real scan fills.
struct EchoJayBorrowHostTestAccess { static void addRecommendable (ChainHost& h, const juce::PluginDescription& d)
{   // seed the SAME stores a real scan fills: the host's scanned entries (buildRecommendable's source) + its known list
    { std::lock_guard<std::mutex> lk (h.pluginsMutex_); h.entries_.add (d); }
    h.knownPlugins_.addType (d); h.recommendable_.push_back ({ d.name, d });
} };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::PluginDescription fake (int uid, const juce::String& name) { juce::PluginDescription d; d.name = name; d.pluginFormatName = "AudioUnit"; d.manufacturerName = "Guard"; d.fileOrIdentifier = "AudioUnit:Effects/aufx,gd" + juce::String (uid) + ",Grd_"; d.uniqueId = 0x5E6D000 + uid; d.version = "1.0"; return d; }
// pump the message thread and record the longest stall between ticks
double pumpMeasuring (double totalMs, double tickMs = 20.0)
{
    double worst = 0; double last = juce::Time::getMillisecondCounterHiRes(); const double t0 = last;
    while (juce::Time::getMillisecondCounterHiRes() - t0 < totalMs)
    {
        juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, tickMs / 1000.0, false);
        const double now = juce::Time::getMillisecondCounterHiRes(); worst = juce::jmax (worst, now - last - tickMs); last = now;
    }
    return worst;
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedCompressorProcessor::schema();
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_preflight_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    echojay::userAppData().getChildFile ("EchoJay").createDirectory();
    std::printf ("preflight_guard: a plugin that hangs on load is caught out of process, marked, substituted; the message thread never blocks\n");
#ifdef EJ_GUARD_TODAY
    check (false, "(a) the probe TIMES OUT at 10 s and the build substitutes", "TODAY: ChainHost has no pre-flight - the in-host create would run SYBIL's initialiser on the message thread (the 20:50 hang)");
#else
    // Stand-in helpers (a shell script cannot be spawned under this harness's sandbox - exec fails 255 - so the
    // two shapes are real binaries): a helper whose "initialiser" never returns (/bin/sleep 30) and one that exits
    // non-zero without hanging (/usr/bin/false).
    ChainHost::resetPreflightVerdictsForTest();
    // 18 Sep 2026: a timeout is "slow", not a verdict - a hang is TWO consecutive timeouts (first bound, then the retry
    // bound). The retry bound is shrunk here (3 s) so leg (a) stays a 13 s wait instead of 40.
    ChainHost::setPreflightBoundsForTest (ChainHost::kPreflightTimeoutMs, 3000);
    ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512);
    const auto dHang = fake (1, "Guard Hanger"), dErr = fake (2, "Guard Failer"), dGood = fake (3, "Guard Good");
    h.preflightCommand = [&] (const juce::PluginDescription& d) -> juce::StringArray
    { return d.uniqueId == dHang.uniqueId ? juce::StringArray { "/bin/sleep", "30" } : juce::StringArray { "/usr/bin/false" }; };
    h.setBuildRoles ({ { "guard hanger", "compressor" }, { "guard failer", "compressor" } });
    ChainHost::markKnownGood (dGood);   // (c): known-good before the round
    check (ChainHost::isKnownGood (dGood) && ! ChainHost::isKnownGood (dHang), "(c) precondition: known_good.json holds the good product only", ChainHost::knownGoodFile().getFullPathName());
    bool done = false; const double t0 = juce::Time::getMillisecondCounterHiRes();
    h.preflightPlugins ({ dHang, dErr, dGood }, [&] { done = true; });
    check (h.preflightSpawnCount() == 2, "(c) two probes spawned (the known-good product needed none), in parallel", juce::String (h.preflightSpawnCount()));
    double worst = 0; while (! done && juce::Time::getMillisecondCounterHiRes() - t0 < 18000) worst = juce::jmax (worst, pumpMeasuring (200));
    const double waited = juce::Time::getMillisecondCounterHiRes() - t0;
    check (done, "the pre-flight settled", juce::String ((int) waited) + " ms");
    const auto vh = ChainHost::preflightVerdictFor (dHang), ve = ChainHost::preflightVerdictFor (dErr), vg = ChainHost::preflightVerdictFor (dGood);
    check (vh.state == ChainHost::PreflightState::hang && vh.attempts == 2 && waited >= 12900 && waited < 16500, "(a) the sleeping fixture timed out at 10 s, was RETRIED (3 s test bound) and timed out again -> hang (two consecutive timeouts)", "state=" + juce::String ((int) vh.state) + " after " + juce::String ((int) waited) + " ms: " + vh.note);
    check (echojay::disableReasonFor (echojay::makeUid (dHang.name, dHang.manufacturerName)) == "hangs-on-load", "(a) the identity is marked hangs-on-load in the disabled-set note (scanner key name_manufacturer)", echojay::disableReasonDetailFor (echojay::makeUid (dHang.name, dHang.manufacturerName)));
    check (worst < 250.0, "(a) the message thread was never blocked during the wait (worst tick gap < 250 ms)", juce::String (worst, 1) + " ms");
    juce::String errH; bool cbH = false;
    h.loadPluginAsync (dHang, ChainHost::LoadOrigin::Assistant, [&] (const juce::String& err) { errH = err; cbH = true; });
    pumpMeasuring (100);
    check (cbH && errH.isEmpty() && h.getNumSlots() == 1 && h.getSlotInfo (0).name == "EchoJay Compressor", "(a) the build SUBSTITUTED EchoJay Compressor for the hanging plugin (never created in-host)", h.getNumSlots() ? h.getSlotInfo (0).name : juce::String ("no slot"));
    check (h.dialSummaryRow (0).contains ("SUBSTITUTED for \"Guard Hanger\" (hangs on load)") && h.getSlotInfo (0).settings.contains ("hangs on load"), "(a) the row and the card say why", h.getSlotInfo (0).settings.upToFirstOccurrenceOf ("\n", false, false));
    check (ve.state == ChainHost::PreflightState::error && echojay::disableReasonFor (echojay::makeUid (dErr.name, dErr.manufacturerName)).isEmpty(), "(b) a non-zero exit is NOT evidence: verdict error, identity NOT marked", ve.note);
    juce::String errE; bool cbE = false;
    h.loadPluginAsync (dErr, ChainHost::LoadOrigin::Assistant, [&] (const juce::String& err) { errE = err; cbE = true; });
    { const double t1 = juce::Time::getMillisecondCounterHiRes(); while (! cbE && juce::Time::getMillisecondCounterHiRes() - t1 < 5000) pumpMeasuring (50); }
    check (cbE && errE.isNotEmpty() && ! errE.contains ("hangs on load") && h.getNumSlots() == 1, "(b) the in-host create was ATTEMPTED (a real create error on the fake identity, not a substitution)", errE.substring (0, 80));
    check (vg.state == ChainHost::PreflightState::unknown, "(c) the known-good product has no probe verdict (none was run)");
    check (ChainHost::probeHelperFile().getFileName() == "EchoJayProbe" && ChainHost::probeHelperFile().getParentDirectory().getFileName() == juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getFileName(), "(d) the helper is looked for as EchoJayProbe beside the plugin binary (Contents/MacOS)", ChainHost::probeHelperFile().getFullPathName());
#endif
    std::printf ("== (e)(f) 18 Sep 2026: a hangs-on-load mark expires after 7 days; a live mark is never probed nor loaded ==\n");
#ifndef EJ_GUARD_TODAY
    {
        ChainHost::resetPreflightVerdictsForTest();
        ChainHost h2 (ChainHost::Mode::Primary); h2.prepare (48000.0, 512); h2.setBuildRoles ({ { "guard fresh", "compressor" }, { "guard stale", "compressor" } });
        int spawns = 0;
        h2.preflightCommand = [&spawns] (const juce::PluginDescription&) -> juce::StringArray { ++spawns; return { "/bin/sleep", "30" }; };
        const auto dFresh = fake (11, "Guard Fresh"), dStale = fake (12, "Guard Stale");
        const auto uFresh = echojay::makeUid (dFresh.name, dFresh.manufacturerName), uStale = echojay::makeUid (dStale.name, dStale.manufacturerName);
        echojay::recordDisableReasons ({ uFresh }, echojay::kDisableWhyHangsOnLoad, "hangs on load (test)");   // marked NOW: until = +7 days
        echojay::recordDisableReasons ({ uStale }, echojay::kDisableWhyHangsOnLoad, "hangs on load (test)");
        {   // age the stale mark: untilMs in the past
            auto all = echojay::readDisableReasons(); auto e = all.getDynamicObject()->getProperty (juce::Identifier (uStale));
            e.getDynamicObject()->setProperty ("untilMs", (double) (juce::Time::currentTimeMillis() - 1000)); e.getDynamicObject()->setProperty ("until", "2026-09-10");
            echojay::disableReasonsFile().replaceWithText (juce::JSON::toString (all, true));
        }
        check (echojay::hangsOnLoadMarkLive (uFresh) && ! echojay::hangsOnLoadMarkExpired (uFresh), "(e) a fresh mark is LIVE (until = at + 7 days)", echojay::disableReasonDetailFor (uFresh));
        check (echojay::hangsOnLoadMarkExpired (uStale), "(f) a mark whose expiry passed is EXPIRED");
        bool done2 = false; h2.preflightPlugins ({ dFresh, dStale }, [&] { done2 = true; });
        check (spawns == 1, "(e) the LIVE mark spawned NO probe; (f) the EXPIRED one was probed again (one spawn)", juce::String (spawns));
        check (echojay::disableReasonFor (uStale).isEmpty(), "(f) the expired mark was CLEARED before the re-probe");
        check (ChainHost::preflightVerdictFor (dFresh).state == ChainHost::PreflightState::hang, "(e) the live mark IS the verdict (hang) without a probe");
        juce::String errF; bool cbF = false;
        h2.loadPluginAsync (dFresh, ChainHost::LoadOrigin::Assistant, [&] (const juce::String& err) { errF = err; cbF = true; }); pumpMeasuring (100);
        check (cbF && h2.getNumSlots() == 1 && h2.getSlotInfo (0).name == "EchoJay Compressor", "(e) the live-marked plugin is never loaded in-host: substituted", h2.getNumSlots() ? h2.getSlotInfo (0).name : juce::String ("none"));
        const double t2 = juce::Time::getMillisecondCounterHiRes(); while (! done2 && juce::Time::getMillisecondCounterHiRes() - t2 < 18000) pumpMeasuring (200);
        check (done2 && ChainHost::preflightVerdictFor (dStale).state == ChainHost::PreflightState::hang && echojay::hangsOnLoadMarkLive (uStale), "(f) the re-probe timed out again -> a NEW mark with a new 7-day expiry", echojay::disableReasonDetailFor (uStale));
    }
#else
    std::printf ("  (skipped in TODAY mode)\n"); ++failures;
#endif
    std::printf ("== (h)(i)(j)(k) 18 Sep 2026: host arch, retry, two-timeouts, instantiated-marker (AMEK Mastering Compressor) ==\n");
#ifndef EJ_GUARD_TODAY
    {
        // (h) ARCH: the probe runs under the HOST's architecture. Pro Tools under Rosetta is x86_64: the command is
        // spawned through /usr/bin/arch -x86_64; a native host runs the helper directly. Both end with the marker path.
        const auto dA = fake (41, "Guard Arch");
        const auto cx = ChainHost::defaultPreflightCommand (dA, "x86_64"), ca = ChainHost::defaultPreflightCommand (dA, "arm64");
        check (cx.size() == 7 && cx[0] == "/usr/bin/arch" && cx[1] == "-x86_64" && cx[2] == ChainHost::probeHelperFile().getFullPathName(),
               "(h) an x86_64 (Rosetta) host spawns the probe through /usr/bin/arch -x86_64", cx.joinIntoString (" "));
        check (ca.size() == 5 && ca[0] == ChainHost::probeHelperFile().getFullPathName(), "(h) a native arm64 host spawns the helper directly", ca.joinIntoString (" "));
        check (cx[cx.size() - 1] == ChainHost::preflightMarkerFile (dA).getFullPathName() && ca[ca.size() - 1] == cx[cx.size() - 1],
               "(h) both carry the INSTANTIATE marker path as the last argument", cx[cx.size() - 1]);
        check (ChainHost::defaultPreflightCommand (dA)[0] == (ChainHost::hostArchName() == "x86_64" ? juce::String ("/usr/bin/arch") : ChainHost::probeHelperFile().getFullPathName()),
               "(h) the default picks this process's architecture (" + ChainHost::hostArchName() + ")");
    }
    {
        ChainHost::resetPreflightVerdictsForTest();
        ChainHost::setPreflightBoundsForTest (1500, 2000);   // short bounds: the legs prove the rule, not the seconds
        ChainHost h3 (ChainHost::Mode::Primary); h3.prepare (48000.0, 512); h3.setBuildRoles ({ { "guard slow", "compressor" }, { "guard twice", "compressor" }, { "guard loaded", "compressor" } });
        const auto dSlow = fake (42, "Guard Slow"), dTwice = fake (43, "Guard Twice"), dLoaded = fake (44, "Guard Loaded");
        std::map<int, int> attempts;
        h3.preflightCommand = [&attempts, dSlow, dTwice] (const juce::PluginDescription& d) -> juce::StringArray
        {
            const int a = ++attempts[d.uniqueId];
            if (d.uniqueId == dSlow.uniqueId) return a == 1 ? juce::StringArray { "/bin/sleep", "30" } : juce::StringArray { "/usr/bin/true" };   // slow once, then fine
            return { "/bin/sleep", "30" };                                                                                                    // never returns
        };
        bool done3 = false; const double t3 = juce::Time::getMillisecondCounterHiRes();
        h3.preflightPlugins ({ dSlow, dTwice, dLoaded }, [&] { done3 = true; });
        // (k) the LOADED case: the probe touched the marker (INSTANTIATE: OK) right after spawning, then stalled in its
        // render check. (The spawn clears a stale marker first, so the touch comes after the spawn - as the real probe's does.)
        ChainHost::preflightMarkerFile (dLoaded).create();
        while (! done3 && juce::Time::getMillisecondCounterHiRes() - t3 < 9000) pumpMeasuring (100);
        const auto vS = ChainHost::preflightVerdictFor (dSlow), vT = ChainHost::preflightVerdictFor (dTwice), vL = ChainHost::preflightVerdictFor (dLoaded);
        check (done3, "(i)(j)(k) the pre-flight settled", juce::String ((int) (juce::Time::getMillisecondCounterHiRes() - t3)) + " ms");
        check (attempts[dSlow.uniqueId] == 2 && vS.state == ChainHost::PreflightState::ok && vS.attempts == 2,
               "(i) RETRY: first timeout -> retried once -> success -> verdict ok", "attempts=" + juce::String (attempts[dSlow.uniqueId]) + " state=" + juce::String ((int) vS.state) + " " + vS.note);
        check (echojay::disableReasonFor (echojay::makeUid (dSlow.name, dSlow.manufacturerName)).isEmpty(), "(i) ...and it was NOT marked hangs-on-load");
        check (attempts[dTwice.uniqueId] == 2 && vT.state == ChainHost::PreflightState::hang && vT.attempts == 2,
               "(j) TWO consecutive timeouts -> hang", "attempts=" + juce::String (attempts[dTwice.uniqueId]) + " " + vT.note);
        check (echojay::disableReasonFor (echojay::makeUid (dTwice.name, dTwice.manufacturerName)) == "hangs-on-load", "(j) ...and only then is it marked hangs-on-load");
        check (attempts[dLoaded.uniqueId] == 1 && vL.state == ChainHost::PreflightState::ok && vL.instantiated,
               "(k) INSTANTIATED then stalled in the render check -> ok, no retry, never a hang (the AMEK case)", vL.note);
        check (echojay::disableReasonFor (echojay::makeUid (dLoaded.name, dLoaded.manufacturerName)).isEmpty(), "(k) ...and NOT marked");
        check (! ChainHost::preflightMarkerFile (dLoaded).existsAsFile(), "(k) the marker is cleaned up after the verdict");
        // (l) 18 Sep 2026: a probe that CRASHES after instantiate (the AMEK shape: marker touched, then SIGSEGV) is ok, never marked
        {
            ChainHost::resetPreflightVerdictsForTest();
            ChainHost h4 (ChainHost::Mode::Primary); h4.prepare (48000.0, 512); h4.setBuildRoles ({ { "guard crasher", "compressor" } });
            const auto dCrash = fake (45, "Guard Crasher");
            const auto marker = ChainHost::preflightMarkerFile (dCrash).getFullPathName();
            h4.preflightCommand = [marker] (const juce::PluginDescription&) -> juce::StringArray
            { return { "/bin/sh", "-c", "/usr/bin/touch '" + marker + "'; kill -SEGV $$" }; };   // instantiate ok, then die
            bool done4 = false; const double t4 = juce::Time::getMillisecondCounterHiRes();
            h4.preflightPlugins ({ dCrash }, [&] { done4 = true; });
            while (! done4 && juce::Time::getMillisecondCounterHiRes() - t4 < 6000) pumpMeasuring (100);
            const auto vC = ChainHost::preflightVerdictFor (dCrash);
            // juce::ChildProcess::getExitCode reports a signal death as 0, so the exit code is no evidence here; the
            // marker (instantiated) and the verdict are.
            check (done4 && vC.state == ChainHost::PreflightState::ok && vC.instantiated,
                   "(l) a probe that crashes AFTER instantiate -> ok (instantiated; a render/exit crash is not a load hang)", "state=" + juce::String ((int) vC.state) + " instantiated=" + (vC.instantiated ? "y" : "n") + " exit=" + juce::String (vC.exitCode) + " " + vC.note);
            check (echojay::disableReasonFor (echojay::makeUid (dCrash.name, dCrash.manufacturerName)).isEmpty(), "(l) ...and NOT marked hangs-on-load");
        }
        ChainHost::setPreflightBoundsForTest (ChainHost::kPreflightTimeoutMs, 3000);
    }
#else
    {   // RED on the old single-shot, with the OLD API only: a fake SLOW probe (times out once, fine the second time) must not be marked
        ChainHost::resetPreflightVerdictsForTest();
        ChainHost hOld (ChainHost::Mode::Primary); hOld.prepare (48000.0, 512); hOld.setBuildRoles ({ { "guard slow", "compressor" } });
        const auto dSlow = fake (42, "Guard Slow"); int calls = 0;
        hOld.preflightCommand = [&calls] (const juce::PluginDescription&) -> juce::StringArray { return ++calls == 1 ? juce::StringArray { "/bin/sleep", "30" } : juce::StringArray { "/usr/bin/true" }; };
        bool doneOld = false; const double tOld = juce::Time::getMillisecondCounterHiRes();
        hOld.preflightPlugins ({ dSlow }, [&] { doneOld = true; });
        while (! doneOld && juce::Time::getMillisecondCounterHiRes() - tOld < 45000) pumpMeasuring (100);
        check (calls == 2, "(i) a slow probe is RETRIED once (the seam was asked twice)", "calls=" + juce::String (calls));
        check (echojay::disableReasonFor (echojay::makeUid (dSlow.name, dSlow.manufacturerName)).isEmpty() && ChainHost::preflightVerdictFor (dSlow).state == ChainHost::PreflightState::ok,
               "(i) ...and after the retry succeeds it is NOT marked hangs-on-load", "state=" + juce::String ((int) ChainHost::preflightVerdictFor (dSlow).state) + " " + ChainHost::preflightVerdictFor (dSlow).note);
        check (false, "(h) an x86_64 (Rosetta) host spawns the probe through /usr/bin/arch -x86_64", "TODAY: no arch, no marker");
    }
#endif
    std::printf ("== (g) 18 Sep 2026 LIVENESS: a build whose chain names THREE not-known-good products spawns THREE probes ==\n");
    // Pro Tools, 18 Sep 13:39: spiff, UAD dbx 160 and Looptrotter SA2RATE2 were not known-good, the build placed all
    // three in-host and no EJPreflight line was ever written - the names for the pre-flight were read out of a freed
    // array (PluginEditor.cpp:28687, the same expression that crashed in roleByNameFor). Under the scribble leg that
    // read is deterministic garbage; this leg asserts the probes actually spawn, one per name, on the real path.
    {
        ChainHost::resetPreflightVerdictsForTest();
        auto af = echojay::userAppData().getChildFile ("EchoJay/auth.json"); af.getParentDirectory().createDirectory();
        af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\",\"email\":\"ui@test.local\",\"tier\":\"pro\",\"tierLevel\":2,\"messageLimit\":999,\"credits\":999}");
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
        check (ed != nullptr, "(g) precondition: a real editor");
        if (ed != nullptr)
        {
            ed->setSize (2000, 1100); pumpMeasuring (60);
            const juce::String uid = "uid-LINK-G";
            proc.borrowEngageBegin (uid, "lease-" + uid, true, true);
            auto* bh = proc.borrowHostIfActiveFor (uid);
            check (bh != nullptr, "(g) precondition: the Link rack is engaged (borrowed host live)");
            if (bh != nullptr)
            {
                const auto d1 = fake (31, "Guard Probe One"), d2 = fake (32, "Guard Probe Two"), d3 = fake (33, "Guard Probe Three");
                for (const auto& d : { d1, d2, d3 }) EchoJayBorrowHostTestAccess::addRecommendable (proc.getChainHost(), d);
                check (proc.getChainHost().descriptionsForNames ({ "Guard Probe One", "Guard Probe Two", "Guard Probe Three" }).size() == 3
                       && ! ChainHost::isKnownGood (d1) && ! ChainHost::isKnownGood (d2) && ! ChainHost::isKnownGood (d3),
                       "(g) precondition: the three names resolve and none is known-good");
                juce::StringArray probed;
                bh->preflightCommand = [&probed] (const juce::PluginDescription& d) -> juce::StringArray { probed.add (d.name); return { "/bin/sleep", "30" }; };
                const juce::String json = "{\"chain\":[{\"name\":\"Guard Probe One\",\"role\":\"compressor\"},{\"name\":\"Guard Probe Two\",\"role\":\"eq\"},{\"name\":\"Guard Probe Three\",\"role\":\"saturation\"}]}";
                EchoJayTabStripTestAccess::build (*ed, uid, json);
                pumpMeasuring (50);
                {   // diagnostics for the FAIL line: where did the descriptions go?
                    auto descs = proc.getChainHost().descriptionsForNames ({ "Guard Probe One", "Guard Probe Two", "Guard Probe Three" });
                    juce::String diag = "descs=" + juce::String ((int) descs.size()) + " sameHost=" + juce::String (proc.borrowHostIfActiveFor (uid) == bh ? "y" : "n")
                        + " primarySpawns=" + juce::String (proc.getChainHost().preflightSpawnCount()) + " borrowSpawns=" + juce::String (bh->preflightSpawnCount());
                    for (const auto& d : descs) diag += " [" + d.name + " uid=" + juce::String (d.uniqueId) + " builtin=" + (ChainHost::isBuiltinDescription (d) ? "y" : "n") + " knownGood=" + (ChainHost::isKnownGood (d) ? "y" : "n")
                                                   + " verdict=" + juce::String ((int) ChainHost::preflightVerdictFor (d).state) + "]";
                    std::printf ("  diag  %s\n", diag.toRawUTF8());
                }
                check (bh->preflightSpawnCount() == 3, "(g) THREE probes spawned by the real build entry (one per not-known-good name)", "spawned=" + juce::String (bh->preflightSpawnCount()));
                check (probed.size() == 3 && probed.contains ("Guard Probe One") && probed.contains ("Guard Probe Two") && probed.contains ("Guard Probe Three"),
                       "(g) each name was probed exactly once (EJPreflight: probing ... per name)", probed.joinIntoString (", "));
                check (EchoJayTabStripTestAccess::status (*ed).startsWith (juce::String::fromUTF8 ("Checking plugins")), "(g) the overlay says \"Checking plugins...\" while the probes run", EchoJayTabStripTestAccess::status (*ed));
                bh->preflightCommand = nullptr;
            }
            proc.borrowRelease (false);
        }
    }

    std::printf ("== (l) 21 Sep 2026: the probe's --list-params mode; an unsigned harness refuses a PACE-wrapped bundle ==\n");
    {
        const juce::File probe (std::getenv ("EJ_PROBE_BIN") != nullptr ? juce::String (std::getenv ("EJ_PROBE_BIN")) : juce::File::getCurrentWorkingDirectory().getChildFile ("build-release/EchoJayProbe_artefacts/Release/EchoJayProbe").getFullPathName());
        // the probe gets a state root of its OWN under the isolated root, seeded with the three files it must not touch: the guard's earlier
        // legs keep an EchoJayProcessor whose pre-flight writes plugin_disabled*.json / known_good.json on its own clock (the scribble leg
        // is slow enough for such a write to land inside the probe window - a sibling object's write, not the probe's; seen 21 Sep 2026)
        const juce::File root = juce::File (std::getenv ("ECHOJAY_STATE_HOME")).getChildFile ("probe_list_root"); const juce::File stateDir = root.getChildFile ("Library/EchoJay");
        stateDir.createDirectory();
        for (const char* seed : { "plugin_disabled_reasons.json", "plugin_disabled.json", "known_good.json", "preflight_marker" }) stateDir.getChildFile (seed).replaceWithText ("{}");
        // the files a harness must never touch: everything under Library/EchoJay of the isolated root (the pre-flight marker, plugin_disabled*.json, known_good.json ...)
        auto stateSnapshot = [&] { juce::StringArray o; for (const auto& f : stateDir.findChildFiles (juce::File::findFiles, true)) o.add (f.getRelativePathFrom (root) + "@" + juce::String (f.getLastModificationTime().toMilliseconds())); o.sort (false); return o; };
        auto runProbe = [&] (const juce::StringArray& args, const juce::StringPairArray& env, juce::String& out, int timeoutMs) -> int
        {
            // the exit status is OBSERVED through a shell echo ("__RC=n" as the last line): juce::ChildProcess::getExitCode reads 0 once
            // isRunning() has reaped the child (21 Sep 2026: the forced-timeout leg saw "refused timeout" with rc 0 that way)
            juce::StringArray cmd; cmd.add ("/bin/sh"); cmd.add ("-c"); cmd.add ("\"$0\" \"$@\"; echo \"__RC=$?\""); cmd.add ("/usr/bin/env"); for (const auto& k : env.getAllKeys()) cmd.add (k + "=" + env[k]); cmd.add (probe.getFullPathName()); cmd.addArray (args);
            juce::ChildProcess cp; if (! cp.start (cmd)) return -1;
            const double t0 = juce::Time::getMillisecondCounterHiRes(); while (cp.isRunning() && juce::Time::getMillisecondCounterHiRes() - t0 < timeoutMs) juce::Thread::sleep (20);
            if (cp.isRunning()) { cp.kill(); return -2; }
            out = cp.readAllProcessOutput(); const int at = out.lastIndexOf ("__RC="); if (at < 0) return -3;
            const int rc = out.substring (at + 5).trim().getIntValue(); out = out.substring (0, at); return rc;
        };
        const juce::StringArray delay { "AUDelay", "AudioUnit:Effects/aufx,dely,appl", "64607a6d", "--list-params" };   // Apple's AUDelay: on every Mac, not PACE-wrapped
        juce::StringPairArray env; env.set ("ECHOJAY_STATE_HOME", root.getFullPathName()); env.set ("HOME", root.getFullPathName());
        juce::File::getCurrentWorkingDirectory().getChildFile ("--list-params").deleteFile();   // a stray marker from an earlier (RED) run must not decide this run
        const auto before = stateSnapshot(); const auto cwdBefore = juce::File::getCurrentWorkingDirectory().findChildFiles (juce::File::findFiles, false).size();
        juce::String out; const int rc = runProbe (delay, env, out, 40000);
        int rows = 0; for (const auto& l : juce::StringArray::fromLines (out)) if (l.matchesWildcard ("*\t*\t*\t*\t*", true) && l.upToFirstOccurrenceOf ("\t", false, false).containsOnly ("0123456789")) ++rows;
        const auto after = stateSnapshot(); const auto cwdAfter = juce::File::getCurrentWorkingDirectory().findChildFiles (juce::File::findFiles, false).size();
        check (probe.existsAsFile(), "(l) the probe binary exists (EJ_PROBE_BIN or the build-release artefact)", probe.getFullPathName());
        check (rc == 0 && rows >= 1, "(l1) --list-params on a non-PACE AU (AUDelay) prints >= 1 \"index<TAB>name<TAB>label<TAB>numSteps<TAB>isDiscrete\" line and exits 0 (RED on the current probe: no such mode)", "rc " + juce::String (rc) + ", rows " + juce::String (rows) + ", head: " + out.substring (0, 90).replace ("\n", " | "));
        // earlier legs of this guard legitimately write plugin_disabled*.json under the isolated root; the probe must neither create nor modify
        // them - the mtime snapshot covers both (a file the probe created shows up as a new entry, one it rewrote as a changed mtime)
        check (before == after && cwdAfter == cwdBefore && ! juce::File::getCurrentWorkingDirectory().getChildFile ("--list-params").exists(), "(l1) ...and touched NO state file: nothing under the isolated root's Library/EchoJay changed (mtimes: marker, plugin_disabled*.json, known_good.json), no \"--list-params\" marker in cwd (RED on the current probe: it creates a marker file named \"--list-params\")", "root files before " + juce::String (before.size()) + " after " + juce::String (after.size()) + ", cwd files " + juce::String (cwdBefore) + " -> " + juce::String (cwdAfter) + (before == after ? juce::String() : " CHANGED: " + [&] { juce::StringArray d; for (const auto& a : after) if (! before.contains (a)) d.add (a); return d.joinIntoString (", "); }()));
        juce::File::getCurrentWorkingDirectory().getChildFile ("--list-params").deleteFile();
        juce::StringPairArray env2 (env); env2.set ("EJ_PROBE_LIST_BOUND_MS", "0");   // the forced timeout: a 0 ms bound refuses before the create can complete
        // ---- 21 Sep 2026, the three probe changes owed from the item-5 scan + round (c)'s step mode ----
        for (const char* stray : { "--list-params", "--list-steps" }) juce::File::getCurrentWorkingDirectory().getChildFile (stray).deleteFile();   // an older probe read the mode word as a marker path
        {
            const juce::StringArray cla { "CLA-76 (m)", "AudioUnit:Effects/aufx,76CM,ksWV", "3d307263", "--list-params" };   // Waves (WaveShell), not PACE: its banner has no trailing newline
            juce::String out4; const int rc4 = runProbe (cla, env, out4, 60000);
            if (rc4 == 3) { check (false, "(l4) --list-params on a WaveShell AU (CLA-76 (m)) prints row 0 at the START of a line (the banner no longer glues to it)", "CLA-76 (m) refused: " + out4.substring (0, 80)); }
            else check (rc4 == 0 && (out4.contains ("\n0\t") ), "(l4) --list-params on a WaveShell AU (CLA-76 (m)) prints row 0 at the START of a line (the banner no longer glues to it)", "rc " + juce::String (rc4) + ": " + out4.fromFirstOccurrenceOf ("eInit", false, false).substring (0, 60).replace ("\n", "\\n"));
            const juce::StringArray steps { "CLA-76 (m)", "AudioUnit:Effects/aufx,76CM,ksWV", "3d307263", "--list-steps", "5" };
            juce::String out5; const int rc5 = runProbe (steps, env, out5, 60000);
            const int stepLines = juce::StringArray::fromLines (out5).size() - 0; int nStep = 0; juce::StringArray texts; for (const auto& l : juce::StringArray::fromLines (out5)) if (l.startsWith ("step\t")) { ++nStep; texts.add (l.fromLastOccurrenceOf ("\t", false, false)); }
            check (rc5 == 0 && nStep == 3 && texts.contains ("50Hz") && texts.contains ("60Hz") && texts.contains ("Off"), "(l5) --list-steps 5 on CLA-76 (m) walks the Analog control's 3 detents and prints their panel texts (50Hz / 60Hz / Off)", "rc " + juce::String (rc5) + " steps " + juce::String (nStep) + " texts " + texts.joinIntoString ("|") + " (" + juce::String (stepLines) + " lines)");
            check (! juce::File::getCurrentWorkingDirectory().getChildFile ("--list-steps").exists(), "(l5) ...and step mode writes no marker file", "");
            juce::File vst3dir ("/Library/Audio/Plug-Ins/VST3"); juce::Array<juce::File> v3; vst3dir.findChildFiles (v3, juce::File::findDirectories, false, "*.vst3");
            // 22 Sep 2026 (ruling 5): the bundle must LOAD, so the candidate is the first non-PACE VST3 whose binary carries the probe's
            // architecture (an x86_64-only bundle cannot be opened by the arm64 probe - that is the dyld "incompatible architecture" refusal)
            // 22 Sep 2026 (ruling 5): the leg is about the PROBE's ability to load a VST3, not about one particular bundle: it tries the
            // non-PACE bundles of the probe's architecture in order (up to six) and passes when one loads with a parameter row
            juce::Array<juce::File> cands;
            for (const auto& f : v3)
            {
                if (echojay::isPaceWrapped (f)) continue;
                const auto bin = f.getChildFile ("Contents/MacOS").getChildFile (f.getFileNameWithoutExtension());
                if (! bin.existsAsFile()) continue;
                juce::ChildProcess lp; juce::String archs; if (lp.start (juce::StringArray { "/usr/bin/lipo", "-archs", bin.getFullPathName() })) archs = lp.readAllProcessOutput();
               #if defined(__arm64__) || defined(__aarch64__)
                if (! archs.contains ("arm64")) continue;
               #else
                if (! archs.contains ("x86_64")) continue;
               #endif
                cands.add (f); if (cands.size() >= 6) break;
            }
            juce::String tried; bool loaded = false; juce::String loadedName;
            for (const auto& pick : cands)
            {
                const juce::StringArray v { pick.getFileNameWithoutExtension(), pick.getFullPathName(), "0", "--list-params" };
                juce::String out6; const int rc6 = runProbe (v, env, out6, 60000);
                tried += pick.getFileNameWithoutExtension() + " rc " + juce::String (rc6) + (out6.contains ("vst3 class:") ? " (" + out6.fromFirstOccurrenceOf ("vst3 class:", false, false).upToFirstOccurrenceOf ("\n", false, false).trim() + ")" : juce::String()) + "; ";
                if (rc6 == 0 && out6.contains ("format=VST3") && out6.contains ("vst3 class:") && out6.contains ("\n0\t")) { loaded = true; loadedName = pick.getFileNameWithoutExtension(); break; }
            }
            check (loaded, "(l6) a .vst3 path is probed as VST3 AND LOADS (rc 0, a parameter row; 22 Sep 2026: rc 3 \"Unable to load VST-3 plug-in file\" no longer passes - the class is resolved through findAllTypesForFile)", (loaded ? "loaded " + loadedName + " | " : juce::String ("none of: ")) + tried);
            const auto ent = juce::File::getCurrentWorkingDirectory().getChildFile ("tools/au_instantiate_probe/EchoJayProbe.entitlements").loadFileAsString();
            check (ent.contains ("com.apple.security.cs.disable-library-validation") && ent.contains ("com.apple.security.cs.allow-unsigned-executable-memory"), "(l7) the probe's entitlements source in the repo carries disable-library-validation AND allow-unsigned-executable-memory (the PACE wrapper's in-memory code)", ent.isEmpty() ? "no tools/au_instantiate_probe/EchoJayProbe.entitlements" : "both keys");
        }
        juce::String out2; const int rc2 = runProbe (delay, env2, out2, 40000);
        check (rc2 == 3 && out2.contains ("refused timeout"), "(l2) --list-params with the timeout FORCED (bound 0 ms) prints \"refused timeout ...\" and exits 3", "rc " + juce::String (rc2) + ": " + out2.substring (0, 80).replace ("\n", " | ") + " ... " + out2.getLastCharacters (60).replace ("\n", " | "));
#ifdef EJ_PACE_CHECK
        const juce::File decap ("/Library/Audio/Plug-Ins/Components/Decapitator.component"), audelay = echojay::bundleFor ([] { juce::PluginDescription d; d.pluginFormatName = "AudioUnit"; d.name = "AUDelay"; return d; }());
        juce::PluginDescription dd; dd.pluginFormatName = "AudioUnit"; dd.name = "Decapitator"; dd.fileOrIdentifier = "AudioUnit:Effects/aufx,DCPT,SnAu";
        check (decap.isDirectory() && echojay::isPaceWrapped (decap) && ! echojay::isPaceWrapped (audelay), "(l3) isPaceWrapped: Decapitator.component (an Eden bundle) true; AUDelay's component false", decap.getFullPathName() + " / " + audelay.getFullPathName());
        check (echojay::refuseIfPaceWrapped (dd).startsWith ("PACE-wrapped: signed probe only"), "(l3) an unsigned harness REFUSES a PACE-wrapped description before load with \"PACE-wrapped: signed probe only\"", echojay::refuseIfPaceWrapped (dd));
#else
        check (false, "(l3) isPaceWrapped: Decapitator.component (an Eden bundle) true; AUDelay's component false", "no EJPaceCheck.h on this build (RED by name)");
        check (false, "(l3) an unsigned harness REFUSES a PACE-wrapped description before load with \"PACE-wrapped: signed probe only\"", "no EJPaceCheck.h on this build (RED by name)");
#endif
    }

    std::printf ("\n==== preflight_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
