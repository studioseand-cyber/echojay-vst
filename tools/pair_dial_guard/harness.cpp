// pair_dial_guard (18e item 7, 19 Sep 2026): a map carrying L/R (or 1/2, A/B) PAIRS and settings naming ONE side dial BOTH
// sides to the same value, so a stereo pair is never half-dialled and never lands on the "by hand" list. The Manley
// fixture: UAD Manley Variable Mu carries "L Attack"/"R Attack", "L Output"/"R Output" (param_maps.json on this Mac);
// the model wrote "L Attack" only. A mock AudioPluginInstance with those two parameters stands in for the plugin.
// RED on the pre-round lib/headers: R Attack stays where it was.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayParamApply.h"
#include <cstdio>
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
struct MockParam final : juce::HostedAudioProcessorParameter
{
    explicit MockParam (const juce::String& n) : nm (n) {}
    juce::String getParameterID() const override { return nm.replace (" ", "_"); }
    float getValue() const override { return v; }
    void setValue (float x) override { v = juce::jlimit (0.0f, 1.0f, x); }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return nm; }
    juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String& t) const override { return t.getFloatValue(); }
    juce::String getText (float x, int) const override { return juce::String (x, 3); }
    juce::String nm; float v = 0.0f;
};
struct MockManley final : juce::AudioPluginInstance
{
    MockManley() { for (const char* n : { "L Attack", "R Attack", "L Output", "R Output", "Mix" }) addHostedParameter (std::make_unique<MockParam> (n)); }
    const juce::String getName() const override { return "UAD Manley Variable Mu"; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = getName(); }
    void prepareToPlay (double, int) override {} void releaseResources() override {} void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};
juce::var entry (const char* name, int index, double lo, double hi, const char* unit)
{
    auto* e = new juce::DynamicObject(); e->setProperty ("name", name); e->setProperty ("index", index); e->setProperty ("kind", "anchored"); e->setProperty ("unit", unit); e->setProperty ("trust", "setread");
    juce::Array<juce::var> range; range.add (lo); range.add (hi); e->setProperty ("range", range);
    juce::Array<juce::var> anchors; for (int k = 0; k <= 20; ++k) { const double t = k / 20.0; const double v = lo + (hi - lo) * (0.85 * t + 0.15 * t * t); juce::Array<juce::var> a; a.add (v); a.add (t); anchors.add (juce::var (a)); } e->setProperty ("anchors", anchors);   // a gently tapered table (not a stepped grid)
    return juce::var (e);
}
} // namespace
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("pair_dial_guard: the Manley fixture - settings name \"L Attack\" only; the map has L/R pairs\n");
    MockManley plug;
    auto* controls = new juce::DynamicObject();
    controls->setProperty ("L Attack", entry ("L Attack", 0, 1.0, 30.0, "ms")); controls->setProperty ("R Attack", entry ("R Attack", 1, 1.0, 30.0, "ms"));
    controls->setProperty ("L Output", entry ("L Output", 2, -20.0, 20.0, "db")); controls->setProperty ("R Output", entry ("R Output", 3, -20.0, 20.0, "db"));
    controls->setProperty ("Mix", entry ("Mix", 4, 0.0, 100.0, "pct"));
    auto* map = new juce::DynamicObject(); map->setProperty ("plugin", "UAD Manley Variable Mu"); map->setProperty ("controls", juce::var (controls));
    const juce::var mapVar (map);   // ONE var owns the object (a second juce::var(map) would double-own it: the 18 Sep UAF)
    auto* req = new juce::DynamicObject(); req->setProperty ("L Attack", 30.0); req->setProperty ("L Output", 20.0); req->setProperty ("Mix", 50.0);
    auto* settings = new juce::DynamicObject(); settings->setProperty ("controls", juce::var (req));
    const auto results = echojay::applySettings (plug, mapVar, juce::var (settings));
    auto& ps = plug.getParameters();
    check (std::abs (ps[0]->getValue() - 1.0f) < 0.02f, "L Attack 30 ms -> parameter 0 at 1.0 (the side the model named)", juce::String (ps[0]->getValue(), 3));
    check (std::abs (ps[1]->getValue() - 1.0f) < 0.02f, "R Attack follows to 1.0 - the PAIR is dialled from the one side named (RED before 18e: stays 0)", juce::String (ps[1]->getValue(), 3));
    check (std::abs (ps[2]->getValue() - 1.0f) < 0.02f && std::abs (ps[3]->getValue() - 1.0f) < 0.02f, "L Output +20 (top of the table) -> both Output sides at 1.0", juce::String (ps[2]->getValue(), 3) + " / " + juce::String (ps[3]->getValue(), 3));
    check (std::abs (ps[4]->getValue() - 0.537f) < 0.02f, "Mix 50 (no pair) applies alone (0.537 on this tapered table)", juce::String (ps[4]->getValue(), 3));
    juce::StringArray names, notes; for (const auto& r : results) { names.add (r.semantic); notes.add (r.semantic + ": " + r.note); }
    std::printf ("  results: %s\n", notes.joinIntoString (" | ").toRawUTF8());
    check (names.contains ("R Attack") && names.contains ("R Output"), "the results list the mirrored sides (so the bubble counts them as dialled, never \"by hand\")", names.joinIntoString (", "));
    bool anyByHand = false; for (const auto& r : results) if (r.note.containsIgnoreCase ("by hand") || r.note.containsIgnoreCase ("hand-dial")) anyByHand = true;
    check (! anyByHand, "no result says \"by hand\" for a paired control", names.joinIntoString (", "));
    // an explicitly written R side is NOT overwritten by the L side's value
    auto* req2 = new juce::DynamicObject(); req2->setProperty ("L Attack", 30.0); req2->setProperty ("R Attack", 1.0);
    auto* settings2 = new juce::DynamicObject(); settings2->setProperty ("controls", juce::var (req2));
    const auto res2 = echojay::applySettings (plug, mapVar, juce::var (settings2));
    { juce::StringArray n2; for (const auto& r : res2) n2.add (r.semantic + ": " + r.note); std::printf ("  results2: %s\n", n2.joinIntoString (" | ").toRawUTF8()); }
    check (std::abs (ps[1]->getValue() - 0.0f) < 0.02f, "when both sides are named, each keeps its own value (R Attack 1 ms -> 0.0)", juce::String (ps[1]->getValue(), 3));
    std::printf ("\n==== pair_dial_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
