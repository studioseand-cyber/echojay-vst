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
#include <cstdio>
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
    double worst = 0; while (! done && juce::Time::getMillisecondCounterHiRes() - t0 < 14000) worst = juce::jmax (worst, pumpMeasuring (200));
    const double waited = juce::Time::getMillisecondCounterHiRes() - t0;
    check (done, "the pre-flight settled", juce::String ((int) waited) + " ms");
    const auto vh = ChainHost::preflightVerdictFor (dHang), ve = ChainHost::preflightVerdictFor (dErr), vg = ChainHost::preflightVerdictFor (dGood);
    check (vh.state == ChainHost::PreflightState::hang && waited >= 9900 && waited < 12500, "(a) the sleeping fixture TIMED OUT at 10 s (probe killed)", "state=" + juce::String ((int) vh.state) + " after " + juce::String ((int) waited) + " ms: " + vh.note);
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
        const double t2 = juce::Time::getMillisecondCounterHiRes(); while (! done2 && juce::Time::getMillisecondCounterHiRes() - t2 < 14000) pumpMeasuring (200);
        check (done2 && ChainHost::preflightVerdictFor (dStale).state == ChainHost::PreflightState::hang && echojay::hangsOnLoadMarkLive (uStale), "(f) the re-probe timed out again -> a NEW mark with a new 7-day expiry", echojay::disableReasonDetailFor (uStale));
    }
#else
    std::printf ("  (skipped in TODAY mode)\n"); ++failures;
#endif
    std::printf ("\n==== preflight_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
