// stepped_position_guard (round (c), 21 Sep 2026): a stepped control is dialled by POSITION - snapped to a detent, the landed panel
// text verified against the map's `positions` - never by anchor interpolation between detents. A position is asked by its panel
// text ("off"), by a number the text carries ("60" -> "60Hz"), or by its 1-based index; an unknown position is refused, a landed
// text that does not match reverts. An `anchored` entry that carries `steps` snaps after interpolation. Plus the real thing:
// CLA-76 (m)'s Analog (3 steps on the AU, mapped today as anchored 50..60 Hz) lands "Off" from the word off and "60Hz" from 60.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayParamApply.h"
#include "EJPaceCheck.h"
#include <cstdio>
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
// a stepped parameter as the AU wrappers present one: any normalised value is STORED raw (no snap), the text shows the nearest detent
struct StepParam final : juce::HostedAudioProcessorParameter
{
    StepParam (const juce::String& n, juce::StringArray texts, bool lie = false) : nm (n), tx (std::move (texts)), liar (lie) {}
    juce::String getParameterID() const override { return nm.replace (" ", "_"); }
    float getValue() const override { return v; }
    void setValue (float x) override { v = juce::jlimit (0.0f, 1.0f, x); }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return nm; }
    juce::String getLabel() const override { return {}; }
    int getNumSteps() const override { return tx.size(); }
    bool isDiscrete() const override { return true; }
    float getValueForText (const juce::String& t) const override { const int i = tx.indexOf (t); return i < 0 ? 0.0f : (float) i / (float) (tx.size() - 1); }
    juce::String getText (float x, int) const override { if (liar) return tx[0]; const int i = (int) std::round (juce::jlimit (0.0f, 1.0f, x) * (float) (tx.size() - 1)); return tx[i]; }
    juce::String nm; juce::StringArray tx; bool liar; float v = 0.0f;
};
struct MockStepped final : juce::AudioPluginInstance
{
    explicit MockStepped (bool lie = false)
    {
        addHostedParameter (std::make_unique<StepParam> ("Analog", juce::StringArray { "Off", "50Hz", "60Hz" }, lie));
        juce::StringArray th; for (int i = 0; i < 23; ++i) th.add (juce::String (-23 + i) + " dB");
        addHostedParameter (std::make_unique<StepParam> ("Thresh", th));
    }
    const juce::String getName() const override { return "MockStepped"; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = getName(); }
    void prepareToPlay (double, int) override {} void releaseResources() override {} void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};
juce::var positionEntry (const char* name, int index, const juce::StringArray& positions)
{
    auto* e = new juce::DynamicObject(); e->setProperty ("name", name); e->setProperty ("index", index); e->setProperty ("kind", "position"); e->setProperty ("steps", positions.size()); e->setProperty ("unit", "position"); e->setProperty ("trust", "setread");
    juce::Array<juce::var> p; for (const auto& t : positions) p.add (t); e->setProperty ("positions", p);
    return juce::var (e);
}
juce::var anchoredSteps (const char* name, int index, double lo, double hi, int steps, const char* unit)
{
    auto* e = new juce::DynamicObject(); e->setProperty ("name", name); e->setProperty ("index", index); e->setProperty ("kind", "anchored"); e->setProperty ("unit", unit); e->setProperty ("trust", "setread"); e->setProperty ("steps", steps);
    juce::Array<juce::var> range; range.add (lo); range.add (hi); e->setProperty ("range", range);
    juce::Array<juce::var> anchors; for (int k = 0; k <= 10; ++k) { const double t = k / 10.0; juce::Array<juce::var> a; a.add (lo + (hi - lo) * t); a.add (t); anchors.add (juce::var (a)); } e->setProperty ("anchors", anchors);   // a continuous-looking anchor table (11 rungs), the detents come from `steps`
    return juce::var (e);
}
juce::var mapOf (const char* plugin, std::initializer_list<std::pair<const char*, juce::var>> entries)
{
    auto* c = new juce::DynamicObject(); for (const auto& e : entries) c->setProperty (e.first, e.second);
    auto* m = new juce::DynamicObject(); m->setProperty ("plugin", plugin); m->setProperty ("controls", juce::var (c)); return juce::var (m);
}
juce::var ask (const char* name, const juce::var& value) { auto* r = new juce::DynamicObject(); r->setProperty (name, value); auto* s = new juce::DynamicObject(); s->setProperty ("controls", juce::var (r)); return juce::var (s); }
echojay::ApplyResult one (juce::AudioPluginInstance& p, const juce::var& map, const char* name, const juce::var& value, bool stale = false)
{ const auto rs = echojay::applySettings (p, map, ask (name, value), stale); for (const auto& r : rs) if (r.semantic == name) return r; return {}; }
juce::String show (const echojay::ApplyResult& r) { return (r.applied ? "APPLIED" : "declined") + juce::String (" norm ") + juce::String (r.normalized, 3) + " text \"" + r.landedText + "\" note: " + r.note; }
juce::String flat (juce::String s) { return s.removeCharacters (" ").toLowerCase(); }
void pump (int ms) { for (int k = 0; k < ms / 20; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); } }   // the WaveShell AU's value/text reads settle on the message loop
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("stepped_position_guard: positions snap to a detent and verify the panel text; the CLA-76 Analog leg on the real AU\n");
    const juce::StringArray analog { "Off", "50Hz", "60Hz" };
    {
        MockStepped plug; auto& ps = plug.getParameters();
        const auto map = mapOf ("MockStepped", { { "Analog", positionEntry ("Analog", 0, analog) }, { "Thresh", anchoredSteps ("Thresh", 1, -23.0, -1.0, 23, "db") } });
        auto r = one (plug, map, "Analog", "off");
        check (r.applied && std::abs (ps[0]->getValue()) < 0.001f && flat (r.landedText) == "off" && r.note.contains ("Off"), "P1. a position asked by its panel text (\"off\") lands position 1 = norm 0.0 and the note reads \"Off\" (verified)", show (r));
        r = one (plug, map, "Analog", 60);
        check (r.applied && std::abs (ps[0]->getValue() - 1.0f) < 0.001f && flat (r.landedText) == "60hz" && r.note.contains ("60Hz"), "P2. a number the panel text carries (60 -> \"60Hz\") lands position 3 = norm 1.0, text verified in the note", show (r));
        r = one (plug, map, "Analog", 2);
        check (r.applied && std::abs (ps[0]->getValue() - 0.5f) < 0.001f && flat (r.landedText) == "50hz", "P3. a bare 1-based position (2) lands position 2 = norm 0.5 (\"50Hz\")", show (r));
        ps[0]->setValue (0.5f); r = one (plug, map, "Analog", "70Hz");
        check (! r.applied && std::abs (ps[0]->getValue() - 0.5f) < 0.001f && r.note.containsIgnoreCase ("position"), "P4. an unknown position (\"70Hz\") is refused, not clamped to the last detent, the value untouched", show (r));
        r = one (plug, map, "Thresh", -12.3);
        check (r.applied && std::abs (ps[1]->getValue() - 0.5f) < 0.001f && flat (r.landedText) == "-12db", "P5. an anchored entry that carries steps SNAPS after interpolation: -12.3 dB on 23 detents -> 11/22 = 0.500, panel \"-12 dB\" (RED as it stood: 0.486 between detents)", show (r) + " value " + juce::String (ps[1]->getValue(), 3));
    }
    {
        MockStepped liar (true); auto& ps = liar.getParameters();
        const auto map = mapOf ("MockStepped", { { "Analog", positionEntry ("Analog", 0, analog) } });
        const auto r = one (liar, map, "Analog", 60);
        check (! r.applied && r.readbackMismatch && std::abs (ps[0]->getValue()) < 0.001f && r.note.containsIgnoreCase ("restored"), "P6. a landed text that does not match the position's panel text (the plugin shows \"Off\") reverts to the pre-write value", show (r));
    }
    std::printf ("== R. the real CLA-76 (m) AudioUnit: Analog is a 3-step control (Off / 50Hz / 60Hz on the panel; mapped today as anchored 50..60 Hz) ==\n");
    {
        juce::PluginDescription d; d.name = "CLA-76 (m)"; d.pluginFormatName = "AudioUnit"; d.fileOrIdentifier = "AudioUnit:Effects/aufx,76CM,ksWV"; d.uniqueId = d.deprecatedUid = (int) (juce::int64) juce::String ("3d307263").getHexValue64();
        const auto pace = echojay::refuseIfPaceWrapped (d);
        check (pace.isEmpty(), "R0. CLA-76 (m) is not PACE-wrapped (the unsigned guard may load it)", pace);
        juce::AudioPluginFormatManager fm; juce::addDefaultFormatsToManager (fm);
        juce::String err; std::unique_ptr<juce::AudioPluginInstance> inst = pace.isEmpty() ? fm.createPluginInstance (d, 48000.0, 512, err) : nullptr;
        if (inst == nullptr) { for (const char* leg : { "R1. \"off\" lands \"Off\" on the real Analog control, panel text verified", "R2. 60 lands \"60Hz\" on the real Analog control, panel text verified" }) check (false, leg, "CLA-76 (m) did not instantiate: " + err); }
        else
        {
            auto& ps = inst->getParameters(); int idx = -1; for (int i = 0; i < ps.size(); ++i) if (ps[i]->getName (128) == "Analog") { idx = i; break; }
            juce::StringArray panel; if (idx >= 0) for (float n : { 0.0f, 0.5f, 1.0f }) { ps[idx]->setValueNotifyingHost (n); pump (200); panel.add (ps[idx]->getCurrentValueAsText().trim()); }
            if (idx >= 0) { ps[idx]->setValueNotifyingHost (0.5f); pump (200); }   // start away from both asked positions
            std::printf ("  Analog at index %d, steps %d, panel texts at 0 / 0.5 / 1: %s\n", idx, idx >= 0 ? ps[idx]->getNumSteps() : -1, panel.joinIntoString (" / ").toRawUTF8());
            const auto map = mapOf ("CLA-76 (m)", { { "Analog", positionEntry ("Analog", idx, panel.size() == 3 ? panel : analog) } });
            // FLAG (21 Sep 2026, found by this leg): the WaveShell AU's value and text reads are ONE WRITE BEHIND until the message loop
            // runs - a native component, so ChainHost's staleDisplayReads gate (auComponentIsBridged) is off for it, and the immediate
            // readback compares the PRE-write state: the existing anchored path reverts too (Ratio 8 -> "write did not stick"). The
            // legs below therefore run with the stale-read setting (the write kept, "readback cannot be confirmed") and verify the panel
            // after the AU settles; the in-host reading (stale off) is printed as a flag, not counted. Not this round's fix.
            { auto rIn = one (*inst, map, "Analog", "off", false); pump (200);
              std::printf ("  FLAG  in-host setting (stale reads off): Analog=off -> %s | panel after settle \"%s\" (the immediate readback was pre-write)\n", show (rIn).toRawUTF8(), ps[idx >= 0 ? idx : 0]->getCurrentValueAsText().trim().toRawUTF8()); ps[idx >= 0 ? idx : 0]->setValueNotifyingHost (0.5f); pump (200); }
            auto r = one (*inst, map, "Analog", "off", true); pump (200); auto nowText = ps[idx >= 0 ? idx : 0]->getCurrentValueAsText().trim();
            check (idx >= 0 && r.applied && flat (nowText) == "off" && r.normalized > 0.99f, "R1. \"off\" lands \"Off\" on the real Analog control (position 3 = norm 1.0 on this panel), panel text verified after the AU settled", show (r) + " | panel now \"" + nowText + "\"");
            r = one (*inst, map, "Analog", 60, true); pump (200); nowText = ps[idx >= 0 ? idx : 0]->getCurrentValueAsText().trim();
            check (idx >= 0 && r.applied && flat (nowText) == "60hz" && std::abs (r.normalized - 0.5f) < 0.01f, "R2. 60 lands \"60Hz\" on the real Analog control (position 2 = norm 0.5), panel text verified after the AU settled", show (r) + " | panel now \"" + nowText + "\"");
            inst.reset();
        }
    }
    std::printf ("\n==== stepped_position_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
