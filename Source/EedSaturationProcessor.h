/*
    EedSaturationProcessor.h  —  "EchoJay Saturation": the plain face on the
    Harmonic cluster's core (BUILTIN_SUITE_PLAN.md §4).

    Drive into a selectable curve, tilt the tone, blend back. Everything
    interesting is in HarmonicCore; this file is the knobs, the contract and the
    registry entry.

    Note what is NOT here: no name matching, no add-menu entry, no advertisement
    text, no dispatch branch. All of that is generated from the registry entry at
    the bottom of the .cpp.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedHarmonicCore.h"

class EedSaturationProcessor : public EedDeviceProcessor
{
public:
    EedSaturationProcessor();

    const juce::String getName() const override { return "EchoJay Saturation"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    // Static so the registrar can publish the schema without constructing a
    // device: the advertisement has to exist before anything is instantiated.
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical param ids — used by the editor so a typo cannot silently
    // decouple a knob from the schema entry it is supposed to drive.
    static constexpr const char* kDriveDb  = "drive_db";
    static constexpr const char* kType     = "type";
    static constexpr const char* kToneDb   = "tone_db";
    static constexpr const char* kMix      = "mix";
    static constexpr const char* kOutputDb = "output_db";

    echojay::harmonic::HarmonicCore& core() noexcept { return core_; }

private:
    echojay::harmonic::HarmonicCore core_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedSaturationProcessor)
};
