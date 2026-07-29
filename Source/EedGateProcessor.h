/*
    EedGateProcessor.h  —  "EchoJay Gate".

    The same DynamicsCore as the compressor, in Gate mode. What makes a gate a
    gate rather than a steep expander is the pair of parameters the expander does
    not have:

      * HYSTERESIS — the gate opens at the threshold but does not close until the
        signal falls BELOW threshold minus hysteresis. Without it, a signal
        sitting on the threshold opens and closes every few samples and the gate
        buzzes. This is the single most common way a gate is unusable.

      * HOLD — once open, it stays open for at least this long after the signal
        drops. That is what keeps a snare's tail or a room decay intact instead
        of chopping it off mid-decay.

    RANGE, not mute. The closed state attenuates by `range_db` rather than
    silencing, so the gate is usable as "duck the bleed by 20 dB" — which is what
    is wanted far more often than a hard on/off, and what stops the gated source
    sounding like it is being switched.

    Peak detection, fixed: a gate has to react to the transient that opens it,
    and an RMS window would let the front of every note through closed.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDynamicsCore.h"

class EedGateProcessor : public EedDeviceProcessor
{
public:
    EedGateProcessor();

    const juce::String getName() const override { return "EchoJay Gate"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kThresholdDb   = "threshold_db";
    static constexpr const char* kRangeDb       = "range_db";
    static constexpr const char* kAttackMs      = "attack_ms";
    static constexpr const char* kHoldMs        = "hold_ms";
    static constexpr const char* kReleaseMs     = "release_ms";
    static constexpr const char* kHysteresisDb  = "hysteresis_db";

    float gainReductionDb() const noexcept { return core_.gainReductionDb(); }

    // The detector's current level, for the dot riding the transfer curve. Same
    // benign racy single-float contract as the meter above.
    float detectorLevelDb() const noexcept { return core_.detectorLevelDb(); }

private:
    echojay::DynamicsCore core_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGateProcessor)
};
