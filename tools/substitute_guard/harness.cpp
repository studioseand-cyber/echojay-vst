// substitute_guard - ITEM 3 + AMENDMENT (17 Sep 2026 ruling) on the REAL ChainHost.
// Dial-only ON, a third-party probe (real AudioPluginInstance, role "compressor") whose fetches answer with no
// map and no near map, settings {"params":{"threshold_db":-18,"ratio":4}}: after the bounded settle the slot is
// REPLACED by EchoJay Compressor, dialled applied 2/2, the row says SUBSTITUTED, and NO coral row exists anywhere
// (notDialable false, no "NOT DIALABLE"). Dial-only OFF: no substitution, and the red NOT DIALABLE row exists.
// RED today (-DEJ_GUARD_TODAY): no substitution API; the slot stays the probe, status noMap.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "../guard_common/probe.h"
#include "EedCompressorProcessor.h"
#include "EedLimiterProcessor.h"
#include "BuiltinIntentTranslate.h"   // referenced so the static lib links the TU whose registrar adds "EchoJay Compressor"
#include <cstdio>
struct EchoJayBorrowHostTestAccess { static juce::AudioProcessor* proc (ChainHost& h, int i) { return h.getSlotProcessor (i); }
    // 21n ruling 1c: seed the scanned entries (buildRecommendable's source) as a real scan would
    static void addEntry (ChainHost& h, const juce::PluginDescription& d) { std::lock_guard<std::mutex> lk (h.pluginsMutex_); h.entries_.add (d); } };
#include "PluginScanner.h"
namespace { int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::var settings() { auto* p = new juce::DynamicObject(); p->setProperty ("threshold_db", -18.0); p->setProperty ("ratio", 4.0); auto* o = new juce::DynamicObject(); o->setProperty ("params", juce::var (p)); return juce::var (o); }
juce::var emptyMiss (const juce::String& body) { return guardprobe::missRow (body, {}); }
const char* stName (ChainHost::DialStatus s) { switch (s) { case ChainHost::DialStatus::pending: return "pending"; case ChainHost::DialStatus::applied: return "applied"; case ChainHost::DialStatus::partial: return "partial"; case ChainHost::DialStatus::noMap: return "noMap"; case ChainHost::DialStatus::none: return "none"; default: return "other"; } }
void wire (ChainHost& h) {
    h.onNeedParamMaps = [&h] (const juce::StringArray& fps) { juce::Timer::callAfterDelay (30, [&h, fps] { auto* o = new juce::DynamicObject(); for (auto& fp : fps) o->setProperty (juce::Identifier (fp), juce::var()); h.storeParamMaps (juce::var (o)); }); };
    h.onNeedFallbackMaps = [&h] (const juce::String& body) { juce::Timer::callAfterDelay (50, [&h, body] { h.storeFallbackMaps (emptyMiss (body)); }); }; }
void settle (ChainHost& h) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (! h.dialStateSettled() && juce::Time::getMillisecondCounterHiRes() - t0 < 5000) guardprobe::pumpMs (10); }
}
using namespace guardprobe;
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedCompressorProcessor::schema();   // force-link (see include)
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_subst_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("substitute_guard: under dial-only a noMap slot becomes the built-in of its role, dialled; no coral row\n");
    const std::map<juce::String, juce::String> roles { { "guard comp", "compressor" } };
    std::printf ("== dial-only ON ==\n");
    {
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); wire (h); h.setDialOnlyMode (true);
        h.completeLoad (std::make_unique<Probe> (probeDesc (9, "Guard Comp")), probeDesc (9, "Guard Comp"), ChainHost::LoadOrigin::Restore);
        h.setSlotStructuredSettings (0, settings()); settle (h);
        check (h.getDialInfos()[0].status == ChainHost::DialStatus::noMap, "precondition: the probe ends noMap after its fetches answered", stName (h.getDialInfos()[0].status));
#ifdef EJ_GUARD_TODAY
        check (false, "slot 0 is a BUILT-IN (EchoJay Compressor) after the settle", "TODAY: no substitution API - slot stays \"" + h.getSlotInfo (0).name + "\" status " + stName (h.getDialInfos()[0].status));
#else
        const auto subs = h.substituteNoMapSlots (roles);
        check (subs.size() == 1 && subs[0].from == "Guard Comp" && subs[0].to == "EchoJay Compressor", "substituteNoMapSlots swapped the probe for EchoJay Compressor", subs.empty() ? juce::String ("none") : subs[0].from + " -> " + subs[0].to);
        check (h.getNumSlots() == 1 && h.getSlotInfo (0).name == "EchoJay Compressor" && h.getDialInfos()[0].builtin, "slot 0 is a BUILT-IN (EchoJay Compressor) after the settle", h.getSlotInfo (0).name);
        const auto di = h.getDialInfos()[0];
        check (di.status == ChainHost::DialStatus::applied && di.appliedCount == 2 && subs.size() == 1 && subs[0].applied == 2 && subs[0].requested == 2, "dialled: applied 2/2 (threshold_db, ratio) on the built-in", juce::String (stName (di.status)) + " applied=" + juce::String (di.appliedCount));
        const auto row = h.dialSummaryRow (0);
        check (row.contains ("SUBSTITUTED for \"Guard Comp\""), "the row says why (SUBSTITUTED for \"Guard Comp\")", row.fromFirstOccurrenceOf ("status=", false, false));
        check (! row.contains ("NOT DIALABLE") && ! di.notDialable, "AMENDMENT: no coral NOT DIALABLE row anywhere under dial-only");
        check (h.getSlotInfo (0).settings.startsWith ("Guard Comp had no working map - built EchoJay Compressor instead"), "the card text says the swap in normal words", h.getSlotInfo (0).settings.upToFirstOccurrenceOf ("\n", false, false));
#endif
    }
    std::printf ("== control: dial-only OFF ==\n");
    {
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); wire (h); h.setDialOnlyMode (false);
        h.completeLoad (std::make_unique<Probe> (probeDesc (10, "Guard Comp")), probeDesc (10, "Guard Comp"), ChainHost::LoadOrigin::Restore);
        h.setSlotStructuredSettings (0, settings()); settle (h);
#ifndef EJ_GUARD_TODAY
        const auto subs = h.substituteNoMapSlots (roles);
        check (subs.empty() && h.getSlotInfo (0).name == "Guard Comp", "dial-only OFF: nothing substituted");
        check (h.getDialInfos()[0].notDialable && h.dialSummaryRow (0).contains ("NOT DIALABLE"), "dial-only OFF: the red NOT DIALABLE row exists (the amendment's only home)", h.dialSummaryRow (0).fromFirstOccurrenceOf ("status=", false, false));
#else
        check (h.getSlotInfo (0).name == "Guard Comp", "dial-only OFF: nothing substituted");
#endif
    }
    std::printf ("== 18 Sep 2026 (item 4): a SUBSTITUTED built-in is always dialled - the model's THIRD-PARTY controls translate by semantic ==\n");
#ifndef EJ_GUARD_TODAY
    {
        // The live case: AMEK Mastering Compressor's controls as the model wrote them, the built-in standing in.
        auto amek = [] { auto* c = new juce::DynamicObject(); c->setProperty ("Ratio", "2:1"); c->setProperty ("Threshold", "-18 dB"); c->setProperty ("Attack", "30 ms"); c->setProperty ("Release", "auto"); c->setProperty ("Gain Reduction", "3-4 dB");
                         auto* o = new juce::DynamicObject(); o->setProperty ("controls", juce::var (c)); return juce::var (o); };
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); wire (h); h.setDialOnlyMode (true);
        h.completeLoad (std::make_unique<Probe> (probeDesc (19, "AMEK Mastering Compressor")), probeDesc (19, "AMEK Mastering Compressor"), ChainHost::LoadOrigin::Restore);
        h.setSlotStructuredSettings (0, amek()); settle (h);
        const auto subs = h.substituteNoMapSlots ({ { "amek mastering compressor", "compressor" } });
        const auto di = h.getDialInfos()[0];
        check (subs.size() == 1 && subs[0].to == "EchoJay Compressor" && di.builtin, "AMEK -> EchoJay Compressor substituted", subs.empty() ? juce::String ("none") : subs[0].to);
        check (di.status == ChainHost::DialStatus::applied && di.appliedCount >= 4 && di.appliedCount == di.requestedCount,
               "APPLIED n/n: Ratio 2:1 / Threshold -18 dB / Attack 30 ms / Release auto translated and dialled (was 0/3)",
               juce::String ("status=") + stName (di.status) + " applied=" + juce::String (di.appliedCount) + "/" + juce::String (di.requestedCount));
        auto* dev = dynamic_cast<EedDeviceProcessor*> (EchoJayBorrowHostTestAccess::proc (h, 0));
        check (dev != nullptr && std::abs (dev->getParamValue ("ratio") - 2.0) < 1e-3 && std::abs (dev->getParamValue ("threshold_db") + 18.0) < 1e-3
               && std::abs (dev->getParamValue ("attack_ms") - 30.0) < 1e-3 && dev->getParamValue ("auto_release") > 0.5,
               "the values landed on the device: ratio 2, threshold -18, attack 30 ms, auto release on",
               dev ? "ratio=" + juce::String (dev->getParamValue ("ratio")) + " thr=" + juce::String (dev->getParamValue ("threshold_db")) + " atk=" + juce::String (dev->getParamValue ("attack_ms")) + " autoRel=" + juce::String (dev->getParamValue ("auto_release")) : "no device");
        check (! h.getSlotInfo (0).settings.contains ("hand-dial"), "the card never says hand-dialing for a built-in", h.getSlotInfo (0).settings.upToFirstOccurrenceOf ("\n", false, false));
    }
    {   // the pure translator: a GR target with NO threshold derives the threshold from the measured input p90
        auto* c = new juce::DynamicObject(); c->setProperty ("Ratio", "2:1"); c->setProperty ("Gain Reduction", "3-4 dB"); c->setProperty ("Timing", "1");
        const juce::var cv (c);   // ONE owner (wrapping the raw pointer twice freed it under the first var - the harness's own bug, caught by the scribble leg)
        const auto tr = echojay::translateControlsForBuiltin ("EchoJay Compressor", EedCompressorProcessor::schema(), cv, -20.0f);
        const double thr = tr.payload.getProperty ("params", juce::var()).getProperty ("threshold_db", juce::var());
        check (tr.derivedThreshold && std::abs (thr - (-20.0 - 3.5 * 2.0)) < 1e-3, "GR 3-4 dB at 2:1 with input p90 -20 -> threshold_db -27 (p90 - GR * r/(r-1))", "thr=" + juce::String (thr) + " " + tr.translated.joinIntoString ("; "));
        check (tr.dropped.size() == 1 && tr.dropped[0].startsWith ("Timing 1"), "a control with no built-in counterpart is listed as dropped, not guessed", tr.dropped.joinIntoString ("; "));
        const auto trNoLevel = echojay::translateControlsForBuiltin ("EchoJay Compressor", EedCompressorProcessor::schema(), cv, std::numeric_limits<float>::quiet_NaN());
        check (! trNoLevel.derivedThreshold && trNoLevel.dropped.size() == 2, "with no measured level the GR target is NOT turned into a number (declined, listed)", trNoLevel.dropped.joinIntoString ("; "));
    }
    {   // item 5 needs a limiter INPUT GAIN on the built-in: input_db, -12..+12 dB
        EedLimiterProcessor lim; lim.prepareToPlay (48000.0, 512);
        auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", 7.5); pp->setProperty ("ceiling_db", -0.1);
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp));
        int ap = 0, sk = 0; lim.applyStructured (juce::var (w), EedDeviceProcessor::ParamSource::Assistant, &ap, &sk);
        check (ap == 2 && sk == 0 && std::abs (lim.getParamValue ("input_db") - 7.5) < 1e-6 && std::abs (lim.getParamValue ("ceiling_db") + 0.1) < 1e-6, "EchoJay Limiter dials input_db +7.5 and ceiling_db -0.1 (2/2 applied)", "applied=" + juce::String (ap) + " skipped=" + juce::String (sk));
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
        for (int k = 0; k < 6; ++k) { b.clear(); lim.processBlock (b, m); }   // the push EASES over 50 ms (ruling G): let it land first
        // Limiter v2 (8 Oct 2026) reports a fixed latency (1160 samples at 48 k), so the impulse leaves in a LATER block:
        // drain silent blocks until the reported latency has passed and take the peak over all of them. The claim under
        // test - the gain is applied before the ceiling - is unchanged; only the window the old (108-sample) limiter let
        // the guard get away with is widened to the delay the device reports.
        b.clear(); b.setSample (0, 100, 0.1f); b.setSample (1, 100, 0.1f); lim.processBlock (b, m);
        float heard = b.getMagnitude (0, 0, 512);
        for (int done = 512; done < lim.getLatencySamples() + 512; done += 512) { b.clear(); lim.processBlock (b, m); heard = juce::jmax (heard, b.getMagnitude (0, 0, 512)); }
        check (heard > 0.2f, "+7.5 dB of input gain is heard before the ceiling (a 0.1 impulse leaves above 0.2 within the reported latency, after the 50 ms ease)", juce::String (heard, 3) + " (latency " + juce::String (lim.getLatencySamples()) + ")");
    }
#else
    check (false, "APPLIED n/n: Ratio 2:1 / Threshold -18 dB / Attack 30 ms / Release auto translated and dialled (was 0/3)", "TODAY: the controls payload is dropped");
#endif
    std::printf ("== V. 22 Sep 2026 (21m item 2): a mono variant is never loaded on a stereo rack - the stereo sibling by name, else refused ==\n");
    {
        auto mk = [] (const char* name) { juce::PluginDescription d; d.name = name; d.pluginFormatName = "AudioUnit"; d.fileOrIdentifier = juce::String ("AudioUnit:Effects/aufx,") + name; d.uniqueId = d.deprecatedUid = juce::String (name).hashCode(); d.manufacturerName = "Waves"; return d; };
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512); h.setHostChannelWidth (2);
        h.addKnownPluginForTests (mk ("PuigChild 660 (m)")); h.addKnownPluginForTests (mk ("PuigChild 660 (s)")); h.addKnownPluginForTests (mk ("Kramer Tape (m)"));
        juce::String why; const auto s = h.variantForRack (mk ("PuigChild 660 (m)"), &why);
        check (s.name == "PuigChild 660 (s)" && why.isEmpty(), "V1. \"PuigChild 660 (m)\" requested on a STEREO rack -> \"PuigChild 660 (s)\" (the same product's stereo sibling)", s.name + " | " + why);
        const auto k = h.variantForRack (mk ("Kramer Tape (m)"), &why);
        check (k.name == "Kramer Tape (m)" && why == "mono-only plugin on a stereo channel", "V2. a mono variant with NO stereo sibling -> refused \"mono-only plugin on a stereo channel\" (not loaded)", k.name + " | " + why);
        const auto st = h.variantForRack (mk ("PuigChild 660 (s)"), &why);
        check (st.name == "PuigChild 660 (s)" && why.isEmpty(), "V3. a stereo variant is untouched", st.name);
        h.setHostChannelWidth (1); const auto m1 = h.variantForRack (mk ("PuigChild 660 (m)"), &why);
        check (m1.name == "PuigChild 660 (m)" && why.isEmpty(), "V4. on a MONO rack the mono variant loads as asked", m1.name);
    }
    std::printf ("== F. 22 Sep 2026 (21n ruling 1c): the [AVAILABLE PLUGINS] feed carries REGISTRATION names with their variant suffixes; a family is never collapsed onto a bare stem ==\n");
    {
        auto waves = [] (const char* name, const char* code) { juce::PluginDescription d; d.name = name; d.pluginFormatName = "AudioUnit"; d.manufacturerName = "Waves";
            d.fileOrIdentifier = juce::String ("AudioUnit:Effects/aufx,") + code + ",ksWV"; d.uniqueId = d.deprecatedUid = juce::String (code).hashCode(); d.version = "15.0.70"; return d; };
        ScannedPlugin sp; sp.name = "PuigChild 660"; sp.manufacturer = "Waves"; sp.format = "AU"; sp.enabled = true; sp.uid = "waves-660";
        {   // only the (m) registered (this Mac): the feed must say "PuigChild 660 (m)"
            ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512);
            EchoJayBorrowHostTestAccess::addEntry (h, waves ("PuigChild 660 (m)", "FCHM"));
            h.buildRecommendable (std::vector<ScannedPlugin> { sp }, {});
            const auto names = h.getRecommendableNames();
            check (names.contains ("PuigChild 660 (m)") && ! names.contains ("PuigChild 660"), "F1. a scan with only \"(m)\" registered -> the feed carries \"PuigChild 660 (m)\", not the bare stem (RED as it stood: \"PuigChild 660\")", names.joinIntoString ("|"));
            check (h.buildMapFpsJson (200).contains ("PuigChild 660 (m)") || ! h.buildMapFpsJson (200).contains ("PuigChild 660"), "F1. mapFps keys on the same registration name (no fingerprint -> no entry, but never the bare stem)", h.buildMapFpsJson (200).substring (0, 80));
        }
        {   // both variants registered: BOTH are offered, each under its own registration name
            ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512);
            EchoJayBorrowHostTestAccess::addEntry (h, waves ("CLA-76 (m)", "C76M")); EchoJayBorrowHostTestAccess::addEntry (h, waves ("CLA-76 (s)", "C76S"));
            ScannedPlugin c; c.name = "CLA-76"; c.manufacturer = "Waves"; c.format = "AU"; c.enabled = true; c.uid = "waves-cla76";
            h.buildRecommendable (std::vector<ScannedPlugin> { c }, {});
            const auto names = h.getRecommendableNames();
            check (names.contains ("CLA-76 (m)") && names.contains ("CLA-76 (s)") && ! names.contains ("CLA-76"), "F2. (m) + (s) registered -> both rows, no bare stem", names.joinIntoString ("|"));
        }
        {   // an unsuffixed registration is unchanged
            ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512);
            juce::PluginDescription d; d.name = "Pro-Q 3"; d.pluginFormatName = "AudioUnit"; d.manufacturerName = "FabFilter"; d.fileOrIdentifier = "AudioUnit:Effects/aufx,FQ3p,FabF"; d.uniqueId = d.deprecatedUid = 4242;
            EchoJayBorrowHostTestAccess::addEntry (h, d);
            ScannedPlugin q; q.name = "Pro-Q 3"; q.manufacturer = "FabFilter"; q.format = "AU"; q.enabled = true; q.uid = "ff-q3";
            h.buildRecommendable (std::vector<ScannedPlugin> { q }, {});
            // RE-RULED 29 Sep 2026 (21t-k item 2(b)): the feed also carries EchoJay's OWN devices now, on every
            // send - Sean's 21:24 log had `EJChat: chain name OUT OF FEED: "EchoJay Pitch"` on a build of our own
            // tuner. "exactly as before" is therefore asserted as "the scanned row, and nothing else scanned":
            // the built-ins are subtracted before the count.
            {
                const auto all = h.getRecommendableNames();
                juce::StringArray scanned = all;
                for (const auto& b : ChainHost::builtinDeviceNames()) scanned.removeString (b);
                check (all.contains ("Pro-Q 3") && scanned.size() == 1,
                       "F3 as re-ruled. an unsuffixed registration is offered exactly as before - one scanned row, "
                       "beside the built-ins the feed now always carries",
                       "scanned: " + scanned.joinIntoString ("|") + "   all: " + all.joinIntoString ("|"));
            }
        }
    }
    // ---- A BLACKLISTED PLUGIN IS NOT IN THE OUTGOING FEED (2 Oct 2026 ruling) ------------------------------
    {
        std::printf ("\n-- a blacklisted NAME-ONLY row is withheld from the feed, not just refused at load --\n");
        // Sean's 16:33 session, second half. AVOX SYBIL is on the crash skip list, and the first fix stopped it
        // LOADING - but it was still in the [AVAILABLE PLUGINS] list the server sees, so the model proposed it and
        // the user watched it be offered and withheld in the same turn. withholdReasonLocked had both faults this
        // rig reproduces: it compared the blacklist RAW (so a trailing-space OSType never matched its trimmed line
        // on disk) and it short-circuited when fileOrIdentifier was empty - which is every merged feed row.
        auto antares = [] (const char* nm, const char* ident)
        {
            juce::PluginDescription d; d.name = nm; d.pluginFormatName = "AudioUnit";
            d.manufacturerName = "Antares"; d.fileOrIdentifier = ident;
            d.uniqueId = d.deprecatedUid = juce::String (ident).hashCode(); d.version = "4.2.0"; return d;
        };
        // The real identifier ends in a space: Antares' manufacturer OSType is literally 'VST '.
        const juce::String realIdent = "AudioUnit:Effects/aufx,AnVD,VST ";
        ChainHost h (ChainHost::Mode::Primary); h.prepare (48000.0, 512);
        EchoJayBorrowHostTestAccess::addEntry (h, antares ("AVOX SYBIL",  realIdent.toRawUTF8()));
        EchoJayBorrowHostTestAccess::addEntry (h, antares ("AVOX THROAT", "AudioUnit:Effects/aufx,AnVT,VST "));
        // ...and the blacklist line is the TRIMMED form, which is what is actually on disk.
        h.addToBlacklist (realIdent.trimEnd(), "crashed the host during instantiate (deadman)");
        // The feed rows are NAME-ONLY, which is the shape that defeated the old check.
        ScannedPlugin sy; sy.name = "AVOX SYBIL";  sy.manufacturer = "Antares"; sy.format = "AU"; sy.enabled = true; sy.uid = "antares-sybil";
        ScannedPlugin th; th.name = "AVOX THROAT"; th.manufacturer = "Antares"; th.format = "AU"; th.enabled = true; th.uid = "antares-throat";
        h.buildRecommendable (std::vector<ScannedPlugin> { sy, th }, {});
        const auto names = h.getRecommendableNames();
        check (! names.contains ("AVOX SYBIL"),
               "a blacklisted plugin is ABSENT from the outgoing feed, so the model never proposes it  (RED as it "
               "stood: offered, proposed, then refused at load - the user saw it withheld in the same turn)",
               names.joinIntoString ("|"));
        // BOTH DIRECTIONS, or this is just "the feed is empty": its sibling, same manufacturer, same OSType shape,
        // not on the list, is still offered.
        check (names.contains ("AVOX THROAT"),
               "...while its unlisted sibling - same manufacturer, same trailing-space OSType - is still offered",
               names.joinIntoString ("|"));
    }

    // ---- THE BLACKLIST KEY: AN OSType MAY END IN A SPACE (2 Oct 2026 ruling) -----------------------------
    {
        std::printf ("\n-- a trailing-space OSType is blacklistable --\n");
        // Sean's 11:35/14:20 sessions: AVOX SYBIL was offered in the chain feed and then could not be built.
        // Antares' AU manufacturer OSType is literally 'VST ', so the real identifier is
        // "AudioUnit:Effects/aufx,AnVD,VST " - 32 characters. chain_blacklist.txt held the TRIMMED 31-character
        // form (the reader trimmed it, and the writer had too), and isBlacklisted compares exactly, so the crash
        // skip list answered FALSE for a plugin on the list. The file's own header promises a listed plugin is
        // "withheld from the chain feed and refused at load"; it was neither. A class defect: every product whose
        // OSType ends in a space was unblacklistable, which is to say the skip list failed on exactly the plugins
        // that crash.
        const juce::String real    = "AudioUnit:Effects/aufx,AnVD,VST ";   // 32 - what the scan reports
        const juce::String onDisk  = "AudioUnit:Effects/aufx,AnVD,VST";    // 31 - what the file holds
        check (real.length() == 32 && onDisk.length() == 31,
               "the fixture is the real pair: the identifier is one character longer than the stored line",
               juce::String (real.length()) + " vs " + juce::String (onDisk.length()));
        check (ChainHost::blacklistKey (real) == ChainHost::blacklistKey (onDisk),
               "the stored (trimmed) line and the real identifier reduce to ONE key, so a file already on disk "
               "keeps matching  (RED as it stood: an exact compare answered false and the plugin was offered)",
               "\"" + ChainHost::blacklistKey (real) + "\"");
        // ...and the other direction, or the key would just be "everything matches everything".
        check (ChainHost::blacklistKey (real) != ChainHost::blacklistKey ("AudioUnit:Effects/aufx,AnVD,SfTb"),
               "...while a DIFFERENT manufacturer is still a different key - the normalisation is trailing "
               "whitespace only, not a fuzzy match");
        check (ChainHost::blacklistKey ("  AudioUnit:Effects/aufx,AnVD,VST ") != ChainHost::blacklistKey (real),
               "...and LEADING space is not stripped: it is not part of any identifier, so a line carrying one is "
               "malformed and must not silently match");
    }

    std::printf ("\n==== substitute_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
