// P3: the mode machine and the pitch_scale object move, through the SAME
// applyStructured funnel the chain hands settings to.
#include <JuceHeader.h>
#include "EedPitchProcessor.h"
#include "EedPitchEditor.h"
#include "EedDeviceRegistry.h"
#include "EedKeyFeed.h"
#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>
#include <vector>
static int g_fail = 0;
static void check (bool c, const juce::String& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.toRawUTF8()); if (! c) ++g_fail; }

static juce::var params (std::initializer_list<std::pair<const char*, juce::var>> kv)
{
    auto* o = new juce::DynamicObject();
    for (auto& p : kv) o->setProperty (p.first, p.second);
    auto* outer = new juce::DynamicObject();
    outer->setProperty ("params", juce::var (o));
    return juce::var (outer);
}

int main()
{
    EedPitchProcessor p;
    p.prepareToPlay (48000.0, 512);

    std::printf ("== a FRESHLY CONSTRUCTED device matches its advertised defaults ==\n");
    {
        // The advertisement is a promise about what adding this device does.
        // A member initialiser that disagrees with its ParamSpec breaks that
        // promise silently, and only on the FIRST instance - a state restore
        // hides it forever after.
        EedPitchProcessor fresh;
        fresh.prepareToPlay (48000.0, 512);
        int mismatches = 0;
        for (const auto& sp : EedPitchProcessor::schema().params())
        {
            if (sp.id == "reset_stats") continue;          // momentary, always 0
            const double got = fresh.getParamValue (juce::String (sp.id));
            if (std::abs (got - sp.def) > 1.0e-4)
            {
                ++mismatches;
                std::printf ("      %s: constructed %.3f, advertised %.3f\n",
                             sp.id.c_str(), got, sp.def);
            }
        }
        check (mismatches == 0, "every param constructs at its advertised default");
    }

    std::printf ("== correction_mode WRITES the visible params ==\n");
    {
        juce::String s = p.applyStructured (params ({ { "correction_mode", "hard" } }));
        std::printf ("    applied: %s\n", s.toRawUTF8());
        check (p.getParamValue ("retune_speed_ms") == 0.0, "hard wrote retune_speed_ms 0");
        check (p.getParamValue ("flex") == 0.0, "hard wrote flex 0");
        check (p.getParamValue ("humanize") == 0.0, "hard wrote humanize 0");
        // ON in every mode since the §4 table correction: target selection
        // without vibrato smoothing flips between adjacent degrees whenever a
        // wide vibrato sits on a semitone boundary, at ANY retune speed.
        check (p.getParamValue ("targeting_ignores_vibrato") == 1.0, "hard wrote ignore_vibrato on");
        check (s.contains ("retune_speed_ms") && s.contains ("flex") && s.contains ("humanize"),
               "the summary NAMES what it changed, not just the mode");
    }
    {
        juce::String s = p.applyStructured (params ({ { "correction_mode", "natural" } }));
        std::printf ("    applied: %s\n", s.toRawUTF8());
        check (p.getParamValue ("retune_speed_ms") == 120.0, "natural wrote retune 120");
        check (p.getParamValue ("flex") == 55.0, "natural wrote flex 55");
        check (p.getParamValue ("humanize") == 60.0, "natural wrote humanize 60");
        check (p.getParamValue ("targeting_ignores_vibrato") == 1.0, "natural wrote ignore_vibrato on");
    }
    {
        p.applyStructured (params ({ { "correction_mode", "balanced" } }));
        check (p.getParamValue ("retune_speed_ms") == 40.0 && p.getParamValue ("flex") == 25.0,
               "balanced matches the spec table (40 / 25 / 30)");
        p.applyStructured (params ({ { "correction_mode", "tuned" } }));
        check (p.getParamValue ("retune_speed_ms") == 8.0 && p.getParamValue ("flex") == 0.0,
               "tuned matches the spec table (8 / 0 / 0)");
    }

    std::printf ("== a manual move knocks the display to custom ==\n");
    {
        p.applyStructured (params ({ { "correction_mode", "natural" } }));
        const auto* spec = EedPitchProcessor::schema().find ("correction_mode");
        check (spec->choiceLabel (p.getParamValue ("correction_mode")) == "natural",
               "reads natural after selecting it");
        p.applyStructured (params ({ { "flex", 20.0 } }));
        check (spec->choiceLabel (p.getParamValue ("correction_mode")) == "custom",
               "moving flex by hand shows custom");
        check (p.getParamValue ("flex") == 20.0, "...and the hand-set value stuck");
    }

    std::printf ("== pitch_scale: MERGE semantics keyed on semitone ==\n");
    {
        p.applyStructured (params ({ { "scale", "major" } }));
        check (p.corrector().degreeEnabled (0) && ! p.corrector().degreeEnabled (1),
               "major enabled C, disabled C#");

        auto* d = new juce::DynamicObject();
        d->setProperty ("semitone", 11);
        d->setProperty ("enabled", false);
        juce::Array<juce::var> arr; arr.add (juce::var (d));
        auto* outer = new juce::DynamicObject();
        outer->setProperty ("pitch_scale", juce::var (arr));

        int applied = 0, skipped = 0;
        juce::String s = p.applyStructured (juce::var (outer), &applied, &skipped);
        std::printf ("    applied: %s\n", s.toRawUTF8());
        check (! p.corrector().degreeEnabled (11), "the 7th was removed");
        check (p.corrector().degreeEnabled (0) && p.corrector().degreeEnabled (4),
               "every OMITTED degree kept its state - merge, not replace");
        const auto* sp = EedPitchProcessor::schema().find ("scale");
        check (sp->choiceLabel (p.getParamValue ("scale")) == "custom",
               "editing a degree moves scale to custom");
    }

    std::printf ("== pitch_scale: bias_cents merges independently of enabled ==\n");
    {
        auto* d = new juce::DynamicObject();
        d->setProperty ("semitone", 4);
        d->setProperty ("bias_cents", -14.0);
        juce::Array<juce::var> arr; arr.add (juce::var (d));
        auto* outer = new juce::DynamicObject();
        outer->setProperty ("pitch_scale", juce::var (arr));
        p.applyStructured (juce::var (outer));
        check (std::abs (p.corrector().degreeBias (4) + 14.0f) < 0.01f, "E biased -14 cents");
        check (p.corrector().degreeEnabled (4), "and `enabled` was untouched by an omitted field");
    }

    std::printf ("== an entry with no semitone is SKIPPED, not guessed ==\n");
    {
        auto* d = new juce::DynamicObject();
        d->setProperty ("enabled", false);
        juce::Array<juce::var> arr; arr.add (juce::var (d));
        auto* outer = new juce::DynamicObject();
        outer->setProperty ("pitch_scale", juce::var (arr));
        int applied = 0, skipped = 0;
        p.applyStructured (juce::var (outer), &applied, &skipped);
        check (skipped >= 1 && applied == 0, "keyless entry reported as skipped");
    }

    std::printf ("== P4: key_source auto FOLLOWS the detected key ==\n");
    {
        auto pump = [&] (int blocks)
        {
            juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
            for (int i = 0; i < blocks; ++i) { b.clear(); p.processBlock (b, m); }
        };
        auto publish = [] (int root, bool minor, float conf, float tuning, const char* src)
        {
            echojay::DetectedKeyFact f;
            f.valid = true; f.root = root; f.minor = minor;
            f.confidence = conf; f.tuningHz = tuning; f.fromBus = true;
            std::strncpy (f.sourceName, src, sizeof (f.sourceName) - 1);
            echojay::KeyFeed::instance().publish (f);
        };

        p.applyStructured (params ({ { "key_source", "auto" },
                                     { "reference_source", "auto" } }));

        // F# minor, confidently, from a bus source.
        publish (6, true, 0.86f, 441.3f, "Music Bus");
        pump (4);
        check (p.getParamValue ("key_root") == 6.0, "key_root followed to F#");
        const auto* sp = EedPitchProcessor::schema().find ("scale");
        check (sp->choiceLabel (p.getParamValue ("scale")) == "minor", "scale followed to minor");
        // The LIVE grid follows; the PARAM is the manual FIELD and must not
        // (29 Aug 2026: exporting the detected value through the param was
        // the laundering path - this assertion used to demand it).
        check (std::abs (p.autoKeyState().refApplied - 441.3) < 0.1,
               "the LIVE grid followed the detected tuning");
        check (std::abs (p.getParamValue ("reference_hz") - 440.0) < 0.1,
               "the manual FIELD stayed 440 - a detected value never enters it");

        // THE CIRCULARITY GUARD: a fact derived from this instance's own
        // channel must not move the grid - fall back to 440, never to the
        // channel itself.
        p.setKeyFeedSelfId (77);
        {
            echojay::DetectedKeyFact f;
            f.valid = true; f.root = 6; f.minor = true; f.confidence = 0.86f;
            f.tuningHz = 438.0f; f.fromBus = true;
            f.publisherId = 77; f.selfDerived = true;
            std::strncpy (f.sourceName, "Self", sizeof (f.sourceName) - 1);
            echojay::KeyFeed::instance().publish (f);
        }
        pump (4);
        check (std::abs (p.autoKeyState().refApplied - 440.0) < 0.1,
               "a self-derived fact from THIS instance is not followed - grid falls back to 440");
        check (p.autoKeyState().refSelfIgnored,
               "...and the state says so (refSelfIgnored), for the readout");
        p.setKeyFeedSelfId (0);
        // Restore the external fact so the downstream source-name check sees
        // the state it always saw.
        publish (6, true, 0.86f, 441.3f, "Music Bus");
        pump (4);

        const auto st = p.autoKeyState();
        check (st.applied && st.sourceName == "Music Bus",
               "the state names its source for the UI: " + st.sourceName);
    }

    std::printf ("== KEY-SIDE CIRCULARITY GUARD (behind debugKeySelfGuard): a self-derived fact "
                 "is not followed for key root/mode - chromatic, never the last key ==\n");
    {
        auto pump = [&] (int blocks)
        {
            juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
            for (int i = 0; i < blocks; ++i) { b.clear(); p.processBlock (b, m); }
        };
        auto publishFact = [] (int root, bool minor, float conf, float tuning,
                               const char* src, uint64_t publisher, bool selfDerived)
        {
            echojay::DetectedKeyFact f;
            f.valid = true; f.root = root; f.minor = minor; f.confidence = conf;
            f.tuningHz = tuning; f.fromBus = true;
            f.publisherId = publisher; f.selfDerived = selfDerived;
            std::strncpy (f.sourceName, src, sizeof (f.sourceName) - 1);
            echojay::KeyFeed::instance().publish (f);
        };
        const auto* sp = EedPitchProcessor::schema().find (EedPitchProcessor::kScale);
        auto allDegrees = [&]
        { return sp != nullptr && sp->choiceLabel (p.getParamValue ("scale")) == "chromatic"; };

        // Baseline: an EXTERNAL usable fact is followed (as before).
        p.applyStructured (params ({ { "key_source", "auto" } }));
        p.setKeyFeedSelfId (77);
        publishFact (6, true, 0.86f, 441.3f, "Music Bus", 12345, false);
        pump (4);
        check (p.autoKeyState().applied && p.autoKeyState().root == 6 && p.autoKeyState().minor,
               "baseline: an external fact is followed (F# minor)");

        // THE DEFECT, documented with the flag OFF: the same fact stamped
        // self-derived from THIS instance is still applied for key today.
        p.debugKeySelfGuard (false);
        publishFact (6, true, 0.86f, 438.0f, "this channel", 77, true);
        pump (4);
        check (p.autoKeyState().applied && ! p.autoKeyState().keySelfIgnored,
               "flag OFF (today's build): the self-derived key IS followed - the defect");
        check (std::abs (p.autoKeyState().refApplied - 440.0) < 0.1,
               "...while the reference guard already ignores it (440)");

        // THE GUARD, flag ON: chromatic, actively, and the state says why.
        p.debugKeySelfGuard (true);
        pump (4);
        const auto st = p.autoKeyState();
        check (! st.applied && st.fellBack, "flag ON: the self-derived key is NOT applied");
        check (st.keySelfIgnored, "...keySelfIgnored is set for the readout");
        check (allDegrees(), "...the scale reads chromatic");
        check (std::abs (st.refApplied - 440.0) < 0.1, "...reference stays 440");
        check (! (p.autoKeyState().root == 6 && p.autoKeyState().applied),
               "...the PREVIOUS key (F# minor) did not survive - chromatic, not the last key");

        // A self-derived fact from ANOTHER instance (a bus Link's own
        // analysis) is legitimate and still followed.
        publishFact (9, false, 0.9f, 441.0f, "Music Bus (Link)", 78, true);
        pump (4);
        check (p.autoKeyState().applied && p.autoKeyState().root == 9 && ! p.autoKeyState().minor
               && ! p.autoKeyState().keySelfIgnored,
               "a self-derived fact from ANOTHER instance is followed (A major)");

        // An external fact restores key and reference.
        publishFact (6, true, 0.86f, 441.3f, "Music Bus", 12345, false);
        pump (4);
        check (p.autoKeyState().applied && p.autoKeyState().root == 6 && p.autoKeyState().minor
               && std::abs (p.autoKeyState().refApplied - 441.3) < 0.1,
               "an external fact restores key (F# minor) and reference (441.3)");
        p.debugKeySelfGuard (false);
        p.setKeyFeedSelfId (0);
        publishFact (6, true, 0.86f, 441.3f, "Music Bus", 12345, false);
        pump (4);
    }

    std::printf ("== KEY-SIDE GUARD, RENDER BIT-IDENTITY on the standing take (bar item 2) ==\n");
    {
        // EJ_PITCH_SOURCE or the standing reference path; skipped, not
        // failed, when the material is absent (the repo does not carry it).
        const char* env = std::getenv ("EJ_PITCH_SOURCE");
        juce::File src (env != nullptr ? juce::String (env)
                                       : juce::String ("/Users/SeanD/Music/Logic/test/Bounces/sourceNEW.wav"));
        juce::AudioBuffer<float> take;
        double fs = 48000.0;
        if (src.existsAsFile())
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (src.createInputStream().release(), true));
            if (r != nullptr)
            {
                fs = r->sampleRate;
                take.setSize (1, (int) r->lengthInSamples);
                r->read (&take, 0, (int) r->lengthInSamples, 0, true, false);
            }
        }
        if (take.getNumSamples() == 0)
            std::printf ("  [SKIP] material not found (%s) - render identity not measured\n", src.getFullPathName().toRawUTF8());
        else
        {
            auto render = [&] (const std::function<void (EedPitchProcessor&)>& setup) -> std::vector<float>
            {
                EedPitchProcessor q;
                q.prepareToPlay (fs, 512);
                q.applyStructured (params ({ { "correction_mode", "hard" } }));
                setup (q);
                std::vector<float> out; out.reserve ((size_t) take.getNumSamples());
                juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
                for (int pos = 0; pos < take.getNumSamples(); pos += 512)
                {
                    const int n = juce::jmin (512, take.getNumSamples() - pos);
                    b.clear();
                    b.copyFrom (0, 0, take, 0, pos, n);
                    b.copyFrom (1, 0, take, 0, pos, n);
                    q.processBlock (b, m);
                    for (int i = 0; i < n; ++i) out.push_back (b.getSample (0, i));
                }
                return out;
            };
            auto selfFact = [] (uint64_t publisher, bool selfDerived)
            {
                echojay::DetectedKeyFact f;
                f.valid = true; f.root = 7; f.minor = false; f.confidence = 0.86f;   // G major: F -> F#, the damaging case
                f.tuningHz = 440.0f; f.fromBus = true; f.publisherId = publisher; f.selfDerived = selfDerived;
                std::strncpy (f.sourceName, "this channel", sizeof (f.sourceName) - 1);
                echojay::KeyFeed::instance().publish (f);
            };
            auto identical = [] (const std::vector<float>& a, const std::vector<float>& b)
            { return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0; };
            auto differing = [] (const std::vector<float>& a, const std::vector<float>& b)
            { size_t n = 0; for (size_t i = 0; i < std::min (a.size(), b.size()); ++i) if (a[i] != b[i]) ++n; return n; };

            // (a) guard ON + self-derived self fact  ==  manual chromatic
            const auto guarded = render ([&] (EedPitchProcessor& q)
            { q.setKeyFeedSelfId (77); q.debugKeySelfGuard (true); selfFact (77, true);
              q.applyStructured (params ({ { "key_source", "auto" } })); });
            const auto chromatic = render ([&] (EedPitchProcessor& q)
            { q.setKeyFeedSelfId (0); echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact {});
              q.applyStructured (params ({ { "scale", "chromatic" } })); });
            check (identical (guarded, chromatic),
                   "guard ON + self-derived self fact renders BIT-IDENTICAL to manual chromatic ("
                   + juce::String ((int) differing (guarded, chromatic)) + " samples differ)");
            // The positive control: with the guard OFF the same self fact
            // applies G major and the render MUST differ (F -> F#).
            const auto unguarded = render ([&] (EedPitchProcessor& q)
            { q.setKeyFeedSelfId (77); q.debugKeySelfGuard (false); selfFact (77, true);
              q.applyStructured (params ({ { "key_source", "auto" } })); });
            check (! identical (unguarded, chromatic),
                   "POSITIVE CONTROL: guard OFF applies the self-derived G major and differs from chromatic ("
                   + juce::String ((int) differing (unguarded, chromatic)) + " samples differ)");
            // (b) guard ON vs OFF under an EXTERNAL fact: bit-identical (no change to the followed path)
            const auto extOn = render ([&] (EedPitchProcessor& q)
            { q.setKeyFeedSelfId (77); q.debugKeySelfGuard (true); selfFact (12345, false);
              q.applyStructured (params ({ { "key_source", "auto" } })); });
            const auto extOff = render ([&] (EedPitchProcessor& q)
            { q.setKeyFeedSelfId (77); q.debugKeySelfGuard (false); selfFact (12345, false);
              q.applyStructured (params ({ { "key_source", "auto" } })); });
            check (identical (extOn, extOff),
                   "guard ON vs OFF under an EXTERNAL fact: bit-identical ("
                   + juce::String ((int) differing (extOn, extOff)) + " samples differ)");
            echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact {});
        }
    }

    std::printf ("== SEAN'S SAVED STATE (3 Sep 2026): what is DISPLAYED vs what is APPLIED ==\n");
    {
        // The pitch device's slot state exactly as decoded from his Logic
        // project (DEFECT_AUTOKEY_PROVENANCE.md §15): reference_source auto,
        // reference_hz 439.19 persisted from the pre-guard era.
        static const char* kSeanState =
            "{\"v\": 1, \"bypassed\": false, \"params\": {\"correction_mode\": 4.0, \"correct\": 1.0, "
            "\"retune_speed_ms\": 44.211582183837891, \"flex\": 0.0, \"humanize\": 0.0, "
            "\"targeting_ignores_vibrato\": 0.0, \"key_source\": 1.0, \"key_root\": 2.0, \"scale\": 1.0, "
            "\"reference_source\": 0.0, \"reference_hz\": 439.19219970703125, \"ref_manual_by_user\": 0.0, "
            "\"transpose\": 0.0, \"natural_vibrato\": 0.0, \"vib_depth_cents\": 0.0, \"vib_rate_hz\": 5.5, "
            "\"vib_shape\": 0.0, \"vib_onset_ms\": 300.0, \"voice_type\": 1.0, \"tracking\": 1.0, "
            "\"formant_mode\": 1.0, \"formant_shift\": 0.0, \"low_latency\": 0.0, \"mix\": 100.0, "
            "\"output_db\": 0.0, \"target_hz\": 0.0, \"reset_stats\": 0.0}}";
        EedPitchProcessor q;
        q.prepareToPlay (48000.0, 512);
        q.setKeyFeedSelfId (77);
        echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact {});   // his topology: no external fact
        q.setStateInformation (kSeanState, (int) std::strlen (kSeanState));
        auto pump = [&] (int blocks)
        { juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; for (int i = 0; i < blocks; ++i) { b.clear(); q.processBlock (b, m); } };
        pump (4);
        auto st = q.autoKeyState();
        std::printf ("    loaded: key_source %s, reference_source %s, reference_hz FIELD %.2f, APPLIED %.2f, seam_attack_ms %.0f, retune %.2f\n",
                     q.getParamValue ("key_source") < 0.5 ? "auto" : "manual", st.refAuto ? "auto" : "manual",
                     q.getParamValue ("reference_hz"), st.refApplied, q.getParamValue ("seam_attack_ms"), q.getParamValue ("retune_speed_ms"));
        check (! st.active, "key is MANUAL (not on the auto path)");
        check (st.refAuto, "reference is AUTO");
        check (std::abs (st.refApplied - 440.0) < 0.05, "APPLIED reference under auto with no source is 440.0");
        check (std::abs (q.getParamValue ("reference_hz") - 439.19) < 0.01,
               "...while the reference_hz FIELD (the REF knob) still reads the persisted 439.19 - DORMANT contamination");
        check (std::abs (q.getParamValue ("seam_attack_ms") - 60.0) < 0.01, "seam_attack_ms absent from the state -> schema default 60 applied");
        // FullMix role on a vocal: his own KeyEngine publishes a self-derived fact
        {
            echojay::DetectedKeyFact f; f.valid = true; f.root = 0; f.minor = false; f.confidence = 0.86f;
            f.tuningHz = 439.19f; f.fromBus = true; f.publisherId = 77; f.selfDerived = true;
            std::strncpy (f.sourceName, "this channel", sizeof (f.sourceName) - 1);
            echojay::KeyFeed::instance().publish (f);
        }
        pump (4); st = q.autoKeyState();
        check (std::abs (st.refApplied - 440.0) < 0.05 && st.refSelfIgnored,
               "a self-derived 439.19 fact from his own channel is refused: applied stays 440 (refSelfIgnored)");
        check (! st.active, "...and his MANUAL key is untouched by the key guard (not on the auto path)");
        // THE DORMANT VALUE WAKES UP: switching reference to manual applies the field.
        echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact {});
        q.applyStructured (params ({ { "reference_source", "manual" } }));
        pump (4); st = q.autoKeyState();
        std::printf ("    after reference_source -> manual: APPLIED %.2f\n", st.refApplied);
        check (std::abs (st.refApplied - 439.19) < 0.01,
               "switching reference_source to manual APPLIES the persisted 439.19 - the guard never fires on a loaded value");
        echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact {});
    }

    std::printf ("== THE RETUNE DIAL (round 46): the curve, the default, off-curve, saved-state semantics ==\n");
    {
        // Bar leg 6: a fresh instance is at dial 0 = (6 ms, depth 100).
        {
            EedPitchProcessor q; q.prepareToPlay (48000.0, 512);
            check (std::abs (q.getParamValue ("retune")) < 1.0e-6, "fresh instance: retune dial 0");
            check (std::abs (q.getParamValue ("retune_speed_ms") - 6.0) < 1.0e-4 && std::abs (q.getParamValue ("depth") - 100.0) < 1.0e-4,
                   "fresh instance: retune_speed_ms 6 / depth 100 (the round-40 default)");
            check (! q.retuneOffCurve(), "fresh instance is ON the curve");
        }
        // Bar leg 2 (the pairs): the shipped mapping equals the round-40 v3 rows
        // (transfer_tf4_2026-09-05.txt) at the 18 measured positions, to the
        // precision those rows were run at (retune %.1f ms, depth %.3f).
        {
            struct Row { double dial, ms, depth; };
            static const Row kTf4[] = {
                { 0, 6.0, 1.000 }, { 2, 6.1, 0.932 }, { 5, 6.7, 0.870 }, { 10, 9.0, 0.789 }, { 15, 12.7, 0.720 },
                { 20, 17.8, 0.658 }, { 25, 24.5, 0.600 }, { 30, 32.6, 0.545 }, { 35, 42.3, 0.494 }, { 40, 53.4, 0.444 },
                { 45, 65.9, 0.396 }, { 50, 80.0, 0.350 }, { 75, 115.0, 0.300 }, { 100, 150.0, 0.250 }, { 150, 150.0, 0.200 },
                { 200, 150.0, 0.150 }, { 300, 150.0, 0.125 }, { 400, 150.0, 0.100 } };
            EedPitchProcessor q; q.prepareToPlay (48000.0, 512);
            bool all = true;
            for (const auto& r : kTf4)
            {
                q.setParamValue ("retune", r.dial);
                const double ms = q.getParamValue ("retune_speed_ms"), dp = q.getParamValue ("depth") * 0.01;
                const bool ok = std::abs (ms - r.ms) <= 0.05 + 1.0e-9 && std::abs (dp - r.depth) <= 0.0005 + 1.0e-9 && ! q.retuneOffCurve();
                std::printf ("    dial %3.0f -> retune %7.3f ms  depth %.4f   (tf4 row %5.1f / %.3f) %s\n", r.dial, ms, dp, r.ms, r.depth, ok ? "" : "MISMATCH");
                all = all && ok;
            }
            check (all, "the shipped mapping reproduces the 18 round-40 (retune, depth) pairs within the rows' printed precision, on-curve at every one");
        }
        // Bar leg 5: DEPTH (and retune_speed_ms) as the override - off the curve, and back.
        {
            EedPitchProcessor q; q.prepareToPlay (48000.0, 512);
            q.setParamValue ("retune", 100.0);
            check (std::abs (q.getParamValue ("retune_speed_ms") - 150.0) < 1.0e-3 && std::abs (q.getParamValue ("depth") - 25.0) < 1.0e-2 && ! q.retuneOffCurve(),
                   "dial 100 -> 150 ms / depth 25, on the curve");
            q.setParamValue ("depth", 50.0);
            check (q.retuneOffCurve() && std::abs (q.getParamValue ("retune") - 100.0) < 1.0e-6 && std::abs (q.getParamValue ("depth") - 50.0) < 1.0e-3,
                   "DEPTH turned by hand to 50: OFF the curve, the dial still reads 100, depth is what was set");
            q.setParamValue ("retune", 100.0);
            check (! q.retuneOffCurve() && std::abs (q.getParamValue ("depth") - 25.0) < 1.0e-2, "turning RETUNE returns to the curve (depth back to 25)");
            q.setParamValue ("retune_speed_ms", 44.0);
            check (q.retuneOffCurve() && std::abs (q.getParamValue ("retune_speed_ms") - 44.0) < 1.0e-4, "a direct retune_speed_ms write (the model's path) is honoured and shown off-curve");
            // A mode writes its own retune/depth: off the curve by construction, dial at the mode's position.
            q.applyStructured (params ({ { "correction_mode", "natural" } }));
            check (q.retuneOffCurve() && std::abs (q.getParamValue ("retune_speed_ms") - 120.0) < 1.0e-4 && std::abs (q.getParamValue ("retune") - 78.571) < 0.01,
                   "mode natural: retune 120 / depth 100 as the table says, dial shown at 78.6 (the 120 ms position), off the curve");
            q.setParamValue ("retune", 0.0);
            check (! q.retuneOffCurve() && (int) std::lround (q.getParamValue ("correction_mode")) == 4, "turning the dial from a mode: custom, on the curve");
        }
        // Bar leg 1 (the semantics, before the render proof): an OLD file with no
        // `retune` field loads its own retune_speed_ms/depth literally - off the
        // curve at dial 0 - and a file saved by THIS build round-trips exactly.
        {
            const juce::String old44 =
                "{\"v\": 1, \"bypassed\": false, \"params\": {\"correction_mode\": 4.0, \"correct\": 1.0, "
                "\"retune_speed_ms\": 44.211582183837891, \"flex\": 0.0, \"humanize\": 0.0, \"key_source\": 1.0, \"key_root\": 2.0, \"scale\": 1.0}}";
            EedPitchProcessor q; q.prepareToPlay (48000.0, 512);
            q.setStateInformation (old44.toRawUTF8(), (int) old44.getNumBytesAsUTF8());
            check (std::abs (q.getParamValue ("retune_speed_ms") - 44.211582) < 1.0e-4 && std::abs (q.getParamValue ("depth") - 100.0) < 1.0e-6,
                   "Sean's 3 Sep file (retune 44.21, no depth, no retune field): loads retune_speed_ms 44.21 / depth 100 - NOT reinterpreted");
            check (q.retuneOffCurve() && std::abs (q.getParamValue ("retune")) < 1.0e-6, "...shown OFF the curve at dial 0 (the readout explains)");
            const juce::String old6 = old44.replace ("44.211582183837891", "6.0");
            q.setStateInformation (old6.toRawUTF8(), (int) old6.getNumBytesAsUTF8());
            check (! q.retuneOffCurve() && std::abs (q.getParamValue ("retune_speed_ms") - 6.0) < 1.0e-6, "his session at retune 6 / depth 100: loads ON the curve at dial 0");
            // Round trip from this build: dial 200 (on curve), and dial 200 + depth override (off curve) both survive a save/load.
            q.setParamValue ("retune", 200.0);
            juce::MemoryBlock st; q.getStateInformation (st);
            EedPitchProcessor r; r.prepareToPlay (48000.0, 512); r.setStateInformation (st.getData(), (int) st.getSize());
            check (std::abs (r.getParamValue ("retune") - 200.0) < 1.0e-6 && std::abs (r.getParamValue ("retune_speed_ms") - 150.0) < 1.0e-3
                   && std::abs (r.getParamValue ("depth") - 15.0) < 1.0e-2 && ! r.retuneOffCurve(),
                   "save/load at dial 200: retune 200, 150 ms / depth 15, on the curve");
            q.setParamValue ("depth", 60.0);
            q.getStateInformation (st);
            EedPitchProcessor r2; r2.prepareToPlay (48000.0, 512); r2.setStateInformation (st.getData(), (int) st.getSize());
            check (std::abs (r2.getParamValue ("retune") - 200.0) < 1.0e-6 && std::abs (r2.getParamValue ("depth") - 60.0) < 1.0e-3 && r2.retuneOffCurve(),
                   "save/load at dial 200 with DEPTH overridden to 60: the override survives the file and is shown off-curve");
            // The schema order that makes this work: `retune` is listed before retune_speed_ms and depth.
            int iR = -1, iMs = -1, iD = -1, n = 0;
            for (const auto& sp : EedPitchProcessor::schema().params())
            { if (sp.id == "retune") iR = n; if (sp.id == "retune_speed_ms") iMs = n; if (sp.id == "depth") iD = n; ++n; }
            check (iR >= 0 && iR < iMs && iR < iD, "schema order: `retune` precedes retune_speed_ms and depth, so a saved file applies the dial first and the literals after");
        }
    }

    // ---- SAVED-STATE RENDER IDENTITY (5 Sep 2026, UI_SIMPLIFICATION round 46, bar leg 1):
    // EJ_STATE_RENDER_OUT=<dir>: renders the standing take (EJ_PITCH_SOURCE
    // overrides) through saved states exactly as a host would load them. The
    // FIRST run (the pre-change binary) writes <dir>/<name>.wav; every later
    // run COMPARES against those files bit for bit. This is how "nothing
    // already saved is reinterpreted" is proved rather than asserted.
    if (const char* sdir = std::getenv ("EJ_STATE_RENDER_OUT"))
    {
        std::printf ("== SAVED-STATE RENDER IDENTITY -> %s ==\n", sdir);
        const char* env = std::getenv ("EJ_PITCH_SOURCE");
        juce::File src (env != nullptr ? juce::String (env) : juce::String ("/Users/SeanD/Music/Logic/test/Bounces/sourceNEW.wav"));
        juce::AudioBuffer<float> take; double fs = 48000.0;
        if (src.existsAsFile())
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (src.createInputStream().release(), true));
            if (r != nullptr) { fs = r->sampleRate; take.setSize (1, (int) r->lengthInSamples); r->read (&take, 0, (int) r->lengthInSamples, 0, true, false); }
        }
        if (take.getNumSamples() == 0) std::printf ("  [SKIP] material not found\n");
        else
        {
            juce::File out (sdir); out.createDirectory();
            // Sean's slot state as decoded from his Logic project (3 Sep 2026), verbatim.
            const juce::String sean44 =
                "{\"v\": 1, \"bypassed\": false, \"params\": {\"correction_mode\": 4.0, \"correct\": 1.0, "
                "\"retune_speed_ms\": 44.211582183837891, \"flex\": 0.0, \"humanize\": 0.0, "
                "\"targeting_ignores_vibrato\": 0.0, \"key_source\": 1.0, \"key_root\": 2.0, \"scale\": 1.0, "
                "\"reference_source\": 0.0, \"reference_hz\": 439.19219970703125, \"ref_manual_by_user\": 0.0, "
                "\"transpose\": 0.0, \"natural_vibrato\": 0.0, \"vib_depth_cents\": 0.0, \"vib_rate_hz\": 5.5, "
                "\"vib_shape\": 0.0, \"vib_onset_ms\": 300.0, \"voice_type\": 1.0, \"tracking\": 1.0, "
                "\"formant_mode\": 1.0, \"formant_shift\": 0.0, \"low_latency\": 0.0, \"mix\": 100.0, "
                "\"output_db\": 0.0, \"target_hz\": 0.0, \"reset_stats\": 0.0}}";
            const juce::String sean6  = sean44.replace ("44.211582183837891", "6.0");   // his session at the round-40 default
            const juce::String sean6d = sean6.replace ("\"flex\": 0.0", "\"depth\": 100.0, \"seam_attack_ms\": 60.0, \"flex\": 0.0");   // as today's build saves it
            struct Case { const char* name; juce::String state; };
            const Case cases[] = { { "sean_retune44", sean44 }, { "sean_retune6", sean6 }, { "sean_retune6_depth100", sean6d }, { "fresh_default", {} } };
            for (const auto& c : cases)
            {
                EedPitchProcessor q; q.prepareToPlay (fs, 512);
                q.setKeyFeedSelfId (77);
                echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact {});
                if (c.state.isNotEmpty()) q.setStateInformation (c.state.toRawUTF8(), (int) c.state.getNumBytesAsUTF8());
                juce::AudioBuffer<float> o (1, take.getNumSamples());
                juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
                for (int pos = 0; pos < take.getNumSamples(); pos += 512)
                {
                    const int n = juce::jmin (512, take.getNumSamples() - pos);
                    b.clear(); b.copyFrom (0, 0, take, 0, pos, n); b.copyFrom (1, 0, take, 0, pos, n);
                    q.processBlock (b, m);
                    o.copyFrom (0, pos, b, 0, 0, n);
                }
                const juce::String applied = "retune_speed_ms " + juce::String (q.getParamValue ("retune_speed_ms"), 2)
                                           + " depth " + juce::String (q.getParamValue ("depth"), 1);
                juce::File f = out.getChildFile (juce::String (c.name) + ".wav");
                juce::WavAudioFormat wav;
                if (f.existsAsFile())
                {
                    std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (f.createInputStream().release(), true));
                    juce::AudioBuffer<float> ref;
                    if (r != nullptr) { ref.setSize (1, (int) r->lengthInSamples); r->read (&ref, 0, (int) r->lengthInSamples, 0, true, false); }
                    size_t diff = 0;
                    const int n = juce::jmin (ref.getNumSamples(), o.getNumSamples());
                    for (int i = 0; i < n; ++i) if (ref.getSample (0, i) != o.getSample (0, i)) ++diff;
                    const bool same = ref.getNumSamples() == o.getNumSamples() && diff == 0;
                    check (same, juce::String (c.name) + " renders BIT-IDENTICAL to the reference render (" + juce::String ((int) diff)
                                 + " samples differ, " + juce::String (ref.getNumSamples()) + " vs " + juce::String (o.getNumSamples()) + "); applied " + applied);
                }
                else
                {
                    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), fs, 1, 32, {}, 0));
                    if (w != nullptr) { w->writeFromAudioSampleBuffer (o, 0, o.getNumSamples()); w.reset(); }
                    std::printf ("  wrote reference %s (%d samples); applied %s\n", f.getFileName().toRawUTF8(), o.getNumSamples(), applied.toRawUTF8());
                }
            }
        }
    }

    // ---- PARAMETER VERIFICATION RENDERS (5 Sep 2026, UI_SIMPLIFICATION ruling A):
    // A PARAMETER'S DOCUMENTED BEHAVIOUR IS A CLAIM, NOT A FACT, UNTIL A RENDER
    // SHOWS IT. Gated by EJ_VERIFY_OUT=<dir>: renders the standing take through
    // a FRESH processor at the schema defaults, then once per (param, value) -
    // min / mid / max / default for numerics, every choice for choice params,
    // both states for booleans - through the REAL setParamValue path (the path
    // a knob takes). Offline rulers then say whether the renders differ and in
    // which direction. natural_vibrato was the param whose description was
    // fiction; this is the method that caught it, applied to all of them.
    if (const char* vdir = std::getenv ("EJ_VERIFY_OUT"))
    {
        std::printf ("== PARAMETER VERIFICATION RENDERS -> %s ==\n", vdir);
        const char* env = std::getenv ("EJ_PITCH_SOURCE");
        juce::File src (env != nullptr ? juce::String (env) : juce::String ("/Users/SeanD/Music/Logic/test/Bounces/sourceNEW.wav"));
        juce::AudioBuffer<float> take; double fs = 48000.0;
        if (src.existsAsFile())
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (src.createInputStream().release(), true));
            if (r != nullptr) { fs = r->sampleRate; take.setSize (1, (int) r->lengthInSamples); r->read (&take, 0, (int) r->lengthInSamples, 0, true, false); }
        }
        if (take.getNumSamples() == 0) std::printf ("  [SKIP] material not found\n");
        else
        {
            juce::File out (vdir); out.createDirectory();
            auto renderTo = [&] (const juce::String& name, const std::function<void (EedPitchProcessor&)>& setup)
            {
                EedPitchProcessor q; q.prepareToPlay (fs, 512);
                q.applyStructured (params ({ { "key_source", "manual" }, { "key_root", 2.0 }, { "scale", 1.0 } }));   // D minor by hand: the take's key, so tuning params act
                setup (q);
                juce::AudioBuffer<float> o (1, take.getNumSamples());
                juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
                for (int pos = 0; pos < take.getNumSamples(); pos += 512)
                {
                    const int n = juce::jmin (512, take.getNumSamples() - pos);
                    b.clear(); b.copyFrom (0, 0, take, 0, pos, n); b.copyFrom (1, 0, take, 0, pos, n);
                    q.processBlock (b, m);
                    o.copyFrom (0, pos, b, 0, 0, n);
                }
                juce::File f = out.getChildFile (name + ".wav"); f.deleteFile();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), fs, 1, 32, {}, 0));
                if (w != nullptr) { w->writeFromAudioSampleBuffer (o, 0, o.getNumSamples()); w.reset(); }
                std::printf ("  wrote %s\n", f.getFileName().toRawUTF8());
            };
            renderTo ("default", [] (EedPitchProcessor&) {});
            for (const auto& sp : EedPitchProcessor::schema().params())
            {
                const juce::String id (sp.id);
                if (id == "reset_stats" || id == "ref_manual_by_user" || id == "target_hz") continue;   // momentary / provenance flag / P1 diagnostic
                std::vector<double> values;
                if (! sp.choices.empty()) { for (size_t i = 0; i < sp.choices.size(); ++i) values.push_back ((double) i); }
                else if (sp.boolean) { values = { 0.0, 1.0 }; }
                else { values = { sp.min, 0.5 * (sp.min + sp.max), sp.max }; if (std::find (values.begin(), values.end(), sp.def) == values.end()) values.push_back (sp.def); }
                for (double v : values)
                {
                    juce::String label = sp.choices.empty() ? juce::String (v, 2) : juce::String (sp.choiceLabel (v));
                    renderTo (id + "__" + label.replaceCharacter ('/', '-'), [&] (EedPitchProcessor& q)
                    {
                        if (id == "formant_shift") q.setParamValue ("formant_mode", 2.0);            // shift only audible in shift mode
                        if (id == "vib_rate_hz" || id == "vib_shape" || id == "vib_onset_ms") q.setParamValue ("vib_depth_cents", 30.0);   // the generator needs a depth to show rate/shape/onset
                        if (id == "key_source") { echojay::DetectedKeyFact f; f.valid = true; f.root = 7; f.minor = false; f.confidence = 0.9f; f.tuningHz = 440.0f; f.publisherId = 999; std::strncpy (f.sourceName, "Bus", 4); echojay::KeyFeed::instance().publish (f); }
                        q.setParamValue (id, v);
                    });
                    if (id == "key_source") echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact {});
                }
            }
        }
    }

    std::printf ("== below the gate it falls to CHROMATIC, not to the last key ==\n");
    {
        auto pump = [&] (int blocks)
        {
            juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
            for (int i = 0; i < blocks; ++i) { b.clear(); p.processBlock (b, m); }
        };
        // A stale key applied with total confidence is worse than none: the
        // fallback must be chromatic, and the PREVIOUS key must not survive.
        echojay::DetectedKeyFact f;
        f.valid = true; f.root = 6; f.minor = true; f.confidence = 0.31f;
        f.tuningHz = 441.3f;
        std::strncpy (f.sourceName, "Music Bus", 10);
        echojay::KeyFeed::instance().publish (f);
        pump (4);

        const auto* sp = EedPitchProcessor::schema().find ("scale");
        check (sp->choiceLabel (p.getParamValue ("scale")) == "chromatic",
               "a 0.31-confidence key falls back to chromatic");
        for (int s2 = 0; s2 < 12; ++s2)
            if (! p.corrector().degreeEnabled (s2))
            { check (false, "chromatic must allow every degree"); break; }
        const auto st = p.autoKeyState();
        check (st.fellBack && ! st.applied, "the state reports the fallback so the UI can show it");
    }

    std::printf ("== no source at all is also chromatic, not a guess ==\n");
    {
        echojay::KeyFeed::instance().publish (echojay::DetectedKeyFact{});
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
        for (int i = 0; i < 4; ++i) { b.clear(); p.processBlock (b, m); }
        const auto* sp = EedPitchProcessor::schema().find ("scale");
        check (sp->choiceLabel (p.getParamValue ("scale")) == "chromatic",
               "no key detected -> chromatic");
    }

    std::printf ("== setting key or scale BY HAND takes over from auto ==\n");
    {
        p.applyStructured (params ({ { "key_source", "auto" } }));
        p.applyStructured (params ({ { "scale", "dorian" } }));
        const auto* ks = EedPitchProcessor::schema().find ("key_source");
        check (ks->choiceLabel (p.getParamValue ("key_source")) == "manual",
               "a hand-set scale flips key_source to manual");
        // ...and auto must not overwrite it on the next block.
        echojay::DetectedKeyFact f;
        f.valid = true; f.root = 6; f.minor = true; f.confidence = 0.9f;
        echojay::KeyFeed::instance().publish (f);
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
        for (int i = 0; i < 4; ++i) { b.clear(); p.processBlock (b, m); }
        const auto* sp = EedPitchProcessor::schema().find ("scale");
        check (sp->choiceLabel (p.getParamValue ("scale")) == "dorian",
               "the hand-set scale survives the next detected key");
    }

    std::printf ("== a live key change CROSS-FADES rather than switching on a sample ==\n");
    {
        p.applyStructured (params ({ { "key_source", "auto" } }));
        echojay::DetectedKeyFact f;
        f.valid = true; f.root = 0; f.minor = false; f.confidence = 0.9f;
        echojay::KeyFeed::instance().publish (f);
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
        for (int i = 0; i < 40; ++i) { b.clear(); p.processBlock (b, m); }
        check (! p.corrector().scaleCrossfading(), "settled after the first key");

        f.root = 6; f.minor = true;
        echojay::KeyFeed::instance().publish (f);
        b.clear(); p.processBlock (b, m);
        check (p.corrector().scaleCrossfading(),
               "a modulation starts a cross-fade instead of stepping");
        check (p.corrector().scaleCrossfadeProgress() < 0.9f, "...and it is genuinely gradual");
    }

    std::printf ("== the P3-P4 safety rule is stated where the model reads it ==\n");
    {
        // Until the key auto-map exists the model will be putting this in vocal
        // chains with no key information, and guessing measures WORSE than
        // doing nothing. The warning has to be in the text, with the reason.
        const auto& sc = *EedPitchProcessor::schema().find ("scale");
        const juce::String d (sc.description);
        check (sc.def == 9.0, "scale DEFAULTS to chromatic");
        check (d.containsIgnoreCase ("NEVER GUESS A KEY"), "text forbids guessing a key outright");
        check (d.contains ("13") && d.contains ("29"),
               "text carries the measured cost of guessing (13 -> 29 cents)");
        check (d.containsIgnoreCase ("chromatic is the answer"),
               "text names chromatic as the answer when the key is unknown");
    }

    std::printf ("== the two acceptance briefs are discriminable from the text ==\n");
    {
        const auto& m = *EedPitchProcessor::schema().find ("correction_mode");
        const juce::String d (m.description);
        check (d.containsIgnoreCase ("obviously tuned"),
               "spec 10.5's exact brief appears, attached to hard");
        const int hardAt = d.indexOfIgnoreCase ("obviously tuned");
        const int natAt  = d.indexOfIgnoreCase ("keep it sounding natural");
        check (natAt >= 0, "spec 10.6's brief appears, attached to natural");
        check (d.indexOfIgnoreCase ("reach for hard") < hardAt + 200
                 && d.indexOfIgnoreCase ("reach for hard") > 0,
               "'obviously tuned' sits next to the word hard, not floating");
        check (d.containsIgnoreCase ("natural is transparent"),
               "natural is described by what it SOUNDS like, not by its numbers");
    }

    std::printf ("== pitch_scale MERGE semantics are stated, since eq_bands replaces ==\n");
    {
        // A model that has to guess which shape it is getting will eventually
        // guess wrong, and the failure is silent either way.
        const auto& sc = *EedPitchProcessor::schema().find ("scale");
        (void) sc;
        const juce::String summary (BuiltinDeviceRegistry::instance()
                                      .findByName ("EchoJay Pitch")->summary);
        check (summary.containsIgnoreCase ("pitch_scale"),
               "the advertisement mentions the pitch_scale move at all");
        check (summary.containsIgnoreCase ("merge") || summary.containsIgnoreCase ("omitted"),
               "...and states its MERGE semantics");
    }

    std::printf ("== the correction_mode table is COMPLETE: every param is either "
                 "written by every mode, or exempted here with a reason ==\n");
    {
        // WHY THIS TEST EXISTS. natural_vibrato was added at P5 without a
        // column in the mode table, and hard tune silently re-added the full
        // wobble on top of the snapped note - it measured WORSE than dry
        // (15.7 against 13.0 cents) while the whole suite stayed green
        // (PITCH_P0_VALIDATION.md §12). A mode is only as honest as its
        // table is complete, and completeness must fail the build, not wait
        // for a re-measure.
        //
        // THE RULE. Walk the schema. Every param must be either
        //   (a) DETERMINED by selecting a mode - proven behaviourally: set
        //       the param to each end of its range, select the mode, and the
        //       readback must land on the same value both times - or
        //   (b) listed below as NOT character-bearing, WITH the reason.
        // A new param lands in neither and fails, which forces the author to
        // answer "does this shape the corrected sound's character?" at the
        // moment the param is added, not at the next listening session.
        struct Exempt { const char* id; const char* why; };
        static const Exempt kNotCharacter[] = {
            { "correct",          "the master enable; a mode is a character, not an on/off" },
            { "correction_mode",  "the selector itself" },
            { "key_source",       "WHAT to correct to, not how the correction sounds" },
            { "key_root",         "WHAT to correct to" },
            { "scale",            "WHAT to correct to" },
            { "reference_source", "WHAT tuning to correct to" },
            { "reference_hz",     "WHAT tuning to correct to" },
            { "ref_manual_by_user", "internal provenance marker, not character" },
            { "transpose",        "a pitch offset on the result, orthogonal to character" },
            { "voice_type",       "detector fit to the material, not character" },
            { "tracking",         "detector strictness, not character" },
            { "target_hz",        "the P1 fixed-target diagnostic path" },
            { "low_latency",      "a latency trade; changes delay, not character" },
            { "formant_shift",    "a WHO-is-singing control, not a correction character - and "
                                  "inert in every mode anyway, since every mode writes "
                                  "formant_mode preserve (which the walk above proves)" },
            { "mix",              "output stage" },
            { "output_db",        "output stage" },
            { "vib_depth_cents",  "ADDED vibrato is a creative layer the spec's mode table deliberately leaves alone (only natural_vibrato is in it)" },
            { "vib_rate_hz",      "added vibrato, as above" },
            { "vib_shape",        "added vibrato, as above" },
            { "vib_onset_ms",     "added vibrato, as above" },
            { "reset_stats",      "momentary action, not a setting" },
        };
        auto exempt = [&] (const std::string& id)
        {
            for (const auto& e : kNotCharacter) if (id == e.id) return true;
            return false;
        };
        // The exemption list may not rot: every entry must still name a real
        // param, so a rename cannot quietly widen the hole it guards.
        for (const auto& e : kNotCharacter)
            check (EedPitchProcessor::schema().find (e.id) != nullptr,
                   juce::String ("exempt id still exists in the schema: ") + e.id);

        static const char* kModeNames[4] = { "natural", "balanced", "tuned", "hard" };
        for (const auto& sp : EedPitchProcessor::schema().params())
        {
            if (exempt (sp.id)) continue;
            bool determined = true;
            for (int m = 0; m < 4 && determined; ++m)
            {
                p.setParamValue (juce::String (sp.id), sp.min);
                p.setParamValue ("correction_mode", (double) m);
                const double a = p.getParamValue (juce::String (sp.id));
                p.setParamValue (juce::String (sp.id), sp.max);
                p.setParamValue ("correction_mode", (double) m);
                const double b = p.getParamValue (juce::String (sp.id));
                if (std::abs (a - b) > 1.0e-6) determined = false;
                (void) kModeNames[m];
            }
            check (determined,
                   juce::String (sp.id.c_str())
                   + " is character-bearing (not exempted), so every mode must WRITE it"
                     " - if it was just added, either give it a column in applyMode's"
                     " table or exempt it HERE with the reason");
        }
    }

    std::printf ("== UI coverage: every schema param has a hand control, or is "
                 "exempted here with the reason ==\n");
    {
        // WHY THIS EXISTS. `scale` was in the schema, dialable by the model,
        // wired in the editor - and squeezed by the layout to a ~10-pixel
        // sliver, so a user reported the panel "shows KEY only" and the
        // parameter was, in practice, invisible state. The same shape of
        // failure as the constructed-vs-advertised defaults sweep: what the
        // model can set and the user cannot see will eventually surprise
        // someone. (This walk proves LISTING, not pixels - the layout half
        // lives in the editor-paint harness.)
        //
        // The exemption ledger below is deliberately LOUD: every UI-less
        // param is named with its status, and a new param lands in neither
        // list and fails the build.
        struct NoUi { const char* id; const char* why; };
        static const NoUi kNoUi[] = {
            { "ref_manual_by_user", "internal provenance marker, deliberately "
                                    "uncontrolled - see EedPitchProcessor::onStateApplied" },
            { "transpose",        "INTERNAL, unexposed pending DEFECT_TRANSPOSE_OCTAVE (+12 gives 155c in one region; -12 loses 3.7 dB)" },
            { "target_hz",        "INTERNAL: the P1 fixed-target diagnostic path (UI_SIMPLIFICATION inventory)" },
            { "retune_speed_ms",  "INTERNAL since round 46: driven by the RETUNE dial (`retune`); a direct write is the model's/chain's override, shown off-curve" },
            { "reset_stats",      "INTERNAL: a momentary readout reset (UI_SIMPLIFICATION inventory)" },
        };
        auto exemptUi = [&] (const std::string& id)
        {
            for (const auto& e : kNoUi) if (id == e.id) return true;
            return false;
        };
        for (const auto& e : kNoUi)
            check (EedPitchProcessor::schema().find (e.id) != nullptr,
                   juce::String ("UI-exempt id still exists in the schema: ") + e.id);

        const auto& hand = EedPitchEditor::handControlledParams();
        for (const char* id : hand)
            check (EedPitchProcessor::schema().find (id) != nullptr,
                   juce::String ("hand-controlled id still exists in the schema: ") + id);

        for (const auto& sp : EedPitchProcessor::schema().params())
        {
            bool covered = exemptUi (sp.id);
            for (const char* id : hand) if (sp.id == id) covered = true;
            check (covered,
                   juce::String (sp.id.c_str())
                   + " has no hand control and no exemption - add it to the editor's"
                     " handControlledParams() with a control, or to the ledger HERE"
                     " with its status");
        }
    }

    // ---- reference provenance (29 Aug 2026): the laundering defect --------
    // A detected reference must never become a manual setting. The pre-fix
    // machinery saved the corrector's LIVE (detected) reference and the load
    // flipped the mode to manual - a grid nobody chose, with no control to
    // change it. These four lock the repaired contract.
    {
        std::printf ("\nreference provenance:\n");
        auto load = [] (EedPitchProcessor& p,
                        std::initializer_list<std::pair<const char*, juce::var>> kv)
        {
            const juce::String js = juce::JSON::toString (params (kv), true);
            p.setStateInformation (js.toRawUTF8(), (int) js.getNumBytesAsUTF8());
        };
        {
            EedPitchProcessor p;
            load (p, { { "reference_source", 1.0 }, { "reference_hz", 439.2 } });
            check (p.getParamValue (EedPitchProcessor::kRefSource) < 0.5,
                   "laundered state (manual + value, no marker) reverts to AUTO on load");
        }
        {
            EedPitchProcessor p;
            load (p, { { "reference_source", 1.0 }, { "reference_hz", 441.0 },
                       { "ref_manual_by_user", 1.0 } });
            check (p.getParamValue (EedPitchProcessor::kRefSource) >= 0.5,
                   "marked manual state stays MANUAL on load");
            check (std::abs (p.getParamValue (EedPitchProcessor::kReferenceHz) - 441.0) < 0.01,
                   "manual field holds the entered 441");
        }
        {
            EedPitchProcessor p;
            p.setParamValue (EedPitchProcessor::kReferenceHz, 442.0);
            check (p.getParamValue (EedPitchProcessor::kRefSource) >= 0.5,
                   "a LIVE reference write takes manual control");
            juce::MemoryBlock mb; p.getStateInformation (mb);
            EedPitchProcessor q;
            q.setStateInformation (mb.getData(), (int) mb.getSize());
            check (q.getParamValue (EedPitchProcessor::kRefSource) >= 0.5
                   && std::abs (q.getParamValue (EedPitchProcessor::kReferenceHz) - 442.0) < 0.01,
                   "marked manual 442 survives a save/load round-trip");
        }
        {
            EedPitchProcessor p;
            juce::MemoryBlock mb; p.getStateInformation (mb);
            EedPitchProcessor q;
            q.setStateInformation (mb.getData(), (int) mb.getSize());
            check (q.getParamValue (EedPitchProcessor::kRefSource) < 0.5,
                   "auto survives a save/load round-trip (the field saved is the manual 440, never a detected grid)");
        }
    }

    // ---- EDITOR SNAPSHOTS (5 Sep 2026, round 47): EJ_EDITOR_SNAP=<dir> renders
    // the panel offline to PNG - front, front off-curve, advanced - so a layout
    // is LOOKED AT before Sean does (his screenshot found labels sitting under
    // the wrong control). Offscreen: no window, no host.
    if (const char* sdir = std::getenv ("EJ_EDITOR_SNAP"))
    {
        std::printf ("== EDITOR SNAPSHOTS -> %s ==\n", sdir);
        juce::ScopedJuceInitialiser_GUI gui;
        juce::File out (sdir); out.createDirectory();
        auto snap = [&] (const char* name, const std::function<void (EedPitchProcessor&, EedPitchEditor&)>& setup)
        {
            EedPitchProcessor p; p.prepareToPlay (48000.0, 512);
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            auto* pe = dynamic_cast<EedPitchEditor*> (ed.get());
            if (pe == nullptr) { std::printf ("  no editor\n"); return; }
            ed->setSize (620, 400);
            setup (p, *pe);
            { juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; for (int i = 0; i < 4; ++i) { b.clear(); p.processBlock (b, m); } }   // a host runs audio: the auto-key state is a block-thread product
            pe->syncNow();   // what the 30 Hz timer does in a host
            juce::Image img = ed->createComponentSnapshot (ed->getLocalBounds(), false, 2.0f);
            juce::File f = out.getChildFile (juce::String (name) + ".png"); f.deleteFile();
            juce::FileOutputStream os (f);
            juce::PNGImageFormat png; png.writeImageToStream (img, os);
            std::printf ("  wrote %s (%dx%d)\n", f.getFileName().toRawUTF8(), img.getWidth(), img.getHeight());
        };
        snap ("front",          [] (EedPitchProcessor&, EedPitchEditor& e) { e.showAdvanced (false); });
        snap ("front_offcurve", [] (EedPitchProcessor& p, EedPitchEditor& e) { p.setParamValue ("retune", 100.0); p.setParamValue ("depth", 50.0); e.showAdvanced (false); e.repaint(); });
        snap ("advanced",       [] (EedPitchProcessor&, EedPitchEditor& e) { e.showAdvanced (true); });
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
