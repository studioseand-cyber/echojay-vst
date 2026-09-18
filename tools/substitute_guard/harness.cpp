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
struct EchoJayBorrowHostTestAccess { static juce::AudioProcessor* proc (ChainHost& h, int i) { return h.getSlotProcessor (i); } };
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
        b.clear(); b.setSample (0, 100, 0.1f); b.setSample (1, 100, 0.1f); lim.processBlock (b, m);
        check (b.getMagnitude (0, 0, 512) > 0.2f, "+7.5 dB of input gain is heard before the ceiling (a 0.1 impulse leaves above 0.2, after the 50 ms ease)", juce::String (b.getMagnitude (0, 0, 512), 3));
    }
#else
    check (false, "APPLIED n/n: Ratio 2:1 / Threshold -18 dB / Attack 30 ms / Release auto translated and dialled (was 0/3)", "TODAY: the controls payload is dropped");
#endif
    std::printf ("\n==== substitute_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
