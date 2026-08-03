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

    THE DEPTH PASS (DEVICE_DEPTH_PLAN.md, Dynamics) gave this face the controls
    that turn a gate into a trigger:

      * `mode` — gate | duck. The same threshold, hysteresis, hold and range,
        applied to the OTHER side of the line: a gate attenuates what is below
        the threshold, a ducker attenuates what is above it. One switch, and the
        device becomes "pull this down whenever it gets loud".
      * `sc_hpf_hz` / `sc_lpf_hz` — FREQUENCY-SELECTIVE TRIGGERING, which is the
        single biggest thing a gate can gain. A tom mic hears the snare; band the
        detector to 80-250 Hz and the gate opens on the tom and ignores it. The
        filters are on the detector only, so a gate triggered from a narrow band
        still passes the full-range signal when it opens.
      * `lookahead_ms` — the attack can start BEFORE the transient that opened
        the gate, so the front of a note survives instead of being clipped off by
        however fast the attack is. Reported to the host as latency.
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
    static constexpr const char* kMode          = "mode";
    static constexpr const char* kScHpfHz       = "sc_hpf_hz";
    static constexpr const char* kScLpfHz       = "sc_lpf_hz";
    static constexpr const char* kLookaheadMs   = "lookahead_ms";

    static constexpr double kMaxLookaheadMs = 10.0;

    // Whether this device is currently gating or ducking. The editor's curve is
    // drawn in whichever mode the DSP is running, read from here rather than from
    // a second copy of the index.
    echojay::DynamicsMode dynamicsMode() const noexcept { return core_.getMode(); }
    bool isDucking() const noexcept
    {
        return core_.getMode() == echojay::DynamicsMode::Duck;
    }

    float gainReductionDb() const noexcept { return core_.gainReductionDb(); }

    // The detector's current level, for the instantaneous lift in the transfer
    // curve's glow. Same benign racy single-float contract as the meter above.
    float detectorLevelDb() const noexcept { return core_.detectorLevelDb(); }

    // Where the signal LIVES on that curve — the dwell histogram behind the
    // transfer curve's glow. Same never-block contract as the floats above,
    // published whole so the shape is never half of two different moments.
    const echojay::dyn::DwellTap& dwellHistogram() const noexcept
    {
        return core_.dwellHistogram();
    }

private:
    echojay::DynamicsCore core_;

    // The REQUESTED lookahead. The ring rounds to whole samples, so asking it
    // back would make a state round-trip drift away from what was dialled.
    double lookaheadMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGateProcessor)
};
