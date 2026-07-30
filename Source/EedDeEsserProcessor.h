/*
    EedDeEsserProcessor.h  —  "EchoJay De-Esser".

    A frequency-conscious compressor: the DETECTOR listens through a bandpass at
    `freq` while the gain is applied to the audio, so it reduces only when there
    is energy where sibilance lives and ignores everything else. It is the same
    DynamicsCore as the compressor with a filtered sidechain — which is exactly
    the trick the EQ's dynamic Bell band already uses (EqEngine.cpp: a bandpass
    detector driving a per-sample gain), applied to a whole device instead of to
    one band.

    TWO MODES, because de-essing is two different jobs:

      * WIDE   — the gain is applied to the whole signal. The sibilant pulls the
                 entire vocal down for a few milliseconds. Transparent on a
                 solo'd vocal, and the classic broadcast de-esser behaviour.

      * SPLIT  — the signal is split at `freq` with a Linkwitz-Riley crossover and
                 only the high side is ducked. Nothing below `freq` moves at all,
                 so the body of the vocal is untouched. LR4 is what makes this
                 safe: the two halves sum flat, so with no reduction the device is
                 a true pass-through rather than a filter left in the path.

    LISTEN solo's the detector's band, which is the only reliable way to set
    `freq`: you tune until the sibilance is all you hear, then switch back. A
    de-esser dialled by watching a number is a de-esser tuned to the wrong "s".

    AUTO THRESHOLD (the depth pass, DEVICE_DEPTH_PLAN.md). A fixed threshold is
    the reason a de-esser has to be re-dialled per section: it is an ABSOLUTE
    level, and sibilance is a RELATIVE phenomenon — an "s" is loud compared to
    the words around it, not loud compared to 0 dBFS. So a threshold set on a
    verse over-de-esses the chorus, and one set on the chorus misses the verse.

    With `auto_threshold` on, the threshold rides a slow average of the detector
    band and sits a fixed amount above it, so the device reduces when the band
    gets loud RELATIVE TO ITSELF. That survives a level change, a fader move and
    a compressor placed before it.

    The dialled `threshold_db` is then IGNORED — not adjusted, not offset. Making
    it an offset was tempting and would have been worse: the knob would mean two
    different things depending on a switch, and neither the readout nor the
    advertisement could say which. It stays a schema param throughout, keeps its
    value, and comes back the moment auto is switched off; the editor hides it
    while it is doing nothing.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDynamicsCore.h"

class EedDeEsserProcessor : public EedDeviceProcessor
{
public:
    EedDeEsserProcessor();

    const juce::String getName() const override { return "EchoJay De-Esser"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kFreqHz      = "freq_hz";
    static constexpr const char* kRangeDb     = "range_db";
    static constexpr const char* kThresholdDb = "threshold_db";
    static constexpr const char* kAttackMs    = "attack_ms";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kMode        = "mode";     // on = split, off = wide
    static constexpr const char* kListen      = "listen";
    static constexpr const char* kAutoThresh  = "auto_threshold";

    float gainReductionDb() const noexcept { return core_.gainReductionDb(); }

    // The level of the SIDECHAIN BAND, not of the input: this device's detector
    // hears through a bandpass, so this is "how much sibilance is there right
    // now" rather than "how loud is the vocal". That distinction is the whole
    // device, and it is what the band view plots.
    float detectorLevelDb() const noexcept { return core_.detectorLevelDb(); }

    // Where the signal LIVES on that curve — the dwell histogram behind the
    // transfer curve's glow. Same never-block contract as the floats above,
    // published whole so the shape is never half of two different moments.
    const echojay::dyn::DwellTap& dwellHistogram() const noexcept
    {
        return core_.dwellHistogram();
    }

    // The curve this device runs. Fixed rather than published (a de-esser that
    // hands out a ratio is a compressor with a filter), but the editor has to
    // draw the curve the DSP actually runs, so they are named here once instead
    // of re-typed in the editor.
    static constexpr float kFixedRatio  = 8.0f;
    static constexpr float kFixedKneeDb = 4.0f;

    // The sidechain bandpass Q, for drawing the band the detector listens to.
    static constexpr double kSidechainQ = 2.0;

    bool isSplitMode() const noexcept { return splitMode_.load(); }
    bool isListening() const noexcept { return listen_.load(); }
    bool isAutoThreshold() const noexcept { return autoThreshold_.load(); }

    // The threshold the DSP is ACTUALLY comparing against — the dialled one, or
    // the tracked one while auto_threshold is on. The editor's curve and band
    // view are drawn from this, so the picture moves with the auto threshold
    // instead of showing a line the device is no longer using.
    //
    // One benign racy float, the same contract as the GR meter: it is written on
    // the audio thread and read by a 20 Hz timer, and a reading one block stale
    // is invisible.
    float effectiveThresholdDb() const noexcept { return core_.getThresholdDb(); }

    // How far above the band's running average the tracked threshold sits. A
    // sibilant is a few dB louder than the band around it; 6 catches the "s"
    // without chasing every consonant. Published because the editor names it.
    static constexpr float kAutoThresholdOffsetDb = 6.0f;

    // The averaging time behind that running level. Long enough to be a
    // PROGRAMME level and not a syllable — at 250 ms an individual "s" barely
    // moves the average it is being measured against, which is the property that
    // stops the tracker chasing its own reduction.
    static constexpr double kAutoThresholdWindowMs = 250.0;

private:
    // Rebuilds the coefficients for the current freqHz_. AUDIO THREAD ONLY —
    // see the note on appliedFreqHz_.
    void updateFilters();

    echojay::DynamicsCore core_;

    // The sidechain bandpass, one per channel: a biquad carries state, so the
    // two channels cannot share an instance even though they share coefficients.
    echojay::Biquad         scFilter_[2];
    echojay::LinkwitzRiley4 splitFilter_[2];

    // freq_hz is written from the message thread (a dial, or an AI move) and
    // read on the audio thread. Rebuilding the coefficients where they are
    // WRITTEN means the audio thread can read a biquad half-updated — and while
    // both the old and the new filter are stable, a mixture of the two need not
    // be, so the failure mode is a blow-up rather than a glitch.
    //
    // So the target is an atomic and the rebuild happens at the top of
    // processBlock, on the audio thread, only when it has actually moved. Same
    // discipline as EqEngine: parameters are atomics, coefficients are built
    // where they are used.
    std::atomic<double> freqHz_ { 6500.0 };
    double              appliedFreqHz_ = -1.0;   // audio thread only

    std::atomic<bool> splitMode_ { true };
    std::atomic<bool> listen_    { false };

    // AUTO THRESHOLD. The dialled threshold is kept HERE rather than read back
    // off the core, because while auto is on the core is holding the tracked
    // value instead — and a state round-trip that gave back whatever the tracker
    // happened to be at would rewrite the user's dial on every session save.
    std::atomic<bool>   autoThreshold_ { false };
    std::atomic<double> thresholdDb_   { -28.0 };

    // The running mean-square of the detector band. Audio thread only, and one
    // per device rather than per channel: the threshold is one number, so the
    // level it tracks is the linked one.
    float  autoMs_    = 0.0f;
    float  autoCoeff_ = 1.0f;

    double sampleRate_ = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedDeEsserProcessor)
};
