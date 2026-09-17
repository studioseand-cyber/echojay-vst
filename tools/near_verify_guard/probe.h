#pragma once
#include <JuceHeader.h>
namespace guardprobe {
static const char* kNames[5] = { "Key", "Scale", "Retune Speed", "Detune", "Humanize" };
struct NamedParam final : public juce::HostedAudioProcessorParameter {
    juce::String nm; float v = 0.5f; explicit NamedParam (const char* n) : nm (n) {}
    float getValue() const override { return v; } void setValue (float nv) override { v = juce::jlimit (0.0f, 1.0f, nv); }
    float getDefaultValue() const override { return 0.5f; } juce::String getName (int) const override { return nm; } juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String& t) const override { return t.getFloatValue(); } juce::String getText (float value, int) const override { return juce::String (value, 3); }
    juce::String getParameterID() const override { return nm.toLowerCase().replaceCharacter (' ', '_'); } };
inline juce::PluginDescription probeDesc (int uid, const juce::String& name = "Guard Tune") { juce::PluginDescription d; d.name = name; d.pluginFormatName = "AudioUnit"; d.manufacturerName = "Guard"; d.fileOrIdentifier = "guard:near:" + juce::String (uid); d.uniqueId = 0x4E6D000 + uid; d.version = "10.5.0"; return d; }
struct Probe final : public juce::AudioPluginInstance {
    juce::PluginDescription d;
    explicit Probe (const juce::PluginDescription& dd) : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)), d (dd) { for (auto* n : kNames) addHostedParameter (std::make_unique<NamedParam> (n)); }
    void fillInPluginDescription (juce::PluginDescription& o) const override { o = d; }
    const juce::String getName() const override { return d.name; }
    void prepareToPlay (double, int) override {} void releaseResources() override {} void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {} };
// A same-name map from ANOTHER build: names at deliberately WRONG indices (10+i), 3-anchor continuous curve 0..100.
inline juce::var nearMap (const juce::String& fp, const juce::StringArray& names) {
    auto* ctl = new juce::DynamicObject();
    for (int i = 0; i < names.size(); ++i) {
        auto* e = new juce::DynamicObject(); e->setProperty ("index", 10 + i); e->setProperty ("name", names[i]); e->setProperty ("kind", "anchored"); e->setProperty ("trust", "setread");
        juce::var anchors; { juce::var a; a.append (0.0); a.append (0.0); anchors.append (a); } { juce::var m; m.append (37.0); m.append (0.37); anchors.append (m); } { juce::var b; b.append (100.0); b.append (1.0); anchors.append (b); }
        e->setProperty ("anchors", anchors); ctl->setProperty (juce::Identifier (names[i]), juce::var (e)); }
    auto* map = new juce::DynamicObject(); map->setProperty ("fp", fp); map->setProperty ("dialable", true); map->setProperty ("category", "pitch"); map->setProperty ("controls", juce::var (ctl)); map->setProperty ("params", juce::var (new juce::DynamicObject())); return juce::var (map); }
inline juce::var cand (const juce::String& fp, const juce::var& map) { auto* c = new juce::DynamicObject(); c->setProperty ("fp", fp); c->setProperty ("map", map); c->setProperty ("essential_plumbing", juce::var (juce::Array<juce::var>())); return juce::var (c); }
inline juce::var missRow (const juce::String& body, const juce::Array<juce::var>& near) {
    auto ik = juce::JSON::parse (body).getProperty ("plugins", juce::var())[0].getProperty ("ik", juce::var()).toString();
    auto* r = new juce::DynamicObject(); r->setProperty ("i", 0); r->setProperty ("ik", ik); r->setProperty ("tier", "miss"); r->setProperty ("map", juce::var()); r->setProperty ("reason", "fewer_params");
    r->setProperty ("near", juce::var (near)); juce::Array<juce::var> arr; arr.add (juce::var (r)); return juce::var (arr); }
inline void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
}
