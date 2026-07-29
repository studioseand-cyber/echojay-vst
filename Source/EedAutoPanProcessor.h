/*
    EedAutoPanProcessor.h  —  "EchoJay Auto Pan": LFO-steered stereo placement.

    DSP in EedAutoPanEngine (the shared pan law on the shared LfoCore); this file
    is the dialable contract.

    No MIX param, deliberately. Panning is a placement, not a parallel effect: a
    50% blend of "panned left" and "centred" is just "panned less", which DEPTH
    already says, and more precisely. The way to make an auto-pan subtler is to
    turn its depth down.
*/

#pragma once

#include "EedAutoPanEngine.h"
#include "EedDeviceProcessor.h"

class EedAutoPanProcessor : public EedDeviceProcessor
{
public:
    EedAutoPanProcessor() = default;

    const juce::String getName() const override { return "EchoJay Auto Pan"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kRateHz      = "rate_hz";
    static constexpr const char* kSync        = "sync";
    static constexpr const char* kDivision    = "sync_division";
    static constexpr const char* kDepth       = "depth";
    static constexpr const char* kShape       = "shape";
    static constexpr const char* kStereoPhase = "stereo_phase";

    echojay::AutoPanEngine& engine() noexcept { return engine_; }

private:
    echojay::AutoPanEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedAutoPanProcessor)
};
