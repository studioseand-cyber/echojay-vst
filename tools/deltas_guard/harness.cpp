// deltas_guard (21t, 25 Sep 2026): settings_structured.deltas - a MOVE from where the control already is.
// The server sends a delta when it was never told the control's current value; on 21t-a "lower the threshold"
// arrives as deltas and NOTHING else, so a client that does not apply them leaves the plugin where it was.
// Three shapes, all against THIS instance's readback: { db: N } adds dB, { factor: F } multiplies, and
// { positions: N } moves N entries along the map's position list (positive = later), clamped to the ends.
// An absolute value in `controls` for the same control always wins.
// THE NAMED CASE (Sean, 25 Sep 2026): a set op carrying { Threshold: { db: -3 } } on a compressor reading -18
// ends at -21.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayParamApply.h"
#include <cstdio>
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }

// A continuous control that PRINTS ITS VALUE IN ITS UNIT, because the whole point of a delta is that the client
// reads the plugin's own text back and moves from there.
struct NumParam final : juce::HostedAudioProcessorParameter
{
    NumParam (const juce::String& n, double lo_, double hi_, const juce::String& u, int dp_ = 1)
        : nm (n), lo (lo_), hi (hi_), unit (u), dp (dp_) {}
    juce::String getParameterID() const override { return nm.replace (" ", "_"); }
    float getValue() const override { return v; }
    void setValue (float x) override { v = juce::jlimit (0.0f, 1.0f, x); }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return nm; }
    juce::String getLabel() const override { return unit; }
    float getValueForText (const juce::String& t) const override { return (float) ((t.getDoubleValue() - lo) / (hi - lo)); }
    juce::String getText (float x, int) const override { return juce::String (lo + (hi - lo) * (double) x, dp) + " " + unit; }
    void setDisplay (double d) { v = (float) ((d - lo) / (hi - lo)); }
    juce::String nm; double lo, hi; juce::String unit; int dp; float v = 0.0f;
};
// A stepped control whose text is the position name.
struct StepParam final : juce::HostedAudioProcessorParameter
{
    StepParam (const juce::String& n, juce::StringArray p) : nm (n), pos (std::move (p)) {}
    juce::String getParameterID() const override { return nm.replace (" ", "_"); }
    float getValue() const override { return v; }
    void setValue (float x) override { v = juce::jlimit (0.0f, 1.0f, x); }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return nm; }
    juce::String getLabel() const override { return {}; }
    int getNumSteps() const override { return pos.size(); }
    bool isDiscrete() const override { return true; }
    int indexFor (float x) const { return juce::jlimit (0, pos.size() - 1, (int) std::lround ((double) x * (pos.size() - 1))); }
    float getValueForText (const juce::String& t) const override
    { for (int i = 0; i < pos.size(); ++i) if (pos[i].equalsIgnoreCase (t.trim())) return (float) i / (float) (pos.size() - 1); return 0.0f; }
    juce::String getText (float x, int) const override { return pos[indexFor (x)]; }
    void setPosition (int i) { v = (float) juce::jlimit (0, pos.size() - 1, i) / (float) (pos.size() - 1); }
    juce::String nm; juce::StringArray pos; float v = 0.0f;
};
struct MockComp final : juce::AudioPluginInstance
{
    MockComp()
    {
        addHostedParameter (std::make_unique<NumParam> ("Threshold", -60.0, 0.0, "dB"));
        addHostedParameter (std::make_unique<NumParam> ("Attack", 0.1, 100.0, "ms"));
        addHostedParameter (std::make_unique<StepParam> ("Ratio", juce::StringArray { "2:1", "4:1", "8:1", "20:1" }));
        addHostedParameter (std::make_unique<NumParam> ("Mode", 0.0, 1.0, "", 0));
    }
    const juce::String getName() const override { return "Mock Compressor"; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = getName(); }
    void prepareToPlay (double, int) override {} void releaseResources() override {} void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0; } bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};
juce::var linear (const char* name, int index, double lo, double hi, const char* unit)
{
    auto* e = new juce::DynamicObject(); e->setProperty ("name", name); e->setProperty ("index", index);
    e->setProperty ("kind", "anchored"); e->setProperty ("unit", unit);
    juce::Array<juce::var> range; range.add (lo); range.add (hi); e->setProperty ("range", range);
    juce::Array<juce::var> anchors;
    for (int k = 0; k <= 100; ++k) { const double t = k / 100.0; juce::Array<juce::var> a; a.add (lo + (hi - lo) * t); a.add (t); anchors.add (a); }
    e->setProperty ("anchors", anchors);
    return juce::var (e);
}
juce::var stepped (const char* name, int index, const juce::StringArray& names)
{
    auto* e = new juce::DynamicObject(); e->setProperty ("name", name); e->setProperty ("index", index);
    e->setProperty ("kind", "position"); e->setProperty ("steps", names.size());
    juce::Array<juce::var> pos;
    for (int i = 0; i < names.size(); ++i)
    {
        auto* p = new juce::DynamicObject(); p->setProperty ("name", names[i]); p->setProperty ("display", names[i]);
        p->setProperty ("normalised", (double) i / (double) (names.size() - 1));
        pos.add (juce::var (p));
    }
    e->setProperty ("positions", pos);
    return juce::var (e);
}
juce::var deltaDb (double n)     { auto* o = new juce::DynamicObject(); o->setProperty ("db", n);        return juce::var (o); }
juce::var deltaFactor (double f) { auto* o = new juce::DynamicObject(); o->setProperty ("factor", f);    return juce::var (o); }
juce::var deltaPos (int n)       { auto* o = new juce::DynamicObject(); o->setProperty ("positions", n); return juce::var (o); }
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("deltas_guard: settings_structured.deltas - db, factor and positions, against the readback\n");

    auto* controls = new juce::DynamicObject();
    controls->setProperty ("Threshold", linear ("Threshold", 0, -60.0, 0.0, "db"));
    controls->setProperty ("Attack",    linear ("Attack",    1, 0.1, 100.0, "ms"));
    controls->setProperty ("Ratio",     stepped ("Ratio",    2, juce::StringArray { "2:1", "4:1", "8:1", "20:1" }));
    controls->setProperty ("Mode",      linear ("Mode",      3, 0.0, 1.0, ""));
    auto* map = new juce::DynamicObject();
    map->setProperty ("plugin", "Mock Compressor");
    map->setProperty ("controls", juce::var (controls));
    const juce::var mapVar (map);   // ONE var owns the object (the 18 Sep double-own UAF)

    auto textOf = [] (juce::AudioPluginInstance& p, int i) { return p.getParameters()[i]->getCurrentValueAsText().trim(); };

    {   // THE NAMED CASE
        std::printf ("\n== { Threshold: { db: -3 } } on a compressor reading -18 ends at -21 ==\n");
        MockComp plug;
        static_cast<NumParam*> (plug.getParameters()[0])->setDisplay (-18.0);
        check (textOf (plug, 0) == "-18.0 dB", "fixture: the compressor reads -18.0 dB", textOf (plug, 0));
        auto* d = new juce::DynamicObject(); d->setProperty ("Threshold", deltaDb (-3.0));
        auto* st = new juce::DynamicObject(); st->setProperty ("deltas", juce::var (d));
        const auto res = echojay::applySettings (plug, mapVar, juce::var (st));
        check (textOf (plug, 0) == "-21.0 dB",
               "a db delta is added to the READBACK  (RED as it stood: deltas were not read at all, so the "
               "plugin did not move)", textOf (plug, 0));
        check (res.size() == 1 && res[0].semantic == "Threshold" && res[0].beforeText == "-18.0 dB",
               "...and the result carries before and after, so the move is auditable",
               res.size() == 1 ? (res[0].beforeText + " -> " + res[0].landedText.trim()) : juce::String ("no result"));
    }
    {   // factor, on a time control
        std::printf ("\n== { factor: F } multiplies a time control ==\n");
        MockComp plug;
        static_cast<NumParam*> (plug.getParameters()[1])->setDisplay (10.0);
        auto* d = new juce::DynamicObject(); d->setProperty ("Attack", deltaFactor (2.0));
        auto* st = new juce::DynamicObject(); st->setProperty ("deltas", juce::var (d));
        echojay::applySettings (plug, mapVar, juce::var (st));
        check (textOf (plug, 1) == "20.0 ms", "10 ms x 2 = 20 ms", textOf (plug, 1));
    }
    {   // positions, and its clamp
        std::printf ("\n== { positions: N } moves along the map's position list, clamped at the ends ==\n");
        MockComp plug;
        static_cast<StepParam*> (plug.getParameters()[2])->setPosition (1);   // "4:1"
        check (textOf (plug, 2) == "4:1", "fixture: the ratio reads 4:1", textOf (plug, 2));
        auto* d = new juce::DynamicObject(); d->setProperty ("Ratio", deltaPos (1));
        auto* st = new juce::DynamicObject(); st->setProperty ("deltas", juce::var (d));
        echojay::applySettings (plug, mapVar, juce::var (st));
        check (textOf (plug, 2) == "8:1", "+1 position moves one entry LATER", textOf (plug, 2));
        auto* d2 = new juce::DynamicObject(); d2->setProperty ("Ratio", deltaPos (9));
        auto* st2 = new juce::DynamicObject(); st2->setProperty ("deltas", juce::var (d2));
        echojay::applySettings (plug, mapVar, juce::var (st2));
        check (textOf (plug, 2) == "20:1", "...and a move past the end CLAMPS to the last position", textOf (plug, 2));
        auto* d3 = new juce::DynamicObject(); d3->setProperty ("Ratio", deltaPos (-9));
        auto* st3 = new juce::DynamicObject(); st3->setProperty ("deltas", juce::var (d3));
        echojay::applySettings (plug, mapVar, juce::var (st3));
        check (textOf (plug, 2) == "2:1", "...and past the start clamps to the first", textOf (plug, 2));
    }
    {   // the range clamp
        std::printf ("\n== a db delta is clamped to the map's range, never refused for leaving it ==\n");
        MockComp plug;
        static_cast<NumParam*> (plug.getParameters()[0])->setDisplay (-18.0);
        auto* d = new juce::DynamicObject(); d->setProperty ("Threshold", deltaDb (-100.0));
        auto* st = new juce::DynamicObject(); st->setProperty ("deltas", juce::var (d));
        echojay::applySettings (plug, mapVar, juce::var (st));
        check (textOf (plug, 0) == "-60.0 dB", "-18 - 100 lands at the bottom of [-60 .. 0], not manual", textOf (plug, 0));
    }
    {   // an absolute for the same control wins
        std::printf ("\n== an absolute value in controls beats a delta for the same control ==\n");
        MockComp plug;
        static_cast<NumParam*> (plug.getParameters()[0])->setDisplay (-18.0);
        auto* abs = new juce::DynamicObject(); abs->setProperty ("Threshold", -12.0);
        auto* d   = new juce::DynamicObject(); d->setProperty ("Threshold", deltaDb (-3.0));
        auto* st  = new juce::DynamicObject(); st->setProperty ("controls", juce::var (abs)); st->setProperty ("deltas", juce::var (d));
        const auto res = echojay::applySettings (plug, mapVar, juce::var (st));
        check (textOf (plug, 0) == "-12.0 dB", "the absolute lands and the delta is NOT applied on top", textOf (plug, 0));
        bool said = false;
        for (const auto& r : res) if (r.note.contains ("absolute value was sent")) said = true;
        check (said, "...and the dropped delta is reported, never silent");
    }
    {   // a control the delta cannot be placed on
        std::printf ("\n== a delta the control cannot take is left manual, with the reason ==\n");
        MockComp plug;
        auto* d = new juce::DynamicObject(); d->setProperty ("Nonesuch", deltaDb (-3.0));
        auto* st = new juce::DynamicObject(); st->setProperty ("deltas", juce::var (d));
        const auto res = echojay::applySettings (plug, mapVar, juce::var (st));
        check (res.size() == 1 && res[0].note.contains ("no mapped control"),
               "an unmapped name is named on the card, not silently dropped",
               res.size() == 1 ? res[0].note : juce::String ("no result"));
        auto* d2 = new juce::DynamicObject(); d2->setProperty ("Ratio", deltaDb (-3.0));
        auto* st2 = new juce::DynamicObject(); st2->setProperty ("deltas", juce::var (d2));
        const auto res2 = echojay::applySettings (plug, mapVar, juce::var (st2));
        check (res2.size() == 1 && res2[0].note.isNotEmpty() && textOf (plug, 2) == "2:1",
               "a db delta on a named-position control does not move it, and says why",
               res2.size() == 1 ? res2[0].note.substring (0, 90) : juce::String ("no result"));
    }

    std::printf ("\n%s  (%d failure(s))\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
