/*
    EedSaturationEditor.h  —  the editor for "EchoJay Saturation".

    The point of DeviceEditorBase: this class is four dials, a selector and a
    layout. The EchoJay identity — logo, title, bypass, palette, filmstrip knobs,
    inline hosting — is inherited, not re-authored.

    Every control is driven THROUGH the schema (setParamValue / getParamValue)
    rather than by calling the engine directly. A knob turn and an AI move then
    take the identical path, so the two cannot disagree about clamping, and a
    param that is dialable is automatically also turnable.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedSaturationProcessor.h"

class EedSaturationEditor : public DeviceEditorBase,
                            private juce::Timer
{
public:
    explicit EedSaturationEditor (EedSaturationProcessor& p);
    ~EedSaturationEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();

    EedSaturationProcessor& proc_;

    echojay::device::EchoJayDeviceKnob driveKnob_, toneKnob_, mixKnob_, outKnob_;
    juce::ComboBox                     typeBox_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedSaturationEditor)
};
