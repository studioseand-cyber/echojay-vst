// readback_search_guard (21t-j, 28 Sep 2026, B's contract note 3): landing a dB target on a control whose ONLY
// sampled points are "-inf" / -24.0 / 0.0 - the MC 77's "Input L", the control that made this necessary. The map
// has no anchor below -24, so a start around -30 clamps to the middle of the travel; the search asks the plugin
// itself where -30 is. Drives the SAME function the product calls (echojay::searchForDb).
//
// RED on the pre-21t-j tree: EJReadbackSearch.h does not exist, so this does not compile.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJReadbackSearch.h"
#include <cstdio>
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }

// THE MC 77's Input law, from the signed probe's own profile: "-inf dB" at 0.0, "-24.0 dB" at 0.5, "0.0 dB" at
// 1.0. Linear in dB above zero, silent at the very bottom - three sampled points and a continuous control under
// them, which is exactly the case a three-anchor map cannot place a value in.
struct InfDbParam final : juce::AudioProcessorParameter
{
    float getValue() const override { return v; }
    void  setValue (float x) override { v = juce::jlimit (0.0f, 1.0f, x); ++writes; }
    float getDefaultValue() const override { return 0.375f; }   // the plugin opens at -30.0 dB
    juce::String getName (int) const override { return "Input L"; }
    juce::String getLabel() const override { return "dB"; }
    float getValueForText (const juce::String&) const override { return v; }
    juce::String getText (float x, int) const override
    { return x <= 0.0f ? juce::String ("-inf dB") : juce::String (48.0 * ((double) x - 1.0), 1) + " dB"; }
    float v = 0.5f;
    mutable int writes = 0;
};
// ...and a control that prints no number at all, which must be refused rather than searched.
struct NamedParam final : juce::AudioProcessorParameter
{
    float getValue() const override { return v; }
    void  setValue (float x) override { v = x; }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return "Ratio L"; }
    juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String&) const override { return v; }
    juce::String getText (float x, int) const override
    { return x < 0.33f ? "No Buttons" : x < 0.66f ? "Ratio 20" : "All Buttons"; }
    float v = 0.0f;
};
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("readback_search_guard: a dB target on a control sampled only at -inf / -24 / 0\n");

    {
        InfDbParam p;
        std::printf ("    the control reads: 0.0 -> \"%s\"   0.5 -> \"%s\"   1.0 -> \"%s\"\n",
                     p.getText (0.0f, 32).toRawUTF8(), p.getText (0.5f, 32).toRawUTF8(), p.getText (1.0f, 32).toRawUTF8());
        const int writesBefore = p.writes;
        const auto r = echojay::searchForDb (p, -30.0f);
        check (r.refusal.isEmpty(), "the search ran", r.refusal);
        check (p.writes == writesBefore,
               "THE SEARCH WRITES NOTHING: every probe is getText(norm), and the parameter is not moved until the "
               "caller places it", juce::String (p.writes - writesBefore) + " write(s) during the search");
        check (r.landed && std::abs (r.landedDb - (-30.0f)) <= 0.5f,
               "a target of -30 dB lands within 0.5 dB on a control whose only sampled points are -inf / -24 / 0  "
               "(RED as it stood: the map clamped to its lowest anchor and the control sat at -24)",
               "reads " + juce::String (r.landedDb, 2) + " dB at position " + juce::String (r.position, 4));
        check (std::abs (r.position - 0.375f) <= 0.02f,
               "...at the position the control's own law puts it", juce::String (r.position, 4));
    }
    {   // the ends still land, and -24 (the one anchor a map HAS) is not made worse by the search
        InfDbParam p;
        const auto mid = echojay::searchForDb (p, -24.0f);
        check (mid.landed && std::abs (mid.landedDb + 24.0f) <= 0.5f,
               "-24 still lands, at the midpoint the profile sampled",
               juce::String (mid.landedDb, 2) + " dB at " + juce::String (mid.position, 3));
        const auto top = echojay::searchForDb (p, 0.0f);
        check (top.landed && std::abs (top.landedDb) <= 0.5f, "...and 0 dB lands at the top",
               juce::String (top.landedDb, 2) + " dB at " + juce::String (top.position, 3));
        const auto low = echojay::searchForDb (p, -60.0f);
        check (! low.landed && low.refusal.isEmpty(),
               "a target BELOW everything the control can print does not land, and says so rather than pretending",
               "nearest " + juce::String (low.landedDb, 2) + " dB");
    }
    {   // a control that prints words is refused, not searched
        NamedParam n;
        const auto r = echojay::searchForDb (n, -30.0f);
        check (! r.landed && r.refusal.isNotEmpty(),
               "a control whose text is not a dB number is REFUSED, never searched", r.refusal);
    }
    {   // "-inf" is an absence, not a very small number
        double db = 0.0;
        check (echojay::parseDisplayDb ("-inf dB", db) && db <= -1.0e8,
               "\"-inf dB\" parses as an absence, not as a reading", juce::String (db, 0));
        check (echojay::parseDisplayDb ("-30.2 dB", db) && std::abs (db + 30.2) < 0.001,
               "...and a normal reading parses as itself", juce::String (db, 2));
        check (! echojay::parseDisplayDb ("All Buttons", db), "...and a word parses as nothing");
    }
    std::printf ("\n==== readback_search_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
