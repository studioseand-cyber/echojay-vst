/*
    EedTremoloProcessor.h  —  "EchoJay Tremolo": rhythmic level modulation.

    The first of the four Modulation-cluster faces. Its DSP is in
    EedTremoloEngine (the gain law and the wet/dry blend) sitting on the shared
    LfoCore; this file is the DIALABLE CONTRACT and nothing else.

    Note what is NOT here: no name matching, no add-menu entry, no advertisement
    text, no dispatch branch. All of that is generated from the registry entry at
    the bottom of the .cpp.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedTremoloEngine.h"

class EedTremoloProcessor : public EedDeviceProcessor
{
public:
    EedTremoloProcessor() = default;

    const juce::String getName() const override { return "EchoJay Tremolo"; }

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
    static constexpr const char* kRateHz      = "rate_hz";
    static constexpr const char* kSync        = "sync";
    static constexpr const char* kDivision    = "sync_division";
    static constexpr const char* kDepth       = "depth";
    static constexpr const char* kShape       = "shape";
    static constexpr const char* kStereoPhase = "stereo_phase";
    static constexpr const char* kMix         = "mix";
    static constexpr const char* kMode        = "mode";
    static constexpr const char* kSmoothing   = "smoothing_ms";

    echojay::TremoloEngine& engine() noexcept { return engine_; }

private:
    echojay::TremoloEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTremoloProcessor)
};
