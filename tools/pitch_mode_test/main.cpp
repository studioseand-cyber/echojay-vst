// P3: the mode machine and the pitch_scale object move, through the SAME
// applyStructured funnel the chain hands settings to.
#include <JuceHeader.h>
#include "EedPitchProcessor.h"
#include "EedDeviceRegistry.h"
#include "EedKeyFeed.h"
#include <cstdio>
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
        check (p.getParamValue ("targeting_ignores_vibrato") == 0.0, "hard wrote ignore_vibrato off");
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
        check (std::abs (p.getParamValue ("reference_hz") - 441.3) < 0.1,
               "reference_hz followed the detected tuning, not dragged to 440");

        const auto st = p.autoKeyState();
        check (st.applied && st.sourceName == "Music Bus",
               "the state names its source for the UI: " + st.sourceName);
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

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
