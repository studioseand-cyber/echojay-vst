/*
    EedPhaseInvertProcessor.h  —  "EchoJay Phase Invert": per-channel polarity.

    The second Wave 0 proof device. There is deliberately no EedPhaseInvertEngine:
    the DSP is a sign flip, and wrapping that in a core with its own g++ test
    would be ceremony rather than coverage. The device template says to add an
    engine when there is DSP worth testing in isolation — this is the documented
    exception, not an oversight.

    Two independent switches rather than one. A single "polarity" control handles
    the common case (a mic flipped against another), but the reason you reach for
    this device at all is often a channel-specific problem — a mis-wired cable on
    one side of a stereo pair — and a one-knob version cannot fix that.
*/

#pragma once

#include "EedDeviceProcessor.h"

class EedPhaseInvertProcessor : public EedDeviceProcessor
{
public:
    EedPhaseInvertProcessor() = default;

    const juce::String getName() const override { return "EchoJay Phase Invert"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kInvertL = "invert_left";
    static constexpr const char* kInvertR = "invert_right";

private:
    // Atomics, not a lock: the audio thread reads these every block while the
    // message thread (or an AI move) writes them.
    std::atomic<bool> invertL_ { false };
    std::atomic<bool> invertR_ { false };

    // Flipping polarity is a jump from +1 to -1 — the largest discontinuity a
    // sample can have, and an audible click on anything but silence. The
    // multiplier ramps instead, so a flip is a ~5 ms dip through zero. Same
    // no-click discipline as GainEngine; an AI move can land mid-playback.
    float coeffL_    = 1.0f;      // audio thread only
    float coeffR_    = 1.0f;
    float rampCoeff_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPhaseInvertProcessor)
};
