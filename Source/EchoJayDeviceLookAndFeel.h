/*
    EchoJayDeviceLookAndFeel.h  —  the shared look every built-in device wears
    (BUILTIN_SUITE_PLAN.md §2).

    Extracted verbatim from SurgicalEqEditor, which is where this look was
    designed. The EQ now uses these definitions rather than its own copies, which
    is the proof that they are sufficient: if the extraction had lost something,
    the EQ would show it.

    What lives here:
      * the palette              — aliased to EchoJayLookAndFeel::Colours, NOT
                                   re-declared, so a device and the host plugin
                                   can never drift to two different darks.
      * EchoJayDeviceKnob        — the filmstrip rotary with a caption above and
                                   an editable value underneath. This IS
                                   ChainWetKnob: the same photoreal 128-frame
                                   strip and cyan value arc as the MIX knob, so
                                   every dial in EchoJay is literally one control
                                   with one drawing path.
      * button / combo styling   — the dark fill + teal-glow-when-on treatment.
      * the layout metrics       — header height, row height, dial column size.
                                   Shared so two devices' headers line up exactly
                                   rather than approximately.

    A device editor that uses DeviceEditorBase gets all of this for free and only
    lays out its own controls.
*/

#pragma once

#include <JuceHeader.h>
#include "EchoJayLookAndFeel.h"
#include "ChainWetKnob.h"

#include <functional>

namespace echojay::device
{

// The palette is the plugin's palette. Aliased, never copied.
using Colours = EchoJayLookAndFeel::Colours;

// ---------------------------------------------------------------------------
// Layout metrics, shared so devices align with each other and with the EQ.
// ---------------------------------------------------------------------------
namespace metrics
{
    constexpr int kBarH  = 30;   // logo / global button line
    constexpr int kTopH  = 58;   // header band — a DIAL's height, not a button's,
                                 // so a device can put a dial in its header
                                 // without the row changing size.
    constexpr int kRowH  = 24;   // flat control height (combos, buttons)
    constexpr int kCapH  = 12;   // caption band inside a dial
    constexpr int kKnobW = 62;   // dial column width
    constexpr int kKnobH = 58;   // caption + filmstrip + readout
    constexpr int kPad   = 10;
}

inline juce::Font uiFont (float pt, bool bold = false)
{
    return juce::Font (juce::FontOptions (pt, bold ? juce::Font::bold
                                                   : juce::Font::plain));
}

// ---------------------------------------------------------------------------
// Shared control styling.
// ---------------------------------------------------------------------------
inline void styleButton (juce::TextButton& b, bool toggles)
{
    b.setClickingTogglesState (toggles);
    b.setColour (juce::TextButton::buttonColourId,   Colours::bg3);
    b.setColour (juce::TextButton::buttonOnColourId, Colours::blue);   // teal glow
    b.setColour (juce::TextButton::textColourOffId,  Colours::text2);
    b.setColour (juce::TextButton::textColourOnId,   Colours::blue2);
}

inline void styleCombo (juce::ComboBox& c)
{
    c.setColour (juce::ComboBox::backgroundColourId, Colours::bg3);
    c.setColour (juce::ComboBox::textColourId,       Colours::text);
}

// ---------------------------------------------------------------------------
// A labelled rotary in REAL UNITS: caption above, filmstrip knob, editable
// numeric readout below.
//
// Real units are the point. The 0..1 the filmstrip understands is mapped through
// a juce::NormalisableRange, so the control reads and writes dB / Hz / ms — the
// same units the ParamSchema advertises and the AI dials in. A knob that speaks
// 0..1 and a schema that speaks dB is how a UI and its automation drift apart.
// ---------------------------------------------------------------------------
class EchoJayDeviceKnob : public juce::Component
{
public:
    EchoJayDeviceKnob();

    // decimals/suffix drive the readout: "429 Hz", "16.5 dB", "1.00".
    void setSpec (double lo, double hi, double skewMidPoint,
                  int decimals, const juce::String& suffix,
                  const juce::String& caption, double defaultValue);

    void   setRealValue (double v);          // no callback
    double getRealValue() const noexcept { return value_; }
    void   setDimmed (bool d);

    std::function<void()> onValueChange;     // live during a drag / on typed entry

    // Optional custom readout formatter (must include its own units). Set it
    // BEFORE setSpec. Used by the EQ's FREQ dial, which wants the axis's own
    // "87 Hz" / "1.2k" form rather than a fixed number of decimals.
    std::function<juce::String(double)> formatValue;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    // Reuses the filmstrip; overrides only what is wet/dry-specific.
    struct Rotary : ChainWetKnob
    {
        juce::String tip;
        float        defaultNorm = 0.5f;
        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            setValue (defaultNorm, true);
            if (onGestureEnd) onGestureEnd();
        }
        void updateTooltip() override { if (tip.isNotEmpty()) setTooltip (tip); }
    };

    void refreshReadout();

    Rotary      knob_;
    juce::Label readout_;                    // double-click to type an exact value
    juce::String caption_, suffix_;
    juce::NormalisableRange<double> range_ { 0.0, 1.0 };
    double value_    = 0.0;
    int    decimals_ = 1;
    bool   dimmed_   = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EchoJayDeviceKnob)
};

} // namespace echojay::device
