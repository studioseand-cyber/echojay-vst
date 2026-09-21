// land_by_name_guard (21 Sep 2026, ruling b): a map entry's parameter is resolved by NAME on the live instance; the stored index
// is a hint. Fixture = the index-drift observation: Pro-Q 3 "Analyzer Tilt" stored at 375, the real AU has it at 351. RED as it
// stood: the write went to index 375 (a different parameter). Plus: a named entry that resolves nowhere is skipped and logged,
// never dialled at the guessed index.
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
// 400 parameters; "Analyzer Tilt" REALLY sits at 351, "Solo" at 375 (what the stale map index points at)
struct MockProQ final : juce::AudioPluginInstance
{
    MockProQ() { for (int i = 0; i < 400; ++i) addHostedParameter (std::make_unique<MockParam> (i == 351 ? "Analyzer Tilt" : i == 375 ? "Solo" : i == 12 ? "Output  Gain" : "Param " + juce::String (i))); }
    const juce::String getName() const override { return "Pro-Q 3"; }
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
    juce::Array<juce::var> anchors; for (int k = 0; k <= 4; ++k) { const double t = k / 4.0; juce::Array<juce::var> a; a.add (lo + (hi - lo) * t); a.add (t); anchors.add (juce::var (a)); } e->setProperty ("anchors", anchors);
    return juce::var (e);
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("land_by_name_guard: the Pro-Q 3 Analyzer Tilt fixture (stored 375, real 351); an unresolved name is skipped\n");
    MockProQ plug;
    auto* controls = new juce::DynamicObject();
    controls->setProperty ("Analyzer Tilt", entry ("Analyzer Tilt", 375, 0.0, 6.0, "db"));   // the map's stale index
    controls->setProperty ("Ghost Knob", entry ("Ghost Knob", 3, 0.0, 10.0, "db"));          // a name that resolves nowhere
    controls->setProperty ("Output Gain", entry ("Output Gain", 12, -10.0, 10.0, "db"));     // resolves NORMALISED (the live name has two spaces)
    auto* map = new juce::DynamicObject(); map->setProperty ("plugin", "Pro-Q 3"); map->setProperty ("controls", juce::var (controls));
    const juce::var mapVar (map);
    auto* req = new juce::DynamicObject(); req->setProperty ("Analyzer Tilt", 4.5); req->setProperty ("Ghost Knob", 5.0); req->setProperty ("Output Gain", 0.0);
    auto* settings = new juce::DynamicObject(); settings->setProperty ("controls", juce::var (req));
    const auto results = echojay::applySettings (plug, mapVar, juce::var (settings));
    auto& ps = plug.getParameters();
    juce::StringArray notes; for (const auto& r : results) notes.add (r.semantic + ": " + (r.applied ? "APPLIED@" + juce::String (r.index) : "declined") + " (" + r.note + ")");
    std::printf ("  results: %s\n", notes.joinIntoString (" | ").toRawUTF8());
    check (std::abs (ps[351]->getValue() - 0.75f) < 0.02f, "Analyzer Tilt 4.5 dB lands at the REAL index 351 = 0.75 (RED as it stood: the stale index 375 was written)", "p351=" + juce::String (ps[351]->getValue(), 3) + " p375=" + juce::String (ps[375]->getValue(), 3));
    check (std::abs (ps[375]->getValue()) < 0.001f, "...and index 375 (\"Solo\", the stale index) is untouched", juce::String (ps[375]->getValue(), 3));
    bool ghostApplied = false, ghostNoted = false; for (const auto& r : results) if (r.semantic == "Ghost Knob") { ghostApplied = r.applied; ghostNoted = r.note.containsIgnoreCase ("unresolved"); }
    check (! ghostApplied && ghostNoted && std::abs (ps[3]->getValue()) < 0.001f, "an unresolved name (\"Ghost Knob\", map index 3) is NOT dialled at the guessed index and its note says unresolved (the EJMap: unresolved line is on the log)", "p3=" + juce::String (ps[3]->getValue(), 3));
    check (std::abs (ps[12]->getValue() - 0.5f) < 0.02f, "a name that differs only in whitespace resolves NORMALISED (\"Output  Gain\" -> Output Gain 0 dB = 0.5)", juce::String (ps[12]->getValue(), 3));
    std::printf ("\n==== land_by_name_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
