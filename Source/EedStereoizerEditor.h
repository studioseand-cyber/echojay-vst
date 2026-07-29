/*
    EedStereoizerEditor.h  —  the editor for "EchoJay Stereoizer".

    Four dials and a layout; everything else is inherited from DeviceEditorBase.
    The dials are driven THROUGH the schema (setParamValue / getParamValue), so a
    knob turn and an AI move take the identical path.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedStereoizerProcessor.h"

class EedStereoizerEditor : public DeviceEditorBase,
                            private juce::Timer
{
public:
    explicit EedStereoizerEditor (EedStereoizerProcessor& p);
    ~EedStereoizerEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();

    EedStereoizerProcessor& proc_;

    echojay::device::EchoJayDeviceKnob widthKnob_, haasKnob_, monoKnob_, mixKnob_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedStereoizerEditor)
};
