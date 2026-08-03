/*
    EedExciterProcessor.h  —  "EchoJay Exciter": harmonics on the highs only.

    A face on the Harmonic cluster's core (BUILTIN_SUITE_PLAN.md §4). The DSP is
    in ExciterEngine; this file is the knobs, the contract and the registry entry.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedExciterEngine.h"

class EedExciterProcessor : public EedDeviceProcessor
{
public:
    EedExciterProcessor();

    const juce::String getName() const override { return "EchoJay Exciter"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kFreqHz   = "freq_hz";
    static constexpr const char* kAmount   = "amount";
    static constexpr const char* kMode     = "mode";
    static constexpr const char* kFocus    = "focus";
    static constexpr const char* kMix      = "mix";
    static constexpr const char* kOutputDb = "output_db";

    echojay::ExciterEngine& engine() noexcept { return engine_; }

private:
    echojay::ExciterEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedExciterProcessor)
};
