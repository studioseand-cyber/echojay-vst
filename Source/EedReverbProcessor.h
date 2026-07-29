/*
    EedReverbProcessor.h  —  "EchoJay Reverb": an FDN reverb, fully dialable.

    Wave 1, Time cluster (BUILTIN_SUITE_PLAN.md) — the hardest device in the
    suite. The DSP lives in ReverbEngine (JUCE-free, g++-tested); this file is
    the device: its schema, its id->knob map, and its registry entry.

    NINE dialable params — size, decay, predelay, damping, low cut, width,
    early/late balance, modulation depth and mix. That is the whole control
    surface: there is nothing on this reverb a user can turn that the model
    cannot set exactly.

    LATENCY: NONE, and this is worth being explicit about because a reverb is
    exactly the kind of device that usually has some. There is no lookahead here
    and no oversampling; predelay is a delay applied to the wet path, so the dry
    signal still leaves on the same sample it arrived. getLatencySamples() stays
    0 and the host needs no delay compensation for this device.

    getTailLengthSeconds() is overridden, though — a reverb that reports no tail
    gets truncated by an offline render at the end of every region.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedReverbEngine.h"

class EedReverbProcessor : public EedDeviceProcessor
{
public:
    EedReverbProcessor() = default;

    const juce::String getName() const override { return "EchoJay Reverb"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    double getTailLengthSeconds() const override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical param ids — used by the editor so a typo cannot silently
    // decouple a knob from the schema entry it is supposed to drive.
    static constexpr const char* kSize       = "size";
    static constexpr const char* kDecayS     = "decay_s";
    static constexpr const char* kPredelayMs = "predelay_ms";
    static constexpr const char* kDamping    = "damping";
    static constexpr const char* kLowCutHz   = "low_cut_hz";
    static constexpr const char* kWidth      = "width";
    static constexpr const char* kEarlyLate  = "early_late";
    static constexpr const char* kModDepth   = "mod_depth";
    static constexpr const char* kMix        = "mix";

    echojay::ReverbEngine& engine() noexcept { return engine_; }
    const echojay::ReverbEngine& engine() const noexcept { return engine_; }

private:
    echojay::ReverbEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedReverbProcessor)
};
