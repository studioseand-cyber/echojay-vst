/*
    EedStereoWidthEditor.h  —  the editor for "EchoJay Stereo Width".

    Three dials and a layout. The EchoJay identity — logo, title, bypass,
    palette, filmstrip knobs, inline hosting — is inherited from
    DeviceEditorBase, not re-authored.

    The dials are driven THROUGH the schema (setParamValue / getParamValue)
    rather than by calling the engine directly, so a knob turn and an AI move
    take the identical path and cannot disagree about clamping.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedStereoWidthProcessor.h"

class EedStereoWidthEditor : public DeviceEditorBase,
                             private juce::Timer
{
public:
    explicit EedStereoWidthEditor (EedStereoWidthProcessor& p);
    ~EedStereoWidthEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();

    EedStereoWidthProcessor& proc_;

    echojay::device::EchoJayDeviceKnob widthKnob_, bassMonoKnob_, trimKnob_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedStereoWidthEditor)
};
