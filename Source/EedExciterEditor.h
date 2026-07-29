/*
    EedExciterEditor.h  —  the editor for "EchoJay Exciter".

    Four dials and a mode selector, on DeviceEditorBase. Every control is driven
    THROUGH the schema (setParamValue / getParamValue), so a knob turn and an AI
    move take the identical path.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedExciterProcessor.h"

class EedExciterEditor : public DeviceEditorBase,
                         private juce::Timer
{
public:
    explicit EedExciterEditor (EedExciterProcessor& p);
    ~EedExciterEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();

    EedExciterProcessor& proc_;

    echojay::device::EchoJayDeviceKnob freqKnob_, amountKnob_, mixKnob_, outKnob_;
    juce::ComboBox                     modeBox_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedExciterEditor)
};
