/*
    EedTemplateProcessor.h  —  SKELETON. Copy to Source/, rename Template -> your
    device. Not compiled where it sits (see DeviceTemplate/README.md).

    Everything a device needs is inherited from EedDeviceProcessor: the flat
    settings_structured.params apply path, merge semantics, schema clamping,
    state save/restore, bypass, buses and the AudioProcessor boilerplate.

    What you write: the schema, the id->knob mapping, and the DSP.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedTemplateEngine.h"

class EedTemplateProcessor : public EedDeviceProcessor
{
public:
    EedTemplateProcessor() = default;

    const juce::String getName() const override { return "EchoJay Template"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    // Static so the registrar can publish the schema without constructing the
    // device: the advertisement exists before anything is instantiated.
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical ids in one place, so the editor and the schema cannot drift
    // apart through a typo.
    static constexpr const char* kAmount = "amount";

    echojay::TemplateEngine& engine() noexcept { return engine_; }

private:
    echojay::TemplateEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTemplateProcessor)
};
