// stepped_position_guard (round (c), 21 Sep 2026): a stepped control is dialled by POSITION - snapped to a detent, the landed panel
// text verified against the map's `positions` - never by anchor interpolation between detents. A position is asked by its panel
// text ("off"), by a number the text carries ("60" -> "60Hz"), or by its 1-based index; an unknown position is refused, a landed
// text that does not match reverts. An `anchored` entry that carries `steps` snaps after interpolation. Plus the real thing:
// CLA-76 (m)'s Analog (3 steps on the AU, mapped today as anchored 50..60 Hz) lands "Off" from the word off and "60Hz" from 60.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayParamApply.h"
#include "ChainHost.h"        // 21r item 2(b): the sampled-text store lives on the host that processes audio
#include "EchoJayParamMaps.h"
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
// 21o item 5 (23 Sep 2026): the Auto-Tune shape - a control that reports itself CONTINUOUS (numSteps INT_MAX,
// isDiscrete false) while its display text steps through names at UNEVEN normalised centres. The measured centres
// below are the first five of the real Key sweep (probe --sample-text 2 on Auto-Tune Pro, 23 Sep).
struct UnevenTextParam final : juce::HostedAudioProcessorParameter
{
    UnevenTextParam (juce::String n, juce::StringArray texts, juce::Array<float> los, juce::Array<float> his)
        : nm (std::move (n)), tx (std::move (texts)), lo (std::move (los)), hi (std::move (his)) {}
    juce::String getParameterID() const override { return nm.replace (" ", "_"); }
    float getValue() const override { return v; }
    void setValue (float x) override { v = juce::jlimit (0.0f, 1.0f, x); }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return nm; }
    juce::String getLabel() const override { return {}; }
    int getNumSteps() const override { return juce::AudioProcessor::getDefaultNumParameterSteps(); }   // "continuous", as Antares reports
    bool isDiscrete() const override { return false; }
    float getValueForText (const juce::String& t) const override { const int i = tx.indexOf (t); return i < 0 ? 0.0f : (lo[i] + hi[i]) * 0.5f; }
    juce::String getText (float x, int) const override
    { for (int i = 0; i < tx.size(); ++i) if (x >= lo[i] - 1.0e-6f && x <= hi[i] + 1.0e-6f) return tx[i]; return tx[tx.size() - 1]; }
    juce::String nm; juce::StringArray tx; juce::Array<float> lo, hi; float v = 0.0f;
};
// 21o item 6: a read-out row - readable, never writable; JUCE reports isAutomatable() false for exactly these on an AU
struct MeterParam final : juce::HostedAudioProcessorParameter
{
    explicit MeterParam (juce::String n) : nm (std::move (n)) {}
    juce::String getParameterID() const override { return nm.replace (" ", "_"); }
    float getValue() const override { return v; } void setValue (float x) override { v = x; }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return nm; }
    juce::String getLabel() const override { return {}; }
    bool isAutomatable() const override { return false; }
    float getValueForText (const juce::String& t) const override { return juce::jlimit (0.0f, 1.0f, t.getFloatValue()); }
    juce::String getText (float x, int) const override { return juce::String (x, 2); }
    juce::String nm; float v = 0.0f;
};
struct MockStepped final : juce::AudioPluginInstance
{
    explicit MockStepped (bool lie = false)
    {
        addHostedParameter (std::make_unique<StepParam> ("Analog", juce::StringArray { "Off", "50Hz", "60Hz" }, lie));
        juce::StringArray th; for (int i = 0; i < 23; ++i) th.add (juce::String (-23 + i) + " dB");
        addHostedParameter (std::make_unique<StepParam> ("Thresh", th));
        juce::StringArray gn; for (int i = 0; i < 16; ++i) gn.add ("+" + juce::String (i) + " dB");   // 22 Sep 2026 (ruling 3): a 16-step gain knob, the Manley shape
        addHostedParameter (std::make_unique<StepParam> ("Gain", gn));
        // 21o item 5: "Key", continuous by its own account, five named detents at the measured centres
        addHostedParameter (std::make_unique<UnevenTextParam> ("Key", juce::StringArray { "C", "C#", "D", "D#", "E" },
                            juce::Array<float> { 0.000000f, 0.046875f, 0.136719f, 0.228516f, 0.318359f },
                            juce::Array<float> { 0.044922f, 0.134766f, 0.226562f, 0.316406f, 1.000000f }));
        // 21o item 6: the read-out rows, shaped as the PuigChild 660's LED ladder
        addHostedParameter (std::make_unique<MeterParam> ("Left VU"));
        addHostedParameter (std::make_unique<MeterParam> ("Left Threshold 0"));
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
// the SHIPPING sequence (21 Sep 2026 ruling): write, then verify after a message-loop settle - a plugin whose reads are one write
// behind (WaveShell) must not read as "did not stick". On a tree without the settle helper the immediate verdict stands (RED).
echojay::ApplyResult one (juce::AudioPluginInstance& p, const juce::var& map, const char* name, const juce::var& value)
{
    auto rs = echojay::applySettings (p, map, ask (name, value));
#ifdef EJ_SETTLED_READBACK
    bool pending = false; for (const auto& r : rs) pending = pending || r.pendingSettle;
    if (pending) { for (int k = 0; k < 12; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); } echojay::settleVerify (p, rs, true); }
#endif
    for (const auto& r : rs) if (r.semantic == name) return r; return {};
}
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
        // P3 SUPERSEDED BY THE 24 Sep 2026 RULING (21r item 2a). It used to assert that a bare 1-based index lands
        // position 2. A bare index is a claim about the ORDER of a list the server holds, and when that order is
        // wrong the write lands on a different position and reads back as one - "op said Minor, plugin shows
        // Arabic 2". The assertion is INVERTED here rather than deleted, so the change of rule is visible in the
        // guard that used to hold the old one. Position 2 is still reachable, by its NAME ("50Hz"), one line below.
        const float wasNorm = ps[0]->getValue();
        r = one (plug, map, "Analog", 2);
        check (! r.applied && r.note.contains ("bare index") && std::abs (ps[0]->getValue() - wasNorm) < 0.001f,
               "P3. a bare 1-based position (2) is REFUSED and nothing is written  (ruling of 24 Sep 2026; this leg used to assert the opposite)", show (r));
        r = one (plug, map, "Analog", "50Hz");
        check (r.applied && std::abs (ps[0]->getValue() - 0.5f) < 0.001f && flat (r.landedText) == "50hz",
               "P3. ...and the same position lands by NAME", show (r));
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
    std::printf ("== M. 22 Sep 2026 (ruling 3, the Manley interim): a DISCRETE parameter with an ANCHORED map (no steps declared) snaps an off-rung ask to the nearest detent ==\n");
    {
        MockStepped plug; auto& ps = plug.getParameters();
        auto* e = new juce::DynamicObject(); e->setProperty ("name", "Gain"); e->setProperty ("index", 2); e->setProperty ("kind", "anchored"); e->setProperty ("unit", "db"); e->setProperty ("trust", "setread");
        juce::Array<juce::var> anc; for (int i = 0; i < 16; ++i) { juce::Array<juce::var> a; a.add ((double) i); a.add ((double) i / 15.0); anc.add (juce::var (a)); } e->setProperty ("anchors", anc);
        juce::Array<juce::var> rg; rg.add (0.0); rg.add (15.0); e->setProperty ("range", rg);
        const auto map = mapOf ("MockStepped", { { "Gain", juce::var (e) } });
        const auto r = one (plug, map, "Gain", 2.3);
        check (r.applied && ! r.readbackMismatch && std::abs (ps[2]->getValue() - 2.0f / 15.0f) < 0.002f && flat (r.landedText) == "+2db", "M1. asked +2.3 on a 16-step discrete gain (anchored map, no steps): lands the +2 detent, verified, NOT reverted", show (r));
        check (r.note.contains ("nearest step to 2.3") && r.note.startsWith ("+2 dB"), "M1. the note reads \"<landed> (nearest step to <asked>)\"", r.note);
        check (! r.note.containsIgnoreCase ("hand") && ! r.note.containsIgnoreCase ("ladder") && ! r.note.containsIgnoreCase ("reachable"), "M1. never \"need hand-dialing\" / the ladder refusal for a discrete control that has anchors", r.note);
        const auto r2 = one (plug, map, "Gain", 7.0);
        check (r2.applied && flat (r2.landedText) == "+7db" && ! r2.note.contains ("nearest step"), "M2. an ask ON a detent (+7) lands it with no snap note", show (r2));
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
            // 21 Sep 2026 ruling: the WaveShell AU's reads are ONE WRITE BEHIND until the message loop runs; verification of a landed
            // value happens after a settle for EVERY plugin (one tick minimum, bounded 250 ms), on the shipping path - no test-only setting.
            auto r = one (*inst, map, "Analog", "off"); pump (200); auto nowText = ps[idx >= 0 ? idx : 0]->getCurrentValueAsText().trim();
            check (idx >= 0 && r.applied && ! r.readbackMismatch && flat (r.landedText) == "off" && flat (nowText) == "off" && r.normalized > 0.99f, "R1. \"off\" lands \"Off\" on the real Analog control with the SHIPPING setting (position 3 = norm 1.0 on this panel), verified after the settle, not reverted", show (r) + " | panel now \"" + nowText + "\"");
            r = one (*inst, map, "Analog", 60); pump (200); nowText = ps[idx >= 0 ? idx : 0]->getCurrentValueAsText().trim();
            check (idx >= 0 && r.applied && ! r.readbackMismatch && flat (r.landedText) == "60hz" && flat (nowText) == "60hz" && std::abs (r.normalized - 0.5f) < 0.01f, "R2. 60 lands \"60Hz\" on the real Analog control with the SHIPPING setting (position 2 = norm 0.5), verified after the settle, not reverted", show (r) + " | panel now \"" + nowText + "\"");
            {   // A1: the EXISTING anchored path on the same plugin - Ratio 8 (the map's own anchors 20/12/8/4 at 0/.25/.5/.75)
                auto* e = new juce::DynamicObject(); e->setProperty ("name", "Ratio"); e->setProperty ("index", 4); e->setProperty ("kind", "anchored"); e->setProperty ("trust", "setread");
                juce::Array<juce::var> rg; rg.add (4.0); rg.add (20.0); e->setProperty ("range", rg);
                juce::Array<juce::var> an; for (auto pr : { std::make_pair (20.0, 0.0), std::make_pair (12.0, 0.25), std::make_pair (8.0, 0.5), std::make_pair (4.0, 0.75) }) { juce::Array<juce::var> a; a.add (pr.first); a.add (pr.second); an.add (juce::var (a)); } e->setProperty ("anchors", an);
                const auto rmap = mapOf ("CLA-76 (m)", { { "Ratio", juce::var (e) } });
                ps[4]->setValueNotifyingHost (0.0f); pump (200);
                auto ra = one (*inst, rmap, "Ratio", 8); pump (200);
                check (ra.applied && ! ra.readbackMismatch && std::abs (ps[4]->getValue() - 0.5f) < 0.02f && flat (ps[4]->getCurrentValueAsText()) == "8", "A1. the anchored path on the real WaveShell AU: Ratio 8 lands (norm 0.5, panel \"8\") and is NOT reverted (RED as it stood: \"write did not stick\", value restored)", show (ra) + " | value " + juce::String (ps[4]->getValue(), 3) + " panel \"" + ps[4]->getCurrentValueAsText().trim() + "\"");
            }
            inst.reset();
        }
    }
    std::printf ("== S. 23 Sep 2026 (21o item 5): a position entry may carry its own MEASURED normalised centre; the write lands there, not on (p-1)/(steps-1) ==\n");
    {
        MockStepped m; auto& ps = m.getParameters();
        auto* key = ps[3];
        check (key != nullptr && key->getName (32) == "Key" && ! key->isDiscrete(), "S0. the mock's Key reports itself CONTINUOUS, as Auto-Tune Pro does", key ? juce::String ((int) key->isDiscrete()) : "null");
        // the map the sampled-text builder would write: {index, name, normalised} per detent
        auto posObj = [] (int i, const char* nm, double norm) { auto* o = new juce::DynamicObject(); o->setProperty ("index", i); o->setProperty ("name", nm); o->setProperty ("normalised", norm); return juce::var (o); };
        juce::Array<juce::var> pos { posObj (1, "C", 0.022461), posObj (2, "C#", 0.090820), posObj (3, "D", 0.181641), posObj (4, "D#", 0.272461), posObj (5, "E", 0.363281) };
        auto* me = new juce::DynamicObject(); me->setProperty ("name", "Key"); me->setProperty ("index", 3); me->setProperty ("kind", "position"); me->setProperty ("positions", juce::var (pos));
        const juce::var mapEntry (me);
        {
            const auto r = echojay::applyOne (m, "key", mapEntry, juce::var ("D"));
            check (r.applied && std::abs (key->getValue() - 0.181641f) < 0.002f && r.landedText.trim() == "D",
                   "S1. \"D\" lands on the MEASURED centre 0.1816 and the plugin reads back \"D\"  (RED as it stood: the object entries parse as empty names and the position is refused)",
                   "norm " + juce::String (key->getValue(), 4) + " text \"" + r.landedText.trim() + "\" | " + r.note.substring (0, 80));
        }
        {
            const auto r = echojay::applyOne (m, "key", mapEntry, juce::var ("C#"));
            check (r.applied && std::abs (key->getValue() - 0.090820f) < 0.002f && r.landedText.trim() == "C#", "S2. ...and \"C#\" on 0.0908 (even spacing would have written 0.25 and read \"D#\")",
                   "norm " + juce::String (key->getValue(), 4) + " text \"" + r.landedText.trim() + "\"");
        }
        {
            const auto r = echojay::applyOne (m, "key", mapEntry, juce::var ("Minor"));
            check (! r.applied && r.note.containsIgnoreCase ("unknown position"), "S3. a name the control does not have is REFUSED and the positions are listed", r.note.substring (0, 90));
        }
        {   // a bare-string map is unchanged: even spacing, as round (c) shipped it
            auto* mo = new juce::DynamicObject(); mo->setProperty ("name", "Analog"); mo->setProperty ("index", 0); mo->setProperty ("kind", "position"); mo->setProperty ("steps", 3);
            juce::Array<juce::var> bare { juce::var ("Off"), juce::var ("50Hz"), juce::var ("60Hz") };
            mo->setProperty ("positions", juce::var (bare));
            const auto r = echojay::applyOne (m, "analog", juce::var (mo), juce::var ("50Hz"));
            check (r.applied && std::abs (ps[0]->getValue() - 0.5f) < 0.001f, "S4. a bare-string position map is unchanged (even spacing)", juce::String (ps[0]->getValue(), 3));
        }
    }
    std::printf ("== T. 23 Sep 2026 (21o item 6): a read-out row is distinguishable by flag, so the client marks it instead of offering it ==\n");
    {
        // the 660's own shape, measured: 6 settable controls, 69 read-out rows
        struct Mock660 final : juce::AudioPluginInstance
        {
            Mock660()
            {
                for (const char* n : { "OnOff", "Mains", "Input", "Threshold", "Time Constant", "Output" })
                    addHostedParameter (std::make_unique<StepParam> (n, juce::StringArray { "a", "b" }));
                addHostedParameter (std::make_unique<MeterParam> ("OnOffLed"));
                addHostedParameter (std::make_unique<MeterParam> ("Left VU"));
                for (int i = -20; i <= 0; ++i)  addHostedParameter (std::make_unique<MeterParam> ("Left Input " + juce::String (i)));
                for (int i = 0; i <= 10; ++i)   addHostedParameter (std::make_unique<MeterParam> ("Left Threshold " + juce::String (i)));
                for (int i = 1; i <= 6; ++i)    addHostedParameter (std::make_unique<MeterParam> ("Left Time Constant " + juce::String (i)));
                for (int i = 0; i < 25; ++i)    addHostedParameter (std::make_unique<MeterParam> ("Left Output " + juce::String (-18.0 + i * 1.5, 1)));
                for (const char* n : { "OnOff_On", "Mains_American", "Mains_Off", "Mains_British" })
                    addHostedParameter (std::make_unique<MeterParam> (n));
            }
            const juce::String getName() const override { return "PuigChild 660 (m) [shape]"; }
            void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = getName(); }
            void prepareToPlay (double, int) override {} void releaseResources() override {} void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            double getTailLengthSeconds() const override { return 0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
            juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
            int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
            const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
            void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
        };
        Mock660 m660;
        const int settable = echojay::settableParamCount (m660, 512);
        const auto ro = echojay::readOnlyParamNames (m660, 512, 128);
        check (m660.getParameters().size() == 75, "T1. the fixture carries the 660's 75 parameters", juce::String (m660.getParameters().size()));
        check (settable == 6 && ro.size() == 69, "T1. the client's classifier (the one ChainHost's lookup body uses) exposes SIX settable controls and marks the other 69  (RED as it stood: the body carried no mark at all)",
               juce::String (settable) + " settable / " + juce::String (ro.size()) + " marked");
        check (ro.contains ("Left Threshold 0") && ro.contains ("Left Threshold 6") && ro.contains ("Left VU") && ! ro.contains ("Threshold"),
               "T1. ...the marked set holds the LED rows (Left Threshold 0..6, Left VU) and NOT the real Threshold", ro.joinIntoString (",").substring (0, 90));
    }
    // ---- 21r item 2(a) (24 Sep 2026): a named position is VERIFIED, and anything else is refused ---------------
    // THE DEFECT SHAPE: the op says Minor, the map's position 2 is "Arabic 2", and the write lands on Arabic 2 and
    // reads back as one. A stepped control is dialled BY NAME, the plugin's own display text is read back, and a
    // mismatch is refused and rolled back rather than reported as applied.
    {
        std::printf ("\n== 21r item 2(a): named positions are verified, bare indices are not written ==\n");
        // Ruling 1: the display width is the limit, not a mismatch.
        check (echojay::positionTextMatches ("CHROMATI", "Chromatic"),
               "(2a) a truncated display reads back as its canonical name  [CHROMATI <-> Chromatic]");
        check (echojay::positionTextMatches ("INSTRUME", "Instrument"),
               "(2a) ...at whatever width the plugin prints  [INSTRUME <-> Instrument]");
        check (echojay::positionTextMatches ("MINOR", "Minor"), "(2a) ...and an untruncated name still matches");
        check (! echojay::positionTextMatches ("ARABIC 2", "Minor"),
               "(2a) \"ARABIC 2\" does NOT read back as Minor  (the defect this exists for)");
        check (! echojay::positionTextMatches ("MINOR", "Harmonic Minor"),
               "(2a) ...and it is a PREFIX rule, not a substring rule: MINOR is not Harmonic Minor");
        check (! echojay::positionTextMatches ("", "Minor"), "(2a) an empty readback is never a match");
        check (echojay::positionTextMatches ("MINOR", "Minor", "MINOR"),
               "(2a) a sampled position carries BOTH texts and either one matches");

        // The behavioural half, on a plugin whose position 2 IS "Arabic 2" and whose display truncates at 8.
        {
            struct Trunc8 final : juce::AudioPluginInstance
            {
                Trunc8() { addHostedParameter (std::make_unique<StepParam> ("Scale", juce::StringArray { "MAJOR", "ARABIC 2", "CHROMATI" })); }
                const juce::String getName() const override { return "Trunc8"; }
                void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = getName(); }
                void prepareToPlay (double, int) override {} void releaseResources() override {}
                void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
                double getTailLengthSeconds() const override { return 0; }
                bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
                juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
                int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
                const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
                void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
            };
            Trunc8 t8;
            auto* scale = t8.getParameters()[0];
            const auto entry = positionEntry ("Scale", 0, { "Major", "Arabic 2", "Chromatic" });

            const auto rMinor = echojay::applyOne (t8, "scale", entry, juce::var ("Minor"));
            check (! rMinor.applied && rMinor.note.contains ("unknown position"),
                   "(2a) the op says Minor, no position is named Minor -> NOT written", rMinor.note);
            check (scale->getValue() == 0.0f, "(2a) ...and the parameter never moved", juce::String (scale->getValue(), 3));

            const auto rIdx = echojay::applyOne (t8, "scale", entry, juce::var (2));
            check (! rIdx.applied && rIdx.note.contains ("bare index"),
                   "(2a) a bare index (2) is REFUSED  (RED as it stood: it wrote position 2 - \"Arabic 2\")", rIdx.note);
            check (scale->getValue() == 0.0f, "(2a) ...and again the parameter never moved", juce::String (scale->getValue(), 3));

            const auto rChrom = echojay::applyOne (t8, "scale", entry, juce::var ("Chromatic"));
            check (rChrom.applied && rChrom.landedText == "CHROMATI",
                   "(2a) \"Chromatic\" lands and its truncated readback verifies (ruling 1)",
                   rChrom.landedText + " / " + rChrom.note);

            auto* bare = new juce::DynamicObject();
            bare->setProperty ("name", "Scale"); bare->setProperty ("index", 0);
            bare->setProperty ("kind", "position"); bare->setProperty ("steps", 3);
            const auto rBare = echojay::applyOne (t8, "scale", juce::var (bare), juce::var (2));
            check (! rBare.applied && rBare.note.contains ("not named yet"),
                   "(2a) a stepped control with no named positions is not written; the probe names it first", rBare.note);
        }
    }

    // ---- 21r item 2(b): the sampled-text sweep, parsed and merged ------------------------------------------
    // The probe output below is VERBATIM from EchoJayProbe --sample-stepped against UAD Auto-Tune Realtime
    // Advanced (AudioUnit|22424d57|11.8.0) on 24 Sep 2026, trimmed to three controls. The parse is pinned against
    // the bytes, not against a description of them.
    {
        std::printf ("\n== 21r item 2(b): the sampled-text sweep is parsed and merged by parameter index ==\n");
        const juce::String probeOut =
            "param\t0\tInput Type\n"
            "pos\t1\t0.166016\t0.000000\t0.332031\tSOPRANO\n"
            "pos\t2\t0.500000\t0.333984\t0.666016\tALTO/TEN\n"
            "pos\t3\t0.833008\t0.667969\t0.998047\tLOW MALE\n"
            "pos\t4\t1.000000\t1.000000\t1.000000\tINSTRUME\n"
            "distinct\t4\n"
            "skip\t3\tRetune Speed\tcontinuous\n"
            "param\t2\tScale\n"
            "pos\t1\t0.017578\t0.000000\t0.035156\tMAJOR\n"
            "pos\t2\t0.053711\t0.037109\t0.070312\tMINOR\n"
            "pos\t3\t0.088867\t0.072266\t0.105469\tCHROMATI\n"
            "distinct\t3\n"
            "sampled\t2\tof\t20\n";
        const auto sampled = echojay::parseSampledStepped (probeOut);
        auto* so = sampled.getDynamicObject();
        check (so != nullptr && so->getProperties().size() == 2,
               "(2b) two named controls parsed out of the sweep, the continuous one skipped",
               juce::String (so == nullptr ? -1 : so->getProperties().size()));
        auto scale = sampled.getProperty ("2", juce::var());
        auto* sps = scale.getProperty ("positions", juce::var()).getArray();
        check (scale.getProperty ("name", juce::var()).toString() == "Scale" && sps != nullptr && sps->size() == 3,
               "(2b) ...keyed by PARAMETER INDEX, with the control's own name",
               scale.getProperty ("name", juce::var()).toString() + " / " + juce::String (sps == nullptr ? -1 : sps->size()));
        if (sps != nullptr && sps->size() == 3)
        {
            const auto p2 = sps->getReference (1);
            check (p2.getProperty ("name", juce::var()).toString() == "MINOR"
                   && p2.getProperty ("display", juce::var()).toString() == "MINOR",
                   "(2b) ...each position carrying BOTH texts (identical until a profile names it)");
            check (std::abs ((double) p2.getProperty ("normalised", juce::var()) - 0.053711) < 1e-6,
                   "(2b) ...at the MEASURED centre of the detent, not an even-spacing guess",
                   juce::String ((double) p2.getProperty ("normalised", juce::var()), 6));
        }

        // The merge: a map control at that index gains the names; one the map already names is left alone.
        auto* c2 = new juce::DynamicObject(); c2->setProperty ("index", 2); c2->setProperty ("kind", "position");
        auto* c0 = new juce::DynamicObject(); c0->setProperty ("index", 0); c0->setProperty ("kind", "position");
        juce::Array<juce::var> mine; mine.add (juce::var ("Hand Written"));
        c0->setProperty ("positions", mine); c0->setProperty ("steps", 1);
        auto* ctrl = new juce::DynamicObject();
        ctrl->setProperty ("scale", juce::var (c2)); ctrl->setProperty ("input_type", juce::var (c0));
        auto* mp = new juce::DynamicObject(); mp->setProperty ("controls", juce::var (ctrl));
        juce::var theMap (mp);
        const int filled = echojay::mergeSampledIntoMap (theMap, sampled);
        check (filled == 1, "(2b) exactly the unnamed control is filled", juce::String (filled));
        auto merged = theMap.getProperty ("controls", juce::var()).getProperty ("scale", juce::var());
        check ((int) merged.getProperty ("steps", juce::var()) == 3
               && merged.getProperty ("positions", juce::var()).getArray() != nullptr,
               "(2b) ...and it becomes dialable by name (3 positions)",
               juce::String ((int) merged.getProperty ("steps", juce::var())));
        auto kept = theMap.getProperty ("controls", juce::var()).getProperty ("input_type", juce::var());
        check (kept.getProperty ("positions", juce::var()).getArray() != nullptr
               && kept.getProperty ("positions", juce::var()).getArray()->getReference (0).toString() == "Hand Written",
               "(2b) a map that already names its positions is NOT overwritten by the sweep");

        // A sweep that found nothing still counts as done: it must not be repeated on every load.
        // The scribble leg re-runs this binary in the SAME isolated HOME, and this leg PERSISTS what it stores,
        // so without clearing the cache first the second leg would start with the identity already sampled.
        ChainHost::getParamMapsCacheFile().deleteFile();
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
        const juce::String ik = "AudioUnit|22424d57|11.8.0", fp = "fp-of-that-binary";
        check (! host->steppedTextSampled (ik), "(2b) an identity starts unsampled");
        host->applySampledStepped (fp, ik, juce::var());
        check (host->steppedTextSampled (ik),
               "(2b) a sweep that found NOTHING still marks the identity sampled - it is not repeated every load");
        host->applySampledStepped (fp, ik, sampled);
        check (host->steppedTextFor (fp).getDynamicObject() != nullptr,
               "(2b) ...and a sweep that found something is stored against the fp");
    }

    std::printf ("\n==== stepped_position_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
