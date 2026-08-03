/*
    EedPhaserProcessor.h  —  "EchoJay Phaser": swept allpass cascade.

    DSP in EedPhaserEngine (an allpass chain whose corner is swept by the shared
    LfoCore); this file is the dialable contract.

    No SHAPE param, for the same reason as Chorus: the LFO is driving a filter
    coefficient, and a stepped waveform in a filter coefficient is a click rather
    than a sound. The engine pins the shape to sine, so nothing is advertised that
    the device would not honour.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedPhaserEngine.h"

class EedPhaserProcessor : public EedDeviceProcessor
{
public:
    EedPhaserProcessor() = default;

    const juce::String getName() const override { return "EchoJay Phaser"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kRateHz     = "rate_hz";
    static constexpr const char* kSync       = "sync";
    static constexpr const char* kDivision   = "sync_division";
    static constexpr const char* kDepth      = "depth";
    static constexpr const char* kStages     = "stages";
    static constexpr const char* kFeedback   = "feedback";
    static constexpr const char* kCentreFreq = "center_freq";
    static constexpr const char* kMix        = "mix";
    static constexpr const char* kMode       = "mode";
    static constexpr const char* kSpread     = "stereo_spread";

    echojay::PhaserEngine& engine() noexcept { return engine_; }

private:
    echojay::PhaserEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPhaserProcessor)
};
