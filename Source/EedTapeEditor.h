/*
    EedTapeEditor.h  —  the editor for "EchoJay Tape".

    Eight dials on two rows, plus a hint line that reports what SPEED is actually
    doing to the head bump and the top end — a number in ips means nothing on its
    own, and the two frequencies it moves are the whole reason the knob exists.

    Every control is driven THROUGH the schema (setParamValue / getParamValue),
    so a knob turn and an AI move take the identical path.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedTapeProcessor.h"

class EedTapeEditor : public DeviceEditorBase,
                      private juce::Timer
{
public:
    explicit EedTapeEditor (EedTapeProcessor& p);
    ~EedTapeEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();
    void refreshSpeedHint();

    EedTapeProcessor& proc_;

    // Row 1: the machine.   Row 2: what it does to the signal.
    echojay::device::EchoJayDeviceKnob speedKnob_, driveKnob_, biasKnob_, bumpKnob_;
    echojay::device::EchoJayDeviceKnob wowKnob_, flutterKnob_, mixKnob_, outKnob_;

    bool  suppressCallbacks_ = false;
    float lastHintSpeed_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTapeEditor)
};
