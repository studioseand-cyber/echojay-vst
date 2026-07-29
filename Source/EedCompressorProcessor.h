/*
    EedCompressorProcessor.h  —  "EchoJay Compressor".

    The first face on the shared dynamics core (BUILTIN_SUITE_PLAN.md §4). There
    is deliberately no EedCompressorEngine: DynamicsCore IS the engine, JUCE-free
    and g++-tested in test/dynamics_core_test.cpp, and wrapping it in a per-device
    engine that only forwards setters would add a layer that can drift from the
    schema without adding anything the DSP needs. Same documented exception as
    EedPhaseInvertProcessor, for the same reason: a layer that cannot be wrong is
    better than a layer that merely happens to be right.

    DETECTOR CHOICE. RMS with a 10 ms window, fixed rather than dialable. Peak
    detection makes a compressor chase individual transients, which is what a
    LIMITER is for; RMS tracks loudness, which is what makes a compressor sound
    like it is riding a fader. Exposing the choice would be one more knob whose
    wrong setting makes the device sound broken, so the device commits.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDynamicsCore.h"

class EedCompressorProcessor : public EedDeviceProcessor
{
public:
    EedCompressorProcessor();

    const juce::String getName() const override { return "EchoJay Compressor"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical param ids — used by the editor so a typo cannot silently
    // decouple a knob from the schema entry it drives.
    static constexpr const char* kThresholdDb = "threshold_db";
    static constexpr const char* kRatio       = "ratio";
    static constexpr const char* kAttackMs    = "attack_ms";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kKneeDb      = "knee_db";
    static constexpr const char* kMakeupDb    = "makeup_db";
    static constexpr const char* kMix         = "mix";

    // For the editor's GR meter. Negative dB, 0 when not reducing.
    float gainReductionDb() const noexcept { return core_.gainReductionDb(); }

    // The level the detector is seeing this instant, dBFS. Same one-float
    // contract as the line above. Only a whisper of extra brightness on the
    // curve's glow now — the shape itself comes from the histogram below.
    float detectorLevelDb() const noexcept { return core_.detectorLevelDb(); }

    // Where the signal LIVES on that curve — the dwell histogram behind the
    // transfer curve's glow. Same never-block contract as the floats above,
    // published whole so the shape is never half of two different moments.
    const echojay::dyn::DwellTap& dwellHistogram() const noexcept
    {
        return core_.dwellHistogram();
    }

    // The transfer curve the editor draws is drawn from the core's OWN gain
    // computer, not from a second copy of "threshold, ratio, knee" in UI code.
    // Read-only use: the editor dials through setParamValue like everything
    // else, so there is no second write path into the DSP.
    const echojay::DynamicsCore& core() const noexcept { return core_; }

private:
    echojay::DynamicsCore core_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedCompressorProcessor)
};
