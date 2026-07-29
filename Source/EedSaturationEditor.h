/*
    EedSaturationEditor.h  —  the editor for "EchoJay Saturation".

    Four dials, a selector, and the Harmonic cluster's signature pair
    (VISUALS_PLAN.md): the transfer curve the DSP is actually using, with the
    signal's level riding it, and the harmonics it is actually producing.

    The two answer different questions, which is why both are here. The curve
    says what the shaper WILL do to a sample at any level; the bars say what it
    DID to this material. A device set to 24 dB of drive on a signal peaking at
    -30 dBFS has a dramatic curve and produces almost nothing, and only the pair
    together makes that visible.

    Every control is driven THROUGH the schema (setParamValue / getParamValue),
    so a knob turn and an AI move take the identical path and cannot disagree
    about clamping. The visuals are READ-ONLY and touch none of that.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedSaturationProcessor.h"
#include "viz/HarmonicBars.h"
#include "viz/WaveshaperView.h"

#include <vector>

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

    // Rebuilds the drawn curve, but ONLY when the curve or the drive has moved.
    // A std::function cannot be compared, so setTransfer unconditionally rebuilds
    // its path; calling it every frame would re-evaluate the shaper 161 times at
    // 15 Hz to redraw a picture that had not changed.
    void refreshTransfer();
    void refreshBars();

    EedSaturationProcessor& proc_;

    echojay::device::EchoJayDeviceKnob driveKnob_, toneKnob_, mixKnob_, outKnob_;
    juce::ComboBox                     typeBox_;

    echojay::viz::WaveshaperView shaper_;
    echojay::viz::HarmonicBars   bars_;

    // One analysis frame, sized ONCE in the constructor and reused every tick —
    // the editor's timer has no more business allocating than the audio thread.
    std::vector<float> frame_;

    echojay::harmonic::Curve lastCurve_    = echojay::harmonic::Curve::Soft;
    float                    lastDriveDb_  = -1.0f;
    bool                     haveTransfer_ = false;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedSaturationEditor)
};
