/*
    EedChorusProcessor.h  —  "EchoJay Chorus": modulated multi-voice delay.

    DSP in EedChorusEngine (delay lines swept by the shared LfoCore); this file is
    the dialable contract.

    No SHAPE param, deliberately. The LFO here is driving an interpolated read
    pointer, not a gain: a square or saw waveform in a delay time is a pitch jump
    on every cycle, which is a glitch rather than a sound. The engine pins the
    shape to sine, so a param that the device could not honour is not advertised.
*/

#pragma once

#include "EedChorusEngine.h"
#include "EedDeviceProcessor.h"

class EedChorusProcessor : public EedDeviceProcessor
{
public:
    EedChorusProcessor() = default;

    const juce::String getName() const override { return "EchoJay Chorus"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kRateHz   = "rate_hz";
    static constexpr const char* kSync     = "sync";
    static constexpr const char* kDivision = "sync_division";
    static constexpr const char* kDepth    = "depth";
    static constexpr const char* kDelayMs  = "delay_ms";
    static constexpr const char* kVoices   = "voices";
    static constexpr const char* kFeedback = "feedback";
    static constexpr const char* kMix      = "mix";

    echojay::ChorusEngine& engine() noexcept { return engine_; }

private:
    echojay::ChorusEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedChorusProcessor)
};
