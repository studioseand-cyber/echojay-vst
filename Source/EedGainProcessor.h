/*
    EedGainProcessor.h  —  "EchoJay Gain": level + pan.

    One of the two devices Wave 0 ships to prove the framework end to end
    (BUILTIN_SUITE_PLAN.md Wave 0 step 4). It is deliberately trivial DSP so that
    everything interesting about it is the PATTERN every later device copies:

        registry entry -> add-menu -> chain host -> editor on the shared look
                       -> dialled by settings_structured.params

    Note what is NOT here: no name matching, no add-menu entry, no advertisement
    text, no dispatch branch. All of that is generated from the registry entry at
    the bottom of the .cpp. This file is the whole device.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedGainEngine.h"

class EedGainProcessor : public EedDeviceProcessor
{
public:
    EedGainProcessor() = default;

    const juce::String getName() const override { return "EchoJay Gain"; }

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
    static constexpr const char* kLevelDb = "level_db";
    static constexpr const char* kPan     = "pan";

    echojay::GainEngine& engine() noexcept { return engine_; }

private:
    echojay::GainEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGainProcessor)
};
