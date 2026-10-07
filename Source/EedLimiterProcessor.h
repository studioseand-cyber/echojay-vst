/*
    EedLimiterProcessor.h  —  "EchoJay Limiter", on the v2 engine (session L, 7-8 Oct 2026).

    THE ENGINE is Source/EJLimiterV2Core.h (limv2::Core): held-minimum lookahead with a smooth window, an 8x
    true-peak detector of the same class as the harness that judges it, a two-limb program-dependent release (an
    instant part over a slow floor at a fraction of the reduction), a post-check on the output, channel linking,
    and styles as Tunings measured against FabFilter Pro-L 2 Transparent (docs/limiter_ab/SESSION_L_NOTES.md).
    This file is the device around it: the dialable contract, the meters the loudness loop and the editor read,
    latency reporting, and the registration.

    THE CONTRACT IS UNCHANGED. Same parameter ids, ranges, defaults and state: ceiling_db, input_db, release_ms,
    lookahead_ms, mode, true_peak, sc_hpf_hz. What each one does on v2:
      ceiling_db    the ceiling (smoothed over 20 ms in the engine, the detector always at or below the clip)
      input_db      the loudness push, eased over 20 ms inside the engine
      true_peak     drives the oversampled detector and the post-check (off: sample-domain detection)
      sc_hpf_hz     a 2nd-order high-pass on the detector only, as before
      mode          ALL THREE VALUES RUN THE TRANSPARENT TUNING tonight (punchy and clip are recorded, not yet
                    tuned); the dial keeps its value so a later build can give them their own Tunings
      lookahead_ms  scales the engine's window around its tuned value: 2 ms (the default) IS the tuned window,
                    so an existing chain sounds like the measurement; 0 is the shortest window, 10 is 5x
      release_ms    scales the slow floor's recovery around its tuned value: 50 ms (the default) IS the tuned
                    180 ms; 1000 is 20x slower
    So a chain, preset or server-sent ceiling saved against the old limiter loads and sounds like the new one at
    its defaults, and the two dials still turn and still do something, in the engine's own terms.

    LATENCY IS FIXED. One number per sample rate, identical for every setting (true peak on or off, any
    lookahead): the engine delays the audio by its maximum and delays its own detector by the difference. A
    limiter whose latency moved when a dial moved would put the track out of time with the session each time.
    Reported through ejSetLatencyLogged as before. BYPASS STILL DELAYS, for the same reason it always did, and the
    gain crossfades over 10 ms either way so neither edge of a bypass clicks.

    gainReductionDb() is the DEEPEST gain of the last block (peak GR, negative), as the loudness loop has always
    read it. It is measured, not estimated: limiter_v2_core_test compares it with the block's actual output/input.

    THE DWELL HISTOGRAM AND DETECTOR LEVEL the transfer-curve editor draws still come from the shared DynamicsCore
    detector, which is fed the gained input and asked for nothing else; its gain is not used.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EchoJayLevelTally.h"
#include "EedDynamicsCore.h"
#include "EJLimiterV2Core.h"

class EedLimiterProcessor : public EedDeviceProcessor
{
public:
    EedLimiterProcessor();

    const juce::String getName() const override { return "EchoJay Limiter"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;
    // 18 Sep 2026 (loudness loop): the limiter's own meters for the final bubble.
    double inputDb() const noexcept { return inputDb_; }
    // 18e (item 4): input (after input_db, before the wall) and output meters - short-term LUFS + true peak for the UI
    echojay::LevelTally::Snapshot inputLevels()  const { return inMeter_.snapshot(); }
    echojay::LevelTally::Snapshot outputLevels() const { return outMeter_.snapshot(); }
    float  outputPeakDbMax() const noexcept { const float p = outPeakMax_.load (std::memory_order_relaxed); return p > 0.0f ? 20.0f * std::log10 (p) : -120.0f; }
    void   resetOutputPeak() noexcept { outPeakMax_.store (0.0f, std::memory_order_relaxed); }

    static constexpr const char* kCeilingDb   = "ceiling_db";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kLookaheadMs = "lookahead_ms";
    static constexpr const char* kMode        = "mode";
    static constexpr const char* kTruePeak    = "true_peak";
    static constexpr const char* kScHpfHz     = "sc_hpf_hz";
    static constexpr const char* kInputDb     = "input_db";   // 18 Sep 2026 (item 5): gain INTO the limiter, the loudness push

    // The schema's maximum for lookahead_ms, unchanged. On v2 it scales the engine's window around the tuned value
    // (see the header comment); the engine's storage is sized once, in prepareToPlay, for the largest window.
    static constexpr double kMaxLookaheadMs = 10.0;

    // The three limiter modes, in the schema's order. Named rather than bare
    // indices because the processor branches on them and "mode_ == 2" in a
    // processBlock is how a reordered schema becomes a silent behaviour change.
    enum class Mode { Transparent = 0, Punchy = 1, Clip = 2 };
    static constexpr int kNumModes = 3;

    Mode mode() const noexcept { return mode_; }

    // Whether the dialled release and lookahead are doing anything. On v2 every mode runs the Transparent tuning,
    // in which both dials scale the engine (see the header), so both are always in use. The editor asks rather than
    // testing the mode itself, so the interlock is stated once, next to the code that implements it.
    bool releaseInUse()   const noexcept { return true; }
    bool lookaheadInUse() const noexcept { return true; }

    float gainReductionDb() const noexcept { return wallGrDb_.load (std::memory_order_relaxed); }   // the engine's deepest gain of the last block (peak GR), negative
    float detectorLevelDb()  const noexcept { return core_.detectorLevelDb(); }

    // Where the signal LIVES on that curve — the dwell histogram behind the
    // transfer curve's glow. Same never-block contract as the floats above,
    // published whole so the shape is never half of two different moments.
    const echojay::dyn::DwellTap& dwellHistogram() const noexcept
    {
        return core_.dwellHistogram();
    }

private:
    // Re-derive the engine's Tuning from the dials (mode, lookahead_ms, release_ms) and push every other setting;
    // the four are one decision and are never updated apart.
    void applyLookahead();

    echojay::DynamicsCore    core_;     // the editor's dwell histogram and detector level only
    echojay::limv2::Core     engine_;   // the limiter

    Mode   mode_        = Mode::Transparent;
    double lookaheadMs_ = 2.0;
    double releaseMs_   = 50.0;
    double sampleRate_  = 44100.0;
    double inputDb_     = 0.0;    // dialled input gain, dB
    float  inputGain_   = 1.0f;   // its linear value, for the input meter's scaled copy
    echojay::LevelTally inMeter_ { echojay::LevelTally::Weighting::K }, outMeter_ { echojay::LevelTally::Weighting::K };   // 18e (item 4)
    std::atomic<float> outPeakMax_ { 0.0f };   // max |output sample| since resetOutputPeak ("Peaks" in the loop bubble)
    std::atomic<float> wallGrDb_ { 0.0f };     // the engine's deepest gain of the last block, dB (negative)
    bool   truePeakOn_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterProcessor)
};
