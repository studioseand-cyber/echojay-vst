/*
    EedStereoizerProcessor.h  —  "EchoJay Stereoizer": width, Haas, mono maker, mix.

    The creative face of the shared StereoEngine (BUILTIN_SUITE_PLAN.md §4,
    Wave 1 Session A). Its sibling "EchoJay Stereo Width" is the SAME engine
    with the corrective knobs exposed instead; between them they are the whole
    Stereo cluster, and the DSP was written once.

    MONO SAFETY IS THE POINT of this device. A conventional Haas widener delays
    one channel outright, which combs the moment anyone folds it down. This one
    only ever rewrites the SIDE signal, and L + R = 2M regardless of side, so the
    fold-down is bit-for-bit the fold-down of the input at any setting of any
    knob. StereoEngine's header explains the construction; test/
    stereo_engine_test.cpp pins it.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedStereoEngine.h"

class EedStereoizerProcessor : public EedDeviceProcessor
{
public:
    EedStereoizerProcessor();

    const juce::String getName() const override { return "EchoJay Stereoizer"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical param ids — used by the editor so a typo cannot silently
    // decouple a knob from the schema entry it is supposed to drive.
    static constexpr const char* kWidth       = "width";
    static constexpr const char* kHaasMs      = "haas_ms";
    static constexpr const char* kMonoMakerHz = "mono_maker_hz";
    static constexpr const char* kMix         = "mix";

    echojay::StereoEngine& engine() noexcept { return engine_; }

private:
    echojay::StereoEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedStereoizerProcessor)
};
