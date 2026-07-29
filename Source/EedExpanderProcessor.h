/*
    EedExpanderProcessor.h  —  "EchoJay Expander".

    DynamicsCore in Expand mode: below the threshold it pushes DOWN by
    (ratio - 1) times the shortfall, so quiet material gets quieter while
    anything above the threshold passes untouched. It is the gentle relative of
    the gate — where a gate decides open or closed, an expander applies a
    continuous slope, which is what makes it usable on a vocal or a room mic
    where a gate would sound like switching.

    RANGE caps how far it is allowed to pull. Without it, a 4:1 expander on a
    signal 30 dB under the threshold asks for 90 dB of reduction, which is a
    fade to silence with no way to stop it short. With it, "expand downward but
    never by more than 12 dB" is expressible, which is the setting people
    actually want.

    Peak detection, fixed: an expander is judged on whether it opens cleanly on
    the front of a note, and an RMS window blurs exactly that.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDynamicsCore.h"

class EedExpanderProcessor : public EedDeviceProcessor
{
public:
    EedExpanderProcessor();

    const juce::String getName() const override { return "EchoJay Expander"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kThresholdDb = "threshold_db";
    static constexpr const char* kRatio       = "ratio";
    static constexpr const char* kAttackMs    = "attack_ms";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kRangeDb     = "range_db";

    float gainReductionDb() const noexcept { return core_.gainReductionDb(); }
    float detectorLevelDb()  const noexcept { return core_.detectorLevelDb(); }

    // The knee this device runs, fixed rather than published (see the note in
    // the constructor on why an expander does not hand out a hard-knee switch).
    // Public because the editor has to draw the curve the DSP actually runs, and
    // a 6.0 re-typed in the editor is a picture that silently stops matching the
    // sound the day this changes.
    static constexpr float kFixedKneeDb = 6.0f;

private:
    echojay::DynamicsCore core_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedExpanderProcessor)
};
