/*
    EedStereoWidthProcessor.h  —  "EchoJay Stereo Width": width, bass mono, trim.

    The mixing face of the shared StereoEngine (BUILTIN_SUITE_PLAN.md §4, Wave 1
    Session A). Three knobs, all mono-safe: nothing this device does can change
    what a fold-down sounds like except the output trim, which changes its level
    and not its content.

    Its sibling "EchoJay Stereoizer" is the SAME engine with the creative knobs
    exposed instead. That is the cluster pattern: build the core once, hang
    several faces off it.

    Note what is NOT in this file: no name matching, no add-menu entry, no
    advertisement text, no dispatch branch. All of it is generated from the
    registry entry at the bottom of the .cpp.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedStereoEngine.h"

class EedStereoWidthProcessor : public EedDeviceProcessor
{
public:
    EedStereoWidthProcessor();

    const juce::String getName() const override { return "EchoJay Stereo Width"; }

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
    static constexpr const char* kWidth        = "width";
    static constexpr const char* kBassMonoHz   = "bass_mono_hz";
    static constexpr const char* kOutputTrimDb = "output_trim_db";

    echojay::StereoEngine& engine() noexcept { return engine_; }

private:
    echojay::StereoEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedStereoWidthProcessor)
};
