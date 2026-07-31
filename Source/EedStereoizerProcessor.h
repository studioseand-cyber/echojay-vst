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
#include "viz/VizTap.h"

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

    // The depth pass (DEVICE_DEPTH_PLAN.md, Stereo): the widening character.
    static constexpr const char* kMode        = "mode";

    echojay::StereoEngine& engine() noexcept { return engine_; }

    // The editor's goniometer (VISUALS_PLAN.md Phase V0, the ring-tap path) —
    // the same two lines Stereo Width carries, and for the same reason: width
    // cannot be drawn from a parameter. It is even less drawable here, because
    // this device widens with a Haas delay: how much image 15 ms actually buys
    // depends entirely on what is already in the side signal, and only the
    // samples know that.
    //
    // It holds the OUTPUT, post-engine and therefore post-MIX — the widening is
    // the thing being watched, not the input it started from.
    const echojay::viz::ScopeTap& scopeTap() const noexcept { return scopeTap_; }

private:
    echojay::StereoEngine engine_;
    echojay::viz::ScopeTap scopeTap_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedStereoizerProcessor)
};
