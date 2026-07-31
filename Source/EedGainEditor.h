/*
    EedGainEditor.h  —  the editor for "EchoJay Gain".

    The whole point of DeviceEditorBase: this class is two dials and a layout.
    The EchoJay identity — logo, title, bypass, palette, filmstrip knobs, inline
    hosting — is inherited, not re-authored.

    The dials are driven THROUGH the schema (setParamValue / getParamValue) rather
    than by calling the engine directly. That means a knob turn and an AI move
    take the identical path, so the two cannot disagree about clamping, and a
    param that is dialable is automatically also turnable.

    The visualisation is two echojay::viz::LevelMeter, IN above OUT (VISUALS_PLAN
    .md, Utility). This is the float-tap path rather than the analytic one: a
    level is a property of the SIGNAL, not of the params, so unlike the delay's
    taps or the reverb's envelope it cannot be computed from the schema.

    Stacking them rather than putting them side by side is the whole point of the
    device: the question is not "how loud is the output", it is "what did this
    move DO", and that is a comparison. Two bars on a shared scale, one directly
    above the other, answer it at a glance; side by side, with separate scales,
    they do not.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedGainProcessor.h"
#include "viz/LevelMeter.h"

class EedGainEditor : public DeviceEditorBase,
                      private juce::Timer
{
public:
    explicit EedGainEditor (EedGainProcessor& p);
    ~EedGainEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;

private:
    void timerCallback() override;
    void pushToProcessor();
    void syncFromProcessor();
    void refreshMeters();
    void refreshModeState();

    bool midSideActive() const;

    EedGainProcessor& proc_;

    echojay::device::EchoJayDeviceKnob levelKnob_, panKnob_;

    // The depth pass: MID/SIDE join the dial row in mid_side mode only; the
    // switch strip (mono sum + per-channel polarity) is mode-independent.
    echojay::device::EchoJayDeviceKnob midKnob_, sideKnob_;
    juce::ComboBox                     modeBox_;
    juce::TextButton                   monoBtn_ { "MONO" },
                                       phaseLBtn_ { "INV L" },
                                       phaseRBtn_ { "INV R" };

    echojay::viz::LevelMeter inMeter_, outMeter_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGainEditor)
};
