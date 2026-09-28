/*
  EjmapFixtureReadout.h

  MISMATCH-LIST ITEM 12, AGREED 28 Sep: readouts in the fixture. A control whose value
  differs between two FRESH instances is a READOUT (a meter exposed as a parameter), and
  it has no value on instantiate. The shape:

    fixture:  "readoutCheck": { "method": "instantiate_twice", "instances": 2 }
              present exactly when the check RAN. Absent means "never checked", never
              "no readouts".
    control:  "readout": { "samples": [a, b], "displays": [ta, tb] }
              only on a control that moved.
    readout:  defaultOnInstantiate.normalised = null, .display = null, the note says why,
              and declaredDefault SURVIVES: what the plugin calls its default is still a
              fact.

  NULL, NOT THE FIRST SAMPLE. defaultOnInstantiate means "the value the plugin holds on
  instantiate", and a meter has no such value. Null is the truthful encoding. The first
  sample is a number that looks like a measurement and is not.

  TWO FUNCTIONS, DELIBERATELY SEPARATE:
    applySchema    turns a fixture in the MEASURED shape (what the driver composes and
                   what the reproduction score compares) into the item-12 shape (what is
                   written). It is pure.
    checkEmission  asserts an emitted fixture against the measured one, WITHOUT calling
                   applySchema. It returns at most ONE line per check, so a single defect
                   reddens exactly the check it breaks:
                     C1 readoutCheck present with the agreed content iff the check ran
                     C2 readout on exactly the controls that moved, carrying their samples
                     C3 normalised AND display null on exactly those
                     C4 declaredDefault survives on them
                     C5 every other control's defaultOnInstantiate untouched
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <vector>
#include <set>

namespace ejmap::fixturereadout
{

struct Moved { int index = -1; double a = 0, b = 0; juce::String ta, tb; };

inline const char* kMethod = "instantiate_twice";
inline const char* kReadoutNote = "a readout: its value moved between two fresh instances, so it has no instantiate value";

inline juce::var readoutCheckVar()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("method", kMethod);
    o->setProperty ("instances", 2);
    return juce::var (o);
}

inline juce::var applySchema (const juce::var& measured, const std::vector<Moved>& moved, bool checkRan)
{
    auto out = measured.clone();
    auto* o = out.getDynamicObject();
    if (o == nullptr) return out;
    if (checkRan) o->setProperty ("readoutCheck", readoutCheckVar());
    if (auto* controls = o->getProperty ("controls").getArray())
        for (auto& c : *controls)
        {
            const int index = (int) c.getProperty ("index", -1);
            for (const auto& m : moved)
            {
                if (m.index != index) continue;
                auto* r = new juce::DynamicObject();
                r->setProperty ("samples", juce::Array<juce::var> { m.a, m.b });
                r->setProperty ("displays", juce::Array<juce::var> { m.ta, m.tb });
                if (auto* co = c.getDynamicObject()) co->setProperty ("readout", juce::var (r));
                if (auto* d = c.getProperty ("defaultOnInstantiate", juce::var()).getDynamicObject())
                {
                    d->setProperty ("normalised", juce::var());
                    d->setProperty ("display", juce::var());
                    d->setProperty ("note", kReadoutNote);
                }
            }
        }
    return out;
}

inline juce::StringArray checkEmission (const juce::var& emitted, const juce::var& measured,
                                        const std::vector<Moved>& moved, bool checkRan)
{
    juce::StringArray fail;
    std::set<int> movedIdx;
    for (const auto& m : moved) movedIdx.insert (m.index);

    // C1
    {
        const auto rc = emitted.getProperty ("readoutCheck", juce::var());
        const bool present = emitted.getDynamicObject() != nullptr && emitted.getDynamicObject()->hasProperty ("readoutCheck");
        if (checkRan && ! (present && rc.getProperty ("method", "").toString() == kMethod && (int) rc.getProperty ("instances", 0) == 2))
            fail.add ("C1 readoutCheck: the check ran but the fixture does not carry {method instantiate_twice, instances 2}");
        if (! checkRan && present)
            fail.add ("C1 readoutCheck: present although the check did not run (absence must mean never checked)");
    }

    std::map<int, juce::var> em, me;
    if (auto* a = emitted.getProperty ("controls", juce::var()).getArray())  for (const auto& c : *a) em[(int) c.getProperty ("index", -1)] = c;
    if (auto* a = measured.getProperty ("controls", juce::var()).getArray()) for (const auto& c : *a) me[(int) c.getProperty ("index", -1)] = c;

    juce::StringArray c2, c3, c4, c5;
    for (const auto& [idx, c] : em)
    {
        const bool isMoved = movedIdx.count (idx) > 0;
        const auto* co = c.getDynamicObject();
        const bool hasReadout = co != nullptr && co->hasProperty ("readout");
        const auto d = c.getProperty ("defaultOnInstantiate", juce::var());
        const auto md = me.count (idx) ? me[idx].getProperty ("defaultOnInstantiate", juce::var()) : juce::var();
        if (isMoved != hasReadout) c2.add (juce::String (idx));
        if (isMoved)
        {
            for (const auto& m : moved)
                if (m.index == idx && hasReadout)
                {
                    const auto s = c.getProperty ("readout", juce::var()).getProperty ("samples", juce::var());
                    if (! (s.isArray() && s.size() == 2 && (double) s[0] == m.a && (double) s[1] == m.b)) c2.add (juce::String (idx) + " samples");
                }
            if (! d.getProperty ("normalised", 0).isVoid() || ! d.getProperty ("display", 0).isVoid()) c3.add (juce::String (idx));
            if ((double) d.getProperty ("declaredDefault", -1e9) != (double) md.getProperty ("declaredDefault", -2e9)) c4.add (juce::String (idx));
        }
        else if (juce::JSON::toString (d, true) != juce::JSON::toString (md, true))
            c5.add (juce::String (idx));
    }
    if (! c2.isEmpty()) fail.add ("C2 readout placement wrong on control(s) " + c2.joinIntoString (", "));
    if (! c3.isEmpty()) fail.add ("C3 normalised/display not null on readout control(s) " + c3.joinIntoString (", "));
    if (! c4.isEmpty()) fail.add ("C4 declaredDefault lost or changed on readout control(s) " + c4.joinIntoString (", "));
    if (! c5.isEmpty()) fail.add ("C5 defaultOnInstantiate altered on non-readout control(s) " + c5.joinIntoString (", "));
    return fail;
}

} // namespace ejmap::fixturereadout
