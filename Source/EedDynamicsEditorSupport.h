/*
    EedDynamicsEditorSupport.h  —  the two things all six Dynamics editors do.

    Cluster-local on purpose. It could live in EchoJayDeviceLookAndFeel.h, but
    that is a SHARED file and the parallel-session rule (BUILTIN_SUITE_PLAN.md
    "Rules that keep parallel work safe") is that a cluster adds its own files
    rather than growing shared ones. Six near-identical copies of a knob-row
    layout would be worse than one cluster-local header, and a shared-file edit
    would be worse than both.

    1. bindKnob() — wire a filmstrip dial to a schema entry. The range, the
       default and the double-click reset all come FROM the schema, so the knob
       physically cannot travel somewhere the AI is not allowed to dial. Writes
       go back through setParamValue, which is the same path an AI move takes.

    2. layoutKnobRow() — place N dials evenly across a strip, wrapping onto a
       second row when the rack has given the editor less width than the dials
       need. That wrap is the inline-hosting contract: an editor laid out
       narrower than its default must degrade, not overlap.

    3. bindChoiceBox() / bindToggle() — the same wiring for a MODE selector and
       for an on/off switch. Added by the depth pass, when four of the six faces
       grew a character mode: the items in a combo ARE the schema's choices, in
       the schema's order, so the list a human sees and the list the model is
       taught cannot drift apart, and a click and a dialled `mode` cannot end up
       meaning different things.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedDeviceProcessor.h"

namespace echojay::device
{

// How a dial presents one param. Range/default are NOT here — they come from
// the schema, and duplicating them is exactly the drift this avoids.
// Plain const char* rather than juce::String so a device's spec table is a POD
// array of literals with no static construction at all.
struct KnobSpec
{
    const char* id;
    const char* caption;
    const char* suffix;       // " dB", " ms", " %", ":1", "" ...
    int         decimals = 1;
    double      skewMid  = 0.0;   // 0 = linear; otherwise the value at mid-travel
};

// Returns false when the id is not in the schema — which is a device bug, not a
// user-visible condition, so the caller asserts rather than degrading.
//
// `suppressFlag` is taken as a POINTER, not a reference. A reference parameter
// captured by a lambda that outlives this call is a dangling reference to a
// function argument; it happens to work often enough to ship and then fails
// somewhere unrelated. The pointer is captured by value and stays valid because
// it points at the editor's own member.
inline bool bindKnob (EchoJayDeviceKnob& knob,
                      const KnobSpec& k,
                      const echojay::ParamSchema& schema,
                      EedDeviceProcessor& proc,
                      const bool* suppressFlag)
{
    const auto* spec = schema.find (k.id);
    jassert (spec != nullptr);
    if (spec == nullptr) return false;

    knob.setSpec (spec->min, spec->max, k.skewMid, k.decimals,
                  juce::String (k.suffix), juce::String (k.caption), spec->def);
    knob.setRealValue (proc.getParamValue (juce::String (k.id)));

    const juce::String id (k.id);
    auto* knobPtr = &knob;
    auto* procPtr = &proc;
    knob.onValueChange = [knobPtr, id, procPtr, suppressFlag]
    {
        if (suppressFlag != nullptr && *suppressFlag) return;
        procPtr->setParamValue (id, knobPtr->getRealValue());
    };
    return true;
}

// Pull the processor's current values back into the dials. Only writes what
// actually moved: setRealValue would otherwise fight a drag in progress by
// snapping the knob to the value it just sent.
//
// Takes an array of POINTERS so it works directly off a juce::OwnedArray's raw
// data — the knobs are non-copyable, so an owning container of values is not an
// option and every caller ends up holding pointers anyway.
inline void syncKnobs (EchoJayDeviceKnob* const* knobs, const KnobSpec* specs,
                       int count, EedDeviceProcessor& proc)
{
    for (int i = 0; i < count; ++i)
    {
        const double v = proc.getParamValue (juce::String (specs[i].id));
        if (std::abs (v - knobs[i]->getRealValue()) > 1.0e-4)
            knobs[i]->setRealValue (v);
    }
}

// ---------------------------------------------------------------------------
// A CHOICE param as a combo box. Item ids are index+1, because JUCE reserves 0
// for "nothing selected" — so the wire value and the item id differ by one, in
// exactly one place, here.
//
// Returns false when the id is not a choice param in this device's schema, which
// is a device bug rather than a user-visible condition.
// ---------------------------------------------------------------------------
inline bool bindChoiceBox (juce::ComboBox& box,
                           const char* id,
                           const echojay::ParamSchema& schema,
                           EedDeviceProcessor& proc,
                           const bool* suppressFlag)
{
    const auto* spec = schema.find (id);
    jassert (spec != nullptr && ! spec->choices.empty());
    if (spec == nullptr || spec->choices.empty()) return false;

    styleCombo (box);
    box.clear (juce::dontSendNotification);

    for (std::size_t i = 0; i < spec->choices.size(); ++i)
        box.addItem (juce::String (spec->choices[i]).toUpperCase(), (int) i + 1);

    const juce::String pid (id);
    box.setSelectedId ((int) std::lround (proc.getParamValue (pid)) + 1,
                       juce::dontSendNotification);

    auto* boxPtr  = &box;
    auto* procPtr = &proc;
    box.onChange = [boxPtr, pid, procPtr, suppressFlag]
    {
        if (suppressFlag != nullptr && *suppressFlag) return;
        procPtr->setParamValue (pid, (double) (boxPtr->getSelectedId() - 1));
    };
    return true;
}

// Follow the processor: the AI can move a mode while the editor is open, so the
// selector is a view of the param and never the only thing that writes it.
// Returns true when the selection actually changed, so a caller can relayout.
inline bool syncChoiceBox (juce::ComboBox& box, const char* id, EedDeviceProcessor& proc)
{
    const int want = (int) std::lround (proc.getParamValue (juce::String (id))) + 1;
    if (box.getSelectedId() == want) return false;

    box.setSelectedId (want, juce::dontSendNotification);
    return true;
}

// A boolean param as a latching button. The text is fixed and the STATE is the
// lit/unlit look, which is the one unambiguous way to label a two-state control:
// a button whose text changes to its action ("SPLIT" / "WIDE") has to be read
// twice to answer "which is it now".
inline bool bindToggle (juce::TextButton& btn,
                        const char* id,
                        const juce::String& text,
                        const echojay::ParamSchema& schema,
                        EedDeviceProcessor& proc,
                        const bool* suppressFlag)
{
    const auto* spec = schema.find (id);
    jassert (spec != nullptr);
    if (spec == nullptr) return false;

    styleButton (btn, true);
    btn.setButtonText (text);
    btn.setToggleState (proc.getParamValue (juce::String (id)) >= 0.5,
                        juce::dontSendNotification);

    const juce::String pid (id);
    auto* btnPtr  = &btn;
    auto* procPtr = &proc;
    btn.onClick = [btnPtr, pid, procPtr, suppressFlag]
    {
        if (suppressFlag != nullptr && *suppressFlag) return;
        procPtr->setParamValue (pid, btnPtr->getToggleState() ? 1.0 : 0.0);
    };
    return true;
}

inline bool syncToggle (juce::TextButton& btn, const char* id, EedDeviceProcessor& proc)
{
    const bool want = proc.getParamValue (juce::String (id)) >= 0.5;
    if (btn.getToggleState() == want) return false;

    btn.setToggleState (want, juce::dontSendNotification);
    return true;
}

// Place `count` dials across `row`, wrapping to further rows when they do not
// fit. Returns the space left below them.
inline juce::Rectangle<int> layoutKnobRow (juce::Rectangle<int> area,
                                           EchoJayDeviceKnob* const* knobs, int count,
                                           int gap = 8)
{
    if (count <= 0 || area.isEmpty()) return area;

    // How many fit on one line at the shared dial width, at least one so a very
    // narrow rack squeezes rather than divides by zero.
    const int perRow = juce::jmax (1, (area.getWidth() + gap) / (metrics::kKnobW + gap));
    const int rows   = (count + perRow - 1) / perRow;
    const int rowH   = juce::jmin (metrics::kKnobH,
                                   juce::jmax (1, area.getHeight() / juce::jmax (1, rows)));

    int placed = 0;
    for (int r = 0; r < rows && placed < count; ++r)
    {
        const int inThisRow = juce::jmin (perRow, count - placed);
        const int wantW     = inThisRow * metrics::kKnobW + (inThisRow - 1) * gap;

        auto line = area.removeFromTop (rowH);
        auto strip = line.withSizeKeepingCentre (juce::jmin (wantW, line.getWidth()),
                                                 line.getHeight());

        const int colW = juce::jmax (1, (strip.getWidth() - (inThisRow - 1) * gap) / inThisRow);
        for (int i = 0; i < inThisRow; ++i)
        {
            knobs[placed++]->setBounds (strip.removeFromLeft (colW));
            if (i < inThisRow - 1) strip.removeFromLeft (gap);
        }
    }

    return area;
}

} // namespace echojay::device
