/*
    EedPitchProcessor.h  —  "EchoJay Pitch", device #22
    (PITCH_CORRECTION_SPEC.md), at build phase P0: DETECTION ONLY.

    In this phase the device is a READER: processBlock feeds the YIN engine and
    returns the buffer untouched, bit for bit, at every setting. No shifting,
    no scale logic, no correction — those are P1+ and gate on this detector
    being solid, because a correction artefact caused by a detection error
    looks exactly like a PSOLA bug and gets debugged as one.

    What it shows (the P0 debug readout): detected pitch as note + cents, f0,
    confidence, the voiced/unvoiced flag, and how often the octave-error guard
    has fired — the number the spec says to log.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedPitchEngine.h"

class EedPitchProcessor : public EedDeviceProcessor
{
public:
    EedPitchProcessor() = default;

    const juce::String getName() const override { return "EchoJay Pitch"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical ids in one place, so schema, apply path and editor cannot
    // drift apart through a typo.
    static constexpr const char* kVoiceType  = "voice_type";
    static constexpr const char* kResetStats = "reset_stats";

    echojay::PitchEngine&       engine() noexcept       { return engine_; }
    const echojay::PitchEngine& engine() const noexcept { return engine_; }

private:
    echojay::PitchEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPitchProcessor)
};
