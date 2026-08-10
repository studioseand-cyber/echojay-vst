// P3: the mode machine and the pitch_scale object move, through the SAME
// applyStructured funnel the chain hands settings to.
#include <JuceHeader.h>
#include "EedPitchProcessor.h"
#include "EedDeviceRegistry.h"
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

    std::printf ("== the P3-P4 safety rule is stated where the model reads it ==\n");
    {
        // Until the key auto-map exists the model will be putting this in vocal
        // chains with no key information, and guessing measures WORSE than
        // doing nothing. The warning has to be in the text, with the reason.
        const auto& sc = *EedPitchProcessor::schema().find ("scale");
        const juce::String d (sc.description);
        check (sc.def == 9.0, "scale DEFAULTS to chromatic");
        check (d.containsIgnoreCase ("chromatic unless"), "text says default to chromatic unless told the key");
        check (d.contains ("13") && d.contains ("29"),
               "text carries the measured cost of guessing (13 -> 29 cents)");
        check (d.containsIgnoreCase ("does not detect the key"),
               "text says plainly that the device cannot detect the key yet");
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
