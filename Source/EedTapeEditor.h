/*
    EedTapeEditor.h  —  the editor for "EchoJay Tape".

    A machine selector and ten dials on two rows, plus a hint line that reports
    what SPEED and the machine are actually doing to the head bump and the top
    end — a number in ips means nothing on its own, and the two frequencies it
    moves are the whole reason the knob exists.

    Every control is driven THROUGH the schema (setParamValue / getParamValue),
    so a knob turn and an AI move take the identical path.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedTapeMotionView.h"
#include "EedTapeProcessor.h"
#include "viz/HarmonicBars.h"
#include "viz/WaveshaperView.h"

#include <vector>

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

    // Only when the drive or the bias has moved — see the note on the
    // equivalent in EedSaturationEditor.
    void refreshTransfer();
    void refreshBars();

    EedTapeProcessor& proc_;

    // The machine selector, then two rows of dials:
    // Row 1: the machine's mechanics.   Row 2: what it does to the signal.
    juce::ComboBox                     modeBox_;
    echojay::device::EchoJayDeviceKnob speedKnob_, driveKnob_, biasKnob_, bumpKnob_, hissKnob_;
    echojay::device::EchoJayDeviceKnob wowKnob_, flutterKnob_, crosstalkKnob_, mixKnob_, outKnob_;

    echojay::viz::WaveshaperView shaper_;
    echojay::viz::HarmonicBars   bars_;
    EedTapeMotionView            motion_;

    std::vector<float> frame_;

    bool  suppressCallbacks_ = false;
    float lastHintSpeed_ = -1.0f;
    int   lastHintMode_  = -1;
    float lastDriveDb_   = -1.0f;
    float lastBias_      = -1000.0f;
    int   lastMachine_   = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTapeEditor)
};
