/*
    EedGainEditor.h  —  the editor for "EchoJay Gain".

    The whole point of DeviceEditorBase: this class is two dials and a layout.
    The EchoJay identity — logo, title, bypass, palette, filmstrip knobs, inline
    hosting — is inherited, not re-authored.

    The dials are driven THROUGH the schema (setParamValue / getParamValue) rather
    than by calling the engine directly. That means a knob turn and an AI move
    take the identical path, so the two cannot disagree about clamping, and a
    param that is dialable is automatically also turnable.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedGainProcessor.h"

class EedGainEditor : public DeviceEditorBase,
                      private juce::Timer
{
public:
    explicit EedGainEditor (EedGainProcessor& p);
    ~EedGainEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void pushToProcessor();
    void syncFromProcessor();

    EedGainProcessor& proc_;

    echojay::device::EchoJayDeviceKnob levelKnob_, panKnob_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGainEditor)
};
