/*
    EedLimiterProcessor.h  —  "EchoJay Limiter", on the v2 engine (session L, 7-8 Oct 2026).

    THE ENGINE is Source/EJLimiterV2Core.h (limv2::Core): held-minimum lookahead with a smooth window, an 8x
    true-peak detector of the same class as the harness that judges it, a two-limb program-dependent release (an
    instant part over a slow floor at a fraction of the reduction), a post-check on the output, channel linking,
    and styles as Tunings measured against FabFilter Pro-L 2 Transparent (docs/limiter_ab/SESSION_L_NOTES.md).
    This file is the device around it: the dialable contract, the meters the loudness loop and the editor read,
    latency reporting, and the registration.

    THE DEFAULTS ARE PRO-L 2's DEFAULT SETTING (8 Oct 2026, Sean's ruling): ceiling 0.0, TRUE PK on, LOOKAHEAD 0.18 ms,
    ATTACK 275 ms, RELEASE 400 ms, LINK 75 % transients / 100 % release. At these defaults the processor IS the tuned
    Transparent engine (limv2::transparent()), sample for sample (session G's zero-difference test). Same ids, ranges
    and state format for the old params; attack_ms, link_pct and release_link_pct are NEW ids - a saved chain without
    them loads at their defaults, which is the sound that chain had. The knobs are LITERAL, in Pro-L 2's terms:
      ceiling_db        the ceiling (smoothed over 20 ms; the detector always at or below the clip)
      input_db          the loudness push, eased over 20 ms
      true_peak         the 8x detector and the post-check (off: sample-domain detection)
      lookahead_ms      the window: label 0.18 (default) = the tuned 0.06 ms window that matches Pro-L 2 at ITS 0.18 label
                        (window = label / 3); more = smoother, more pre-dip, up to 5 (1.67 ms)
      attack_ms         the floor's charge: slowAttackMs = 150 ms x (A / 275), slowAttack2Ms = 1200 ms x (A / 275);
                        more = the floor charges slower = punchier, more per-hit dip
      release_ms        the floor's recovery: slowReleaseMs = 180 ms x (R / 400) (Pro-L 2's 400 label measured 160-185 ms);
                        more = steadier, quieter
      link_pct          transient link = L / 100 (a left-only hit dips the right channel L % as much)
      release_link_pct  release link = L / 100 (one floor for both channels at 100)
      sc_hpf_hz         a 2nd-order high-pass on the detector only, as before
      mode              ALL THREE VALUES RUN THE TRANSPARENT TUNING (punchy and clip are recorded, not yet tuned)
    A chain saved with the old limiter's values (lookahead 2.0, release 50) now gets a 2 ms window and a 22 ms floor
    recovery - closer to what that session sounded like with the old limiter than the Transparent tuning would be.

    LATENCY IS FIXED. One number per sample rate, identical for every setting (true peak on or off, any
    lookahead): the engine delays the audio by its maximum and delays its own detector by the difference. A
    limiter whose latency moved when a dial moved would put the track out of time with the session each time.
    Reported through ejSetLatencyLogged as before. BYPASS STILL DELAYS, for the same reason it always did, and the
    gain crossfades over 10 ms either way so neither edge of a bypass clicks.

    gainReductionDb() is the BLOCK gain reduction (output energy over gained-input energy, negative), which is what
    the loudness loop's own estimate measures; gainReductionPeakDb() is the deepest gain of the block, for the meter.
    Both are measured, not estimated: limiter_v2_core_test compares them with the block's actual output/input.

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
    // 8 Oct 2026 (Sean's ruling): a state saved by a pre-v2 build carries EVERY parameter, so an untouched session
    // holds lookahead_ms 2.0 and release_ms 50 - the OLD defaults, which with literal knobs would load as a 2 ms window
    // and a 22 ms floor. Migration at load: a state with NO attack_ms (pre-v2) AND lookahead_ms == 2.0 AND
    // release_ms == 50 loads lookahead 0.18 / release 400 (the new defaults), every new id at its default; any other
    // saved value loads literally, as the user set it; ceiling_db is never touched (session B always sets it).
    void setStateInformation (const void* data, int sizeInBytes) override;
    static constexpr double kOldDefaultLookaheadMs = 2.0, kOldDefaultReleaseMs = 50.0;
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
    // 9 Oct 2026: the panel's data path - the engine feeds this tap per sample (lock-free); the editor reads it on its timer
    echojay::limv2::MeterTap& meterTap() noexcept { return meterTap_; }
    float  outputPeakDbMax() const noexcept { const float p = outPeakMax_.load (std::memory_order_relaxed); return p > 0.0f ? 20.0f * std::log10 (p) : -120.0f; }
    void   resetOutputPeak() noexcept { outPeakMax_.store (0.0f, std::memory_order_relaxed); }

    static constexpr const char* kCeilingDb   = "ceiling_db";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kLookaheadMs = "lookahead_ms";
    static constexpr const char* kMode        = "mode";
    static constexpr const char* kTruePeak    = "true_peak";
    static constexpr const char* kScHpfHz     = "sc_hpf_hz";
    static constexpr const char* kInputDb     = "input_db";   // 18 Sep 2026 (item 5): gain INTO the limiter, the loudness push
    static constexpr const char* kAttackMs    = "attack_ms";          // 8 Oct 2026: how quickly sustained level charges the floor (Pro-L 2's ATTACK)
    static constexpr const char* kLinkPct     = "link_pct";           // 8 Oct 2026: transient channel linking, %
    static constexpr const char* kReleaseLinkPct = "release_link_pct";   // 8 Oct 2026: release (floor) channel linking, %

    // The schema's maximum for lookahead_ms: Pro-L 2's 5 ms. On v2 the label IS the window (see the header comment);
    // the engine's storage is sized once, in prepareToPlay, for this largest window.
    static constexpr double kMaxLookaheadMs = 5.0;

    // The three limiter modes, in the schema's order. Named rather than bare
    // indices because the processor branches on them and "mode_ == 2" in a
    // processBlock is how a reordered schema becomes a silent behaviour change.
    // 9 Oct 2026 (styles): 3 modern and 4 allround ADDED; 1 punchy now runs the tuned Punchy (Sean's ruling: the value
    // and its name keep their meaning); 2 clip is still the Transparent placeholder; 0 transparent is exactly as gated.
    enum class Mode { Transparent = 0, Punchy = 1, Clip = 2, Modern = 3, Allround = 4 };
    static constexpr int kNumModes = 5;

    Mode mode() const noexcept { return mode_; }

    // Whether the dialled release and lookahead are doing anything. On v2 every mode runs the Transparent tuning,
    // in which both dials scale the engine (see the header), so both are always in use. The editor asks rather than
    // testing the mode itself, so the interlock is stated once, next to the code that implements it.
    bool releaseInUse()   const noexcept { return true; }
    bool lookaheadInUse() const noexcept { return true; }

    // gainReductionDb(): BLOCK GR - 10log10 (output energy / gained-input energy) of the last block, negative. This is
    // what the loudness loop reads ("working N dB average"), and it agrees with the loop's own loudness-based
    // estimate (loudness_loop_guard J5) and with the harness's GR. gainReductionPeakDb(): the deepest gain applied
    // in the last block, for the GR meter's needle. Both measured, neither estimated (limiter_v2_core_test).
    float gainReductionDb() const noexcept     { return blockGrDb_.load (std::memory_order_relaxed); }
    float gainReductionPeakDb() const noexcept { return wallGrDb_.load (std::memory_order_relaxed); }
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
    echojay::limv2::MeterTap meterTap_; // the panel's picture and loudness source (EedLimiterPanelV2)

    Mode   mode_        = Mode::Transparent;
    double lookaheadMs_ = 0.18;
    double releaseMs_   = 400.0;
    double attackMs_    = 275.0;
    double linkPct_     = 75.0, releaseLinkPct_ = 100.0;
    double sampleRate_  = 44100.0;
    double inputDb_     = 0.0;    // dialled input gain, dB
    float  inputGain_   = 1.0f;   // its linear value, for the input meter's scaled copy
    echojay::LevelTally inMeter_ { echojay::LevelTally::Weighting::K }, outMeter_ { echojay::LevelTally::Weighting::K };   // 18e (item 4)
    std::atomic<float> outPeakMax_ { 0.0f };   // max |output sample| since resetOutputPeak ("Peaks" in the loop bubble)
    std::atomic<float> wallGrDb_ { 0.0f };     // the engine's deepest gain of the last block, dB (negative): peak GR
    std::atomic<float> blockGrDb_ { 0.0f };    // the engine's block GR (energy ratio), dB (negative)
    bool   truePeakOn_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterProcessor)
};
