/*
    EedPsolaEngine.h  —  the SHIFTER behind "EchoJay Pitch"
    (PITCH_CORRECTION_SPEC.md §2.4, build phase P1).

    Deliberately JUCE-free, like PitchEngine, so the whole shifter unit-tests
    under plain g++ (test/psola_engine_test.cpp).

    P1 SCOPE. TD-PSOLA to a FIXED target, formants preserved. There is no scale,
    no decision stage and no retune envelope here - those are P2. The point of
    P1 is to prove the shift sounds like a voice on sustained notes before any
    musical logic exists to blame.

    HOW TD-PSOLA WORKS, and why it preserves formants for free:

      1. Find pitch EPOCHS in the input - one per period, consistently placed.
      2. Cut a Hann-windowed GRAIN of two periods centred on each epoch.
      3. Re-space those grains at the TARGET period and overlap-add.

    Step 3 changes how often the glottal pulses arrive without stretching the
    pulses themselves, so the spectral envelope - the formants, the thing that
    makes it a voice rather than a chipmunk - stays exactly where it was. That
    is the whole reason PSOLA earns its place over a resampler.

    TWO SYNTHESIS METHODS SINCE THE ANTARES A/B (PITCH_P0_VALIDATION.md §16).
    Granular OLA rebuilds the waveform ~f0 times a second however well the
    grains align - measured at -1.0 dB HNR and +19% spectral flux with NO
    pitch change asked for, against a reference that loses nothing. So
    `preserve` inside +/-2.5 st of unity - which is where a corrector lives -
    runs a SPLICE-RESAMPLER instead: a continuous resample of the dry ring,
    phase-aligned with the dry at every seam, splicing out exactly one period
    only when the read pointer has drifted one (|ratio-1| * f0 splices per
    second: ~1/s at 20 cents, zero at unity). The grain machinery above takes
    over beyond the band, through a short crossfade, and is what `off` and
    `shift` always use.

    EPOCHS. Peak-picking inside each expected period window, which the spec
    names as adequate. What actually matters for quality is not anatomical
    accuracy but CONSISTENCY: the same phase point every period, so grains
    align when they overlap. A search restricted to [0.7T, 1.3T] past the last
    epoch enforces that by construction.

    AMPLITUDE. Grains re-spaced at a different period no longer sum to unity,
    so the overlap-add is NORMALISED by the accumulated window - a parallel sum
    of the Hann windows, divided out at the end. Scaling grains by the period
    ratio instead is the common shortcut and it breathes audibly when the ratio
    moves; this does not.

    UNVOICED IS SACRED (spec §8). Unvoiced and silent output samples are the
    DELAYED DRY INPUT, sample for sample, with no grain content at all - not a
    crossfade, not an approximation. Correcting a consonant is exactly what
    makes cheap correctors sound cheap, and the null test in §8 is written to
    catch any drift from that. Voiced samples within a short fade of the
    boundary do cross-fade, so the seam is click-free; that fade lives entirely
    on the VOICED side and cannot disturb the unvoiced samples.

    LATENCY is real and reported honestly: PSOLA needs roughly one analysis
    window of lookahead, so the engine always runs `latencySamples()` behind
    and says so. That figure depends on the lowest pitch the active voice_type
    must represent, so it changes with voice_type - see prepare().

    THREADING. process() is audio-thread only: no allocation, no locks. Every
    buffer is sized in prepare() for the worst case any voice_type can ask for.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace echojay
{

class PsolaEngine
{
public:
    PsolaEngine() = default;

    // A target of 0 means PASSTHROUGH: output is the delayed dry input, bit for
    // bit. That is the default, so adding the device changes nothing but
    // latency until someone asks for a shift.
    static constexpr float kMinTargetHz = 40.0f;
    static constexpr float kMaxTargetHz = 1600.0f;

    // Ratio clamp. Beyond roughly two octaves either way TD-PSOLA stops
    // sounding like the source no matter how well it is implemented, and a
    // fixed-target P1 can be handed any combination of source and target.
    static constexpr float kMaxRatio = 4.0f;

    // Grains are two periods long, so a synthesis epoch's grain reaches one
    // period either side.
    static constexpr int   kGrainPeriods = 2;

    // ---- formant_mode (spec §2.4) ------------------------------------------
    // Order matches the spec's list, and is APPEND-ONLY: indices never move.
    enum FormantMode { kFormantOff = 0, kFormantPreserve = 1, kFormantShift = 2,
                       kNumFormantModes };

    void setFormantMode (int m) noexcept
    {
        formantMode_.store (std::clamp (m, 0, (int) kNumFormantModes - 1),
                            std::memory_order_relaxed);
    }
    int getFormantMode() const noexcept { return formantMode_.load (std::memory_order_relaxed); }

    // formant_shift, in semitones, active only in kFormantShift. Negative
    // reads bigger/deeper, positive smaller/brighter - the "throat length"
    // control.
    //
    // THE CHEAP VERSION WAS BUILT, MEASURED AND REJECTED, and must not come
    // back (PITCH_P0_VALIDATION.md §11.3): resampling each grain by a user
    // ratio, sharing the `off` code path, left the measured envelope INERT
    // from -9 to +3 semitones, quantised in ~600 Hz steps outside that, and
    // non-monotonic at the bottom - overlap-adding grains at the pitch period
    // reconstructs an envelope that barely follows per-grain resampling, so
    // that geometry cannot work however it is tuned.
    //
    // What ships instead is the spec's own prescription, per grain:
    //   1. estimate the spectral envelope by LPC (order ~ 2 + fs/1000),
    //   2. inverse-filter the grain to its RESIDUAL (flat spectrum, pulses),
    //   3. do the PSOLA move on the residual - copied 1:1, re-spaced at the
    //      target period, exactly like preserve - and
    //   4. re-synthesise through the envelope with its frequency axis warped
    //      by 2^(shift/12): the model envelope is evaluated on a dense grid,
    //      resampled at w/beta, cosine-transformed back to an autocorrelation
    //      (positive definite by construction) and Levinson'd into the warped
    //      all-pole synthesis filter.
    // See placeGrainResidual() for the measured details, including why the warp
    // works on the envelope rather than on the autocorrelation at scaled lags.
    static constexpr float kMaxFormantShiftSt = 12.0f;

    void setFormantShift (float semitones) noexcept
    {
        formantShift_.store (std::clamp (semitones, -kMaxFormantShiftSt, kMaxFormantShiftSt),
                             std::memory_order_relaxed);
    }
    float getFormantShift() const noexcept { return formantShift_.load (std::memory_order_relaxed); }

    // Fade applied on the VOICED side of a voiced/unvoiced seam. Short enough
    // to be inaudible as a level move, long enough to stop a step edge.
    static constexpr float kSeamFadeMs = 1.5f;

    // Divisor floor for the per-sample window-sum normalisation, BOTH modes.
    //
    // Chosen from the measured distribution of the accumulated window sum on
    // the real acapella (tools/pitch_click_test), not from reasoning — a
    // constant chosen by reasoning is exactly how the open-loop gain shipped
    // 5.5 clicks a second: 94% of emitted voiced samples sit in w = 0.95-1.05
    // and normal operation occupies w >= ~0.8, so any floor below that shelf
    // never touches steady state. Below the shelf is a thin edge tail (1.4%
    // of samples under 0.5, 0.98% under 0.35, minimum 0.0), which is where
    // instantaneous normalisation would reconstruct full amplitude from a
    // bare window tail — duplicated-pulse smear. The floor decides where
    // reconstruction gives way to a natural fade; the shipped value was
    // picked by sweeping 0.25 / 0.35 / 0.50 on the same material and reading
    // click density and pitch accuracy, see PITCH_P0_VALIDATION.md §14.
    static constexpr float kWindowFloor = 0.35f;

    // ---- lifecycle (audio stopped) ----------------------------------------
    // `lowestF0Hz` is the floor of the active voice_type: it sets the longest
    // period the engine must be able to buffer and window, and therefore the
    // latency. Everything is sized for the WORST case (the lowest floor any
    // voice_type can select) so a voice_type change never allocates.
    void prepare (double sampleRate, int maxBlockSize,
                  float lowestF0Hz, float worstCaseLowestF0Hz)
    {
        fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        bleedTau_ = 0.1 * fs_;
        ringSlowK_ = 1.0 / (0.14 * fs_);
        bleedGateK_ = 1.0 / (0.1 * fs_);
        carryLimit_ = (double) carryMs_ * 0.001 * fs_;

        const double worstPeriod = fs_ / std::max (10.0f, worstCaseLowestF0Hz);
        maxPeriod_ = (int) std::ceil (worstPeriod);

        setLowestF0 (lowestF0Hz);

        // Ring must hold: the lookahead, the longest grain either side of it,
        // one block, and slack for epoch search. Rounded to a power of two so
        // wrapping is a mask.
        // 8x maxPeriod, not 4: a downward formant_shift stretches a grain to
        // as much as 2x the analysis period either side of its epoch.
        const int need = maxLatency_ + 8 * maxPeriod_ + std::max (64, maxBlockSize) + 64;
        int bits = 1;
        while ((1 << bits) < need) ++bits;
        mask_ = (uint32_t) ((1 << bits) - 1);
        const size_t sz = (size_t) (1 << bits);

        in_.assign  (sz, 0.0f);
        f0_.assign  (sz, 0.0f);      // <= 0 means UNVOICED at that sample
        slowRing_.assign (sz, 0.0f);
        acc_.assign (sz, 0.0f);
        win_.assign (sz, 0.0f);

        // LPC scratch for kFormantShift, sized for the worst-case grain any
        // voice_type can ask for, so the audio thread never allocates. The
        // order follows the spec's ~ 2 + fs/1000 at the working rate.
        lpcOrder_ = std::clamp (2 + (int) (fs_ / 1000.0), 8, kMaxLpcOrder);
        const size_t grainMax = (size_t) (2 * maxPeriod_ + 1);
        lpcX_.assign  (grainMax + (size_t) kMaxLpcOrder, 0.0f);
        lpcY_.assign  (grainMax + (size_t) kMaxLpcOrder, 0.0f);
        lpcE_.assign  (grainMax, 0.0f);
        lpcWin_.assign (grainMax, 0.0f);
        lpcR_.assign  ((size_t) (kMaxLpcOrder + 1), 0.0);
        lpcRw_.assign ((size_t) (kMaxLpcOrder + 1), 0.0);
        lpcP_.assign  ((size_t) (kWarpGrid + 1), 0.0);
        lpcPw_.assign ((size_t) (kWarpGrid + 1), 0.0);
        lpcA_.assign  ((size_t) (kMaxLpcOrder + 1), 0.0);
        lpcAw_.assign ((size_t) (kMaxLpcOrder + 1), 0.0);
        coefPos_.assign  ((size_t) kCoefRing, 0);
        coefOrd_.assign  ((size_t) kCoefRing, 0);
        coefData_.assign ((size_t) kCoefRing * (size_t) (kMaxLpcOrder + 1), 0.0);
        synState_.assign ((size_t) kMaxLpcOrder, 0.0);

        seamFade_ = std::max (1, (int) std::lround (fs_ * (double) kSeamFadeMs * 0.001));

        reset();
    }

    void reset() noexcept
    {
        std::fill (in_.begin(),  in_.end(),  0.0f);
        std::fill (f0_.begin(),  f0_.end(),  0.0f);
        std::fill (acc_.begin(), acc_.end(), 0.0f);
        std::fill (win_.begin(), win_.end(), 0.0f);
        write_ = 0; emitted_ = 0; placedTo_ = 0;
        lastEpoch_ = 0; haveEpoch_ = false;
        nextSynth_ = 0; haveSynth_ = false; synthFrac_ = 0.0;
        coefHead_ = 0; coefTail_ = 0; coefCur_ = -1;
        std::fill (synState_.begin(), synState_.end(), 0.0);
        synIdx_ = 0;
        curTarget_ = 0.0f;
        spliceDrift_ = 0.0; spliceOldDrift_ = 0.0; spliceR_ = 0.0; spliceTf_ = 0.0;
        spliceFadeLen_ = 0; spliceFadePos_ = 0; spliceT_ = 0;
        methodMix_ = 0.0f;
        uvRun_ = 0;
        bridgeSeedT_ = 0.0; bridgeLen_ = 0.0;
    }

    // The active voice_type's floor. Changes the reported latency, which the
    // host is told about - a corrector that silently misaligns a vocal against
    // the track is worse than one that is a few ms slower.
    void setLowestF0 (float hz) noexcept
    {
        const double period = fs_ / (double) std::max (10.0f, hz);
        curPeriod_ = (int) std::ceil (period);
        recomputeLatency();
    }

    // Lookahead in PERIODS of the active floor. This is the whole of the
    // signal-path latency (PITCH_P0_VALIDATION.md §7): the detector's window is
    // a look-back and costs nothing, so this multiplier times the period of
    // fMin IS the delay.
    static constexpr float kLookaheadDefault = 3.0f;
    static constexpr float kLookaheadMin     = 1.0f;
    static constexpr float kLookaheadMax     = 4.0f;

    void setLookaheadPeriods (float periods) noexcept
    {
        lookahead_ = std::clamp (periods, kLookaheadMin, kLookaheadMax);
        recomputeLatency();
    }
    float getLookaheadPeriods() const noexcept { return lookahead_; }

    int latencySamples() const noexcept { return latency_; }

    // What the latency WOULD be for a given floor and multiplier, without
    // touching the engine - so an editor can print the number a control is
    // about to cause before the user commits to it.
    static int latencyFor (double sampleRate, float lowestF0Hz, float periods) noexcept
    {
        const double period = sampleRate / (double) std::max (10.0f, lowestF0Hz);
        return (int) (std::clamp (periods, kLookaheadMin, kLookaheadMax)
                      * (float) std::ceil (period));
    }

    // ---- parameters (message thread) --------------------------------------
    void  setTargetHz (float hz) noexcept
    {
        targetHz_.store (hz <= 0.0f ? 0.0f
                                    : std::clamp (hz, kMinTargetHz, kMaxTargetHz),
                         std::memory_order_relaxed);
    }
    float getTargetHz() const noexcept { return targetHz_.load (std::memory_order_relaxed); }

    // TRACKING-LAG COMPENSATION (PITCH_P0_VALIDATION.md §7.1).
    //
    // A YIN frame spanning [p - frameLen, p] produces an estimate that best
    // describes the MIDDLE of that span, not its end - so an f0 published at
    // p actually characterises audio around p - frameLen/2. Attributing it to
    // p makes every estimate half a window stale on a moving pitch: 15 ms at
    // alto_tenor, 70 ms at bass. That is a correctness problem, not a latency
    // one, and it is fixed by back-dating the attribution rather than by
    // waiting.
    //
    // THE CONSTRAINT THIS CREATES, which matters for low_latency: the f0 is
    // written `lag` samples behind the input head, and it must land AHEAD of
    // the shifter's read point or the sample it describes has already been
    // emitted. So lag <= latency, and with frameLen = 3.5 periods the lag is
    // 1.75 periods - which puts a hard floor of ~1.75 periods on the lookahead
    // that has nothing to do with PSOLA's own needs.
    // Wet/dry blend and final trim. Both live here because the DRY signal at
    // the right delay only exists inside this engine.
    //
    // At mix 100 and 0 dB both are exact no-ops - no multiply happens at all -
    // so the bit-identical unvoiced guarantee of spec §8 survives them.
    void  setMixPercent (float pct) noexcept { mix_.store (std::clamp (pct, 0.0f, 100.0f) * 0.01f); }
    float getMixPercent() const noexcept     { return mix_.load() * 100.0f; }
    void  setOutputDb (float db) noexcept
    {
        outGain_.store (std::fabs (db) < 1.0e-6f ? 1.0f : std::pow (10.0f, db / 20.0f));
        outDb_.store (db);
    }
    float getOutputDb() const noexcept       { return outDb_.load(); }

    // Convenience overload for callers that drive the target through the
    // atomic (the render tool, the engine tests).
    void process (const float* in, float* out, int n, float f0Hz, bool voiced) noexcept
    {
        process (in, out, n, f0Hz, voiced, targetHz_.load (std::memory_order_relaxed));
    }

    // ---- READ-ONLY DIAGNOSTIC HOOK ---------------------------------------
    // Records WHERE the synthesis phase is discontinuous: an epoch re-seed
    // (the analysis epoch jumps to a new peak) and a synthesis-cursor reset.
    // Both change which part of the waveform the next grain is drawn from, so
    // both are candidates for a step in the output. Off unless a caller asks;
    // it changes no decision, only observes them.
    void debugRecordPhaseEvents (bool on) noexcept { debugOn_ = on; }

    // Diagnostic A/B only: route raw grains (identity filter entries) through
    // the residual plumbing, isolating the emit filter's contribution.
    void debugForceRawGrains (bool on) noexcept { dbgRawGrains_ = on; }

    // Diagnostic only: disable the in-band splice-resampler so preserve/off
    // fall back to their grain paths. Exists for the equivalence gate's
    // POSITIVE CONTROL - forcing the two modes onto different paths must
    // make the equivalence check fail, or the check proves nothing.
    void debugDisableSplice (bool on) noexcept { dbgNoSplice_ = on; }

    // Sub-decision synthesis events, for correlating audible burrs against
    // things the hop log cannot see: splice-resampler period jumps, and
    // splice<->grain method transitions. Input-time positions.
    const std::vector<uint64_t>& debugSplices()     const noexcept { return dbgSplice_; }
    const std::vector<uint64_t>& debugMethodFlips() const noexcept { return dbgMethodFlip_; }
    const std::vector<uint64_t>& debugReseeds()      const noexcept { return dbgReseed_; }
    const std::vector<uint64_t>& debugCursorResets() const noexcept { return dbgReset_; }

    // Per-grain geometry, and the accumulated-window sum seen at emit. The
    // first says WHICH grain changed shape; the second is the distribution the
    // normalisation floor has to be chosen against.
    struct DebugGrain { uint64_t pos; int Ta, Ts, half; float gain; };
    const std::vector<DebugGrain>& debugGrains() const noexcept { return dbgGrain_; }
    const std::vector<uint32_t>&   debugWinHist() const noexcept { return dbgWinHist_; }
    static constexpr int kDebugWinBuckets = 80;      // 0.00..4.00 in 0.05 steps
    float debugWinMin() const noexcept { return dbgWinMin_; }

    // Per emitted sample, in emit order (one entry per output sample): the
    // seam gain that mixed wet against dry, and the window sum under it.
    // seamG < 0 marks a sample emitted by the PASSTHROUGH path (no grains).
    // This is what lets a click position be asked "were you a wet/dry seam,
    // a window hole, or neither?" against the shifter's own record instead
    // of a parallel reconstruction's guess.
    struct DebugEmit { float seamG, winSum; };
    const std::vector<DebugEmit>& debugEmits() const noexcept { return dbgEmit_; }

    // Per-sample splice-band author record (4 Sep 2026, the constant-shift
    // mid-voiced breaks): which branch wrote `wet`, with the state that
    // decided it. One push per emitted sample in BOTH emit paths, so the
    // index is the emitted position exactly like dbgEmit_. mix<0 marks a
    // sample emitted by emitDry (no splice state in play).
    struct DebugSpl { float g, mix, r; float drift; int T; };
    const std::vector<DebugSpl>& debugSpliceTrace() const noexcept { return dbgSpl_; }

    void setPitchLagSamples (int lag) noexcept { pitchLag_ = std::max (0, lag); }
    int  getPitchLagSamples() const noexcept   { return pitchLag_; }

    // Drift bleed (see spliceSample): off by default; the A/B lives in
    // tools/pitch_constshift_probe until a ruling ships it.
    void setDriftBleed (bool on) noexcept { driftBleed_ = on; }
    bool getDriftBleed() const noexcept   { return driftBleed_; }

    // Audio-verified bridging (see process()): while a tracked-unvoiced
    // slice's ring audio stays measurably periodic, the MEASURED f0 is
    // written instead of 0, up to maxMs per run. maxMs 0 disables (the
    // default). thresh is the periodicity bar (the census classifier's
    // kStillPeriodic-family test).
    // Diagnostic only (30 Aug 2026 tau sweep): force the pre-gate bleed
    // behaviour so gated-vs-ungated can be measured at every retune tau.
    void debugBleedUngated (bool on) noexcept { dbgBleedUngated_ = on; }

    void setF0Bridge (float maxMs, float thresh) noexcept
    { bridgeMaxMs_ = std::max (0.0f, maxMs); bridgeThresh_ = thresh; }
    float getF0BridgeMaxMs() const noexcept { return bridgeMaxMs_; }

    struct DbgBridge { uint64_t pos; int len; float f0, r; };
    const std::vector<DbgBridge>& debugBridges() const noexcept { return dbgBridge_; }

    // Drift carry (see emitMixed): gaps shorter than this keep the read
    // trajectory across the gap instead of re-anchoring. 0 = old behaviour.
    void setDriftCarryMs (float ms) noexcept
    {
        carryMs_ = std::max (0.0f, ms);
        carryLimit_ = (double) carryMs_ * 0.001 * fs_;
    }
    float getDriftCarryMs() const noexcept { return carryMs_; }

    // Normalised autocorrelation of the INPUT ring at one lag, over a window
    // of two periods ending at `inputPos`. This is the F0JumpGate's audio
    // question (PITCH_P0_VALIDATION.md §16.8): when the estimate jumps an
    // octave, is the waveform still periodic at the OLD lag (a spurious flip
    // - hold) or has that correlation collapsed (the signal really moved -
    // believe it)? O(2T) per call and only asked on octave-scale jumps.
    float inputPeriodicity (uint64_t inputPos, int lagSamples) const noexcept
    {
        if (mask_ == 0 || lagSamples < 8) return 0.0f;
        const int W = 2 * lagSamples;
        const int64_t from   = (int64_t) inputPos - W;
        const int64_t oldest = (int64_t) write_ - (int64_t) (mask_ + 1);
        if (from - lagSamples <= oldest || from - lagSamples < 0
            || inputPos > write_) return 0.0f;

        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (int i = 0; i < W; ++i)
        {
            const double a = in_[(size_t) ((uint32_t) (uint64_t) (from + i) & mask_)];
            const double b = in_[(size_t) ((uint32_t) (uint64_t) (from + i - lagSamples) & mask_)];
            ab += a * b; aa += a * a; bb += b * b;
        }
        if (aa < 1.0e-12 || bb < 1.0e-12) return 0.0f;
        return (float) (ab / std::sqrt (aa * bb));
    }

    // ---- audio thread ------------------------------------------------------
    // Push n input samples with the detector's CURRENT reading, and pull the n
    // output samples that are `latencySamples()` behind them. f0 <= 0 or
    // voiced == false marks the span unvoiced.
    // `targetHz` is passed PER CALL rather than read from an atomic, so a
    // caller can slice a block at hop boundaries and give each slice the target
    // that hop actually decided. Passing 0 means passthrough. The atomic setter
    // remains for the fixed-target diagnostic path.
    // Shift-mode sentinel: kNoShift = "no shift given, use target/f0" (the
    // fixed-target diagnostic and bypass paths). A real shift replaces the
    // RATIO at both synthesis sites with 2^(shift/1200): the fast component
    // cancels ALGEBRAICALLY at the engine's own time-aligned f0 index —
    // never a delay-line's belief about latency (3 Sep 2026 ruling).
    static constexpr float kNoShift = -100000.0f;

    void process (const float* in, float* out, int n, float f0Hz, bool voiced,
                  float targetHz) noexcept
    { process (in, out, n, f0Hz, voiced, targetHz, kNoShift); }

    void process (const float* in, float* out, int n, float f0Hz, bool voiced,
                  float targetHz, float shiftCents) noexcept
    {
        curShift_ = shiftCents;
        // UNPREPARED GUARD. mask_ is 0 and the ring is empty until prepare()
        // runs, and in_[0] on an empty vector is undefined behaviour. JUCE
        // orders prepareToPlay before processBlock so this is latent rather
        // than live - but "latent" is a property of today's callers, not of
        // this code, and the cost of being sure is one branch per block.
        if (mask_ == 0 || in_.empty())
        {
            if (out != in) std::fill (out, out + n, 0.0f);
            return;
        }

        float track = (voiced && f0Hz > 0.0f) ? f0Hz : 0.0f;

        for (int i = 0; i < n; ++i)
        {
            in_[(size_t) ((uint32_t) write_ & mask_)] = in[i];
            ++write_;
        }

        // Attribute the estimate to the audio it actually describes. Clamped
        // so it can never reach behind what has already been emitted - if the
        // lag exceeds the lookahead the compensation is simply reduced rather
        // than corrupting the past.
        const int lag = std::min (pitchLag_, std::max (0, latency_ - 1));

        // AUDIO-VERIFIED BRIDGING (29 Aug 2026 ruling). A tracker blink
        // inside a continuous note writes f0=0 into the ring, and every
        // such zero-run is a seam the shifter executes - measured at ~7
        // breaks/s of voiced, half of all span boundaries being blinks
        // (tools/pitch_span_census: audio periodic straight through the
        // gap, flanks within cents). Bridge CAUSALLY, per hop, on the
        // audio's own testimony: the ring write runs pitchLag_ behind the
        // hop, so the audio around the position being written - including
        // ~lag samples ahead of it - is already in the ring. While a
        // zero slice's ring audio stays periodic at the seeded period
        // (best-lag search, threshold bridgeThresh_), write the MEASURED
        // f0 (fs/bestLag - from the audio, never interpolated from the
        // flanks) instead of 0; the seed follows the audio hop by hop. The
        // moment periodicity dies - a real consonant - the test fails,
        // that hop writes 0, the seam lands exactly at loss of
        // periodicity, and everything from there stays bit-exact dry. A
        // failed test DISARMS until true voicing returns (no flutter);
        // bridgeMaxMs_ caps a run so sustained periodic-but-untracked
        // material cannot out-vote the tracker forever. The CORRECTOR
        // never sees bridged values - this rewrites only the shifter's
        // ring, so tuning decisions are untouched.
        if (bridgeMaxMs_ > 0.0f)
        {
            if (track > 0.0f)
            { bridgeSeedT_ = fs_ / (double) track; bridgeLen_ = 0.0; }
            else if (bridgeSeedT_ > 8.0
                     && bridgeLen_ + n <= (double) bridgeMaxMs_ * 0.001 * fs_)
            {
                const int64_t q = (int64_t) write_ - (int64_t) n / 2 - (int64_t) lag;
                const int T0 = (int) std::lround (bridgeSeedT_);
                int bestLag = 0; float bestR = -1.0f;
                const int step = std::max (1, T0 / 16);
                for (int L = (int) (0.85 * T0); L <= (int) (1.15 * T0); L += step)
                {
                    const float r = q > L ? inputPeriodicity ((uint64_t) (q + L), L) : 0.0f;
                    if (r > bestR) { bestR = r; bestLag = L; }
                }
                for (int L = std::max (8, bestLag - step + 1); L < bestLag + step; ++L)
                {
                    if (L == bestLag) continue;
                    const float r = q > L ? inputPeriodicity ((uint64_t) (q + L), L) : 0.0f;
                    if (r > bestR) { bestR = r; bestLag = L; }
                }
                if (bestR >= bridgeThresh_ && bestLag >= 8)
                {
                    track = (float) (fs_ / (double) bestLag);
                    bridgeSeedT_ = (double) bestLag;
                    bridgeLen_ += (double) n;
                    if (debugOn_)
                        dbgBridge_.push_back ({ (uint64_t) std::max<int64_t> (0,
                            (int64_t) write_ - (int64_t) n - (int64_t) lag),
                            n, track, bestR });
                }
                else bridgeSeedT_ = 0.0;   // real consonant: disarm until voiced
            }
            else bridgeSeedT_ = 0.0;       // cap reached or no seed
        }
        for (int i = 0; i < n; ++i)
        {
            const int64_t at = (int64_t) write_ - (int64_t) n + i - (int64_t) lag;
            if (at < 0 || at < emitted_) continue;
            f0_[(size_t) ((uint32_t) (uint64_t) at & mask_)] = track;
            // The fast-ring slow reference rides the SAME lag compensation
            // as the f0 it will divide - time-aligned by the proven
            // mechanism, not a new timing belief (2 Sep, fourth cut: the
            // feed-time reference led the audio by ~latency, costing ~5c).
            if (fastRingOn_ && ! slowRing_.empty())
                slowRing_[(size_t) ((uint32_t) (uint64_t) at & mask_)] = fastSlowHz_;
        }

        const float target = targetHz;

        // THE ONE COORDINATE SYSTEM: everything below is in INPUT time. Output
        // sample i of this call carries input position `base + i`, which is
        // `latency_` behind the write head. Deriving it from write_ each call
        // rather than accumulating a second counter is what keeps the delay
        // exact - two counters advancing at the same rate never lag each other,
        // which is a silent-output bug waiting to happen.
        const int64_t base = (int64_t) write_ - (int64_t) n - (int64_t) latency_;

        // Passthrough: the delayed dry input, exactly. No grains, no window
        // normalisation, nothing that could round a sample.
        if (target <= 0.0f)
        {
            emitDry (out, n, base);
            return;
        }

        // Place every grain that can touch the range about to be emitted, then
        // read it out.
        advanceSynthesis (base + (int64_t) n + (int64_t) curPeriod_ * kGrainPeriods, base,
                          target);
        curTarget_ = target;
        // curShift_ already stored at entry; emitMixed's splice arm reads it.
        emitMixed (out, n, base);
    }

private:
    // ---- output ------------------------------------------------------------
    void emitDry (float* out, int n, int64_t base) noexcept
    {
        const bool lpc = formantMode_.load (std::memory_order_relaxed) != kFormantOff;
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = base + (int64_t) i;
            if (p < 0) { out[i] = 0.0f; if (debugOn_) { dbgEmit_.push_back ({ -1.0f, 0.0f }); dbgSpl_.push_back ({ -1.0f, -1.0f, 0.0f, 0.0f, 0 }); } continue; }
            const uint32_t idx = (uint32_t) (uint64_t) p & mask_;
            const float og = outGain_.load (std::memory_order_relaxed);
            out[i] = og == 1.0f ? in_[(size_t) idx] : in_[(size_t) idx] * og;
            // Keep the synthesis filter's state fed with the dry signal, so
            // a later switch into correction rings from reality. This writes
            // internal state only - passthrough output stays the exact dry.
            if (lpc) pushSynthState (in_[(size_t) idx]);
            // Clear the accumulators as we pass, so a later switch back to
            // shifting does not read stale grain content.
            acc_[(size_t) idx] = 0.0f;
            win_[(size_t) idx] = 0.0f;
            if (debugOn_) { dbgEmit_.push_back ({ -1.0f, 0.0f }); dbgSpl_.push_back ({ -1.0f, -1.0f, 0.0f, 0.0f, 0 }); }
        }
        emitted_ = base + (int64_t) n;
    }

    void emitMixed (float* out, int n, int64_t base) noexcept
    {
        const int  fm  = formantMode_.load (std::memory_order_relaxed);
        const bool lpc = fm != kFormantOff;
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = base + (int64_t) i;
            if (p < 0) { out[i] = 0.0f; if (debugOn_) { dbgEmit_.push_back ({ -1.0f, 0.0f }); dbgSpl_.push_back ({ -1.0f, -1.0f, 0.0f, 0.0f, 0 }); } continue; }
            const uint32_t idx = (uint32_t) (uint64_t) p & mask_;

            const float dry = in_[(size_t) idx];

            // NEVER fall back to dry inside a voiced span. Where the window
            // sum is thin - which happens at large downshifts, when grains are
            // re-spaced further apart than they are long - substituting the dry
            // signal re-injects the SOURCE pitch, and the output then measures
            // as unshifted. Flooring the divisor instead lets the thin patch
            // come out quiet, which is what a truncated vocal-tract ring
            // actually sounds like, rather than wrong.
            //
            // BOTH modes normalise by the accumulated window, per sample.
            // PRESERVE used an open-loop sqrt(Ts/Ta) grain gain instead, on
            // the argument that instantaneous division would smear duplicated
            // pulses - and that argument shipped 5.5 clicks a second on real
            // material (tools/pitch_click_test, 329 in 60 s): every ordinary
            // f0 update changed the next grain's length AND its gain, so the
            // overlap-add summed to a different amplitude a fixed ~20 samples
            // after the hop, with nothing reconciling the seam. Measured, the
            // window sum sits at 0.95-1.05 for 94% of emitted voiced samples,
            // so this division is a near no-op in steady state and exactly
            // cancels the length-dependent amplitude at the seams. The smear
            // the old comment feared lives only below the floor, where the
            // clamp lets thin coverage fade instead of reconstructing it.
            const float w   = win_[(size_t) idx];
            if (debugOn_ && w > 0.0f)
            {
                if (dbgWinHist_.empty()) dbgWinHist_.assign (kDebugWinBuckets, 0u);
                const int b = std::clamp ((int) (w / 0.05f), 0, kDebugWinBuckets - 1);
                ++dbgWinHist_[(size_t) b];
                dbgWinMin_ = std::min (dbgWinMin_, w);
            }
            const float resid = acc_[(size_t) idx] / std::max (w, kWindowFloor);

            // UNVOICED IS SACRED: the dry sample, untouched. Voiced samples
            // near the seam fade between wet and dry so the join is smooth,
            // and that fade lives entirely on the voiced side.
            const float g = seamGain ((uint64_t) p);
            if (debugOn_) dbgEmit_.push_back ({ g, w });

            // preserve/shift: the OLA'd content is the RESIDUAL; the envelope
            // is re-applied here by the continuous synthesis filter. On dry
            // samples the filter is not run but its state is fed the dry
            // signal, so the next voiced sample rings from where the audio
            // actually was. `off` keeps the direct OLA.
            float wet;
            if (lpc)
            {
                if (g > 0.0f) wet = synthStep ((uint64_t) p, resid);
                else          { pushSynthState (dry); wet = dry; }
            }
            else wet = resid;

            // PRESERVE within the splice band rides the SPLICE-RESAMPLER
            // instead of the grains (PITCH_P0_VALIDATION.md §16): at
            // corrector-scale ratios the output is a continuous resample of
            // the dry signal, phase-aligned with it at every seam, with ONE
            // period-aligned splice each time the read pointer drifts a
            // period - roughly |ratio-1| * f0 splices per second, i.e. ~1/s
            // at 20 cents and ZERO at unity, against ~f0 grain boundaries
            // per second for OLA. Measured on the reference take, the grain
            // path cost -1.0 dB HNR and +19% flux at unity where this path
            // is, by construction, the identity. Formants move with the
            // ratio here - bounded by the band at a level correction never
            // reaches audibly - and the grain path takes over beyond it,
            // through a short crossfade.
            // PRESERVE **AND OFF** ride the splice inside the band. Off was
            // measured through the six-metric gate on its grain path at
            // corrector ratios: HNR 4.87 dB against preserve's 6.99, flux
            // +19% against +3.3, 59 clicks against 2 - a defect, not the
            // mode working, because at these ratios the splice-resampler IS
            // off's semantics (a resampler moves formants with the ratio;
            // the displacement at tens of cents is negligible, which is the
            // same fact that makes it acceptable for preserve). The two
            // modes deliberately converge in-band and diverge beyond it,
            // where off's resampled grains go full chipmunk. SHIFT never
            // splices - its envelope warp needs the LPC grain path.
            if (g > 0.0f && fm != kFormantShift && ! dbgNoSplice_)
            {
                // DRIFT CARRY (5 Sep 2026 ruling): a re-entry after a SHORT
                // gap keeps the accumulated drift instead of re-anchoring.
                // The census (tools/pitch_span_census) measured half of all
                // span boundaries as tracking blinks inside continuous
                // notes - audio periodic straight through, flanks within
                // cents - and every re-anchor there was a content jump the
                // field hears (~7 breaks/s of voiced, one per boundary).
                // Carrying is the conservative action on "cannot measure":
                // the gap itself stays bit-exact dry either way; only the
                // re-anchor is removed, the wet resuming its pre-gap read
                // trajectory (the entry fade's join handles the offset -
                // its r0 term never assumed zero drift). Decided CAUSALLY
                // at re-entry from the gap's length: real pauses (> the
                // threshold) still re-anchor. carryLimit_ = 0 is exactly
                // the old behaviour.
                if (uvRun_ > 0)
                {
                    if ((double) uvRun_ > carryLimit_) spliceDrift_ = 0.0;
                    uvRun_ = 0;
                }
                // The ratio is what the READ point's audio must be scaled by,
                // so evaluate f0 where the read pointer actually is - up to
                // ~3/4 of a period away from p, which on a vibrato is a
                // few cents of systematic error if ignored.
                const int64_t rp = (int64_t) p + (int64_t) std::lround (spliceDrift_);
                const float f0Here = f0At ((uint64_t) std::max<int64_t> (0, rp));
                const float tgt    = curTarget_;
                const bool  ok     = f0Here > 0.0f && tgt > 0.0f;

                // `ok` gates STATE UPDATES only, never emission: the read
                // position is displaced from p, so it can land on an
                // isolated ring-unvoiced sample mid-note, and both earlier
                // treatments of that flicker were measured as clicks -
                // resetting the drift was a fadeless ~200-sample read jump,
                // and falling through to the grain value was two unfaded
                // samples of a different synthesis (§16.10). Through a
                // flicker the splice keeps emitting on frozen state.
                if (ok)
                {
                    // RING-ALIGNED FAST TERM (2 Sep 2026, flag): the slow
                    // shift arrives via curShift_; the fast component is
                    // computed HERE, per sample, as (f0Here/ringSlow)^(k-1)
                    // - the audio's own wobble read at the audio's own
                    // time, the k=100 cancellation generalised. ringSlow is
                    // this engine's one-pole (~140ms) of f0Here, seeded at
                    // re-entry (never a single stale sample - §17.6).
                    double fastFactor = 1.0;
                    const float slowHere = fastRingOn_
                        ? slowRing_[(size_t) ((uint32_t) (uint64_t) std::max<int64_t> (0, rp) & mask_)]
                        : 0.0f;
                    if (fastRingOn_ && slowHere > 0.0f)
                    {
                        // Numerator ring-aligned (phase-critical); the
                        // denominator is the CORRECTOR's slow track, per
                        // hop - one slow reference for both terms (the
                        // engine-side slow track was the third cut's
                        // measured mistake).
                        const double dev = (double) f0Here / (double) slowHere;
                        fastFactor = std::pow (std::clamp (dev, 0.84, 1.19),
                                               (double) fastK_ - 1.0);
                    }
                    const double r = (curShift_ > kNoShift + 1.0f
                        ? std::exp2 ((double) curShift_ / 1200.0)
                        : (double) tgt / (double) f0Here) * fastFactor;
                    const double absSt = std::fabs (std::log2 (r) * 12.0);
                    const float want = absSt <= kSpliceBandSt ? 0.0f : 1.0f;
                    const float step = 1.0f / (float) std::max (16, (int) (0.004 * fs_));
                    methodMix_ += methodMix_ < want ?  std::min (step, want - methodMix_)
                                                    : -std::min (step, methodMix_ - want);

                    // Always slewed at ~2 ms - the earlier snap branch for
                    // moves > 0.125 st was measured creating clicks on fast
                    // downward glides at retune 0 (§16.10): the target
                    // staircases semitone by semitone through the glide and
                    // every 100-cent step snapped the read velocity
                    // instantly. Through the slew a note-sized step still
                    // completes in ~6 ms, which keeps the hard-tune snap
                    // character while the velocity stays continuous.
                    if (spliceR_ <= 0.0) spliceR_ = r;
                    else spliceR_ += (r - spliceR_) * (1.0 / (0.002 * fs_));

                    spliceT_  = std::clamp ((int) std::lround (fs_ / (double) f0Here),
                                            8, maxPeriod_);
                    spliceTf_ = std::clamp (fs_ / (double) f0Here, 8.0, (double) maxPeriod_);
                }

                if (methodMix_ < 1.0f && spliceR_ > 0.0 && spliceT_ > 0)
                {
                    const float ys = spliceSample ((uint64_t) p, spliceT_, spliceTf_, spliceR_);
                    wet = ys + methodMix_ * (wet - ys);
                }
            }
            else if (g <= 0.0f)
            {
                // A seam or unvoiced sample. With drift carry off the next
                // voiced entry starts phase-aligned with the dry by
                // construction; with it on, the drift survives until the
                // re-entry decision above judges the gap's length.
                ++uvRun_;
                if (carryLimit_ <= 0.0) spliceDrift_ = 0.0;
                spliceFadeLen_ = 0; methodMix_ = 0.0f;
                spliceR_ = 0.0; spliceT_ = 0; spliceTf_ = 0.0;
                // ringSlowHz_ deliberately SURVIVES blinks: it is an
                // input-derived slow track (the corrector's 200ms-rule
                // lesson); resetting it at every 11ms dropout left the
                // fast factor at ~1 for 140ms after each of ~8 blinks/s -
                // measured as 25.5c under-correction on the first cut.
            }

            // PHASE-MATCHED JOIN (28 Aug 2026 ruling, the exit seam): the
            // splice-resampler's wet reads at p + drift, and drift mod Tf
            // is INVARIANT under period-aligned splices — so at a seam the
            // wet meets the dry with a random sub-period offset, and the
            // crossfade joined two misaligned periodic signals: measured
            // 7.1 rough spans/s at 80c, ALL at the voiced->unvoiced exit,
            // 0.0 at the 0c control. Inside the fade the DRY LEG starts at
            // the wet's periodicity-equivalent offset (the drift REMAINDER,
            // centred) and eases home following the fade's own amplitude
            // curve — offset r0*g, so the two agree by construction: g=1
            // matched to the wet, g=0 bit-exact true dry. Unvoiced samples
            // (g<=0) are untouched: the sacred-dry contract covers content
            // AND the mix paths (SlotWetBlend, master MIX) that sum this
            // leg against a true-dry copy — which is also why the offset
            // could not simply ride through the seam.
            if (debugOn_)
                dbgSpl_.push_back ({ g, methodMix_, (float) spliceR_,
                                     (float) spliceDrift_, spliceT_ });

            float dryLeg = dry;
            if (g > 0.0f && g < 1.0f && spliceTf_ > 4.0 && methodMix_ < 1.0f)
            {
                double r0 = std::fmod (spliceDrift_, spliceTf_);
                if (r0 >  spliceTf_ * 0.5) r0 -= spliceTf_;
                if (r0 < -spliceTf_ * 0.5) r0 += spliceTf_;
                dryLeg = readInterp ((double) p + r0 * (double) g);
            }
            float y = g <= 0.0f ? dry : (g >= 1.0f ? wet
                                                   : dryLeg + g * (wet - dryLeg));

            // Blend against the delay-matched dry, then trim. Skipped entirely
            // at the defaults so nothing is multiplied that need not be.
            const float m = mix_.load (std::memory_order_relaxed);
            if (m < 1.0f) y = dry + m * (y - dry);
            const float og = outGain_.load (std::memory_order_relaxed);
            if (og != 1.0f) y *= og;
            out[i] = y;

            acc_[(size_t) idx] = 0.0f;
            win_[(size_t) idx] = 0.0f;
        }
        emitted_ = base + (int64_t) n;
    }

    // 0 at and beyond an unvoiced sample, 1 well inside a voiced span, linear
    // across kSeamFadeMs on the voiced side of each edge.
    float seamGain (uint64_t p) const noexcept
    {
        if (! voicedAt (p)) return 0.0f;

        int back = 0;
        while (back < seamFade_ && p >= (uint64_t) (back + 1) && voicedAt (p - (uint64_t) (back + 1)))
            ++back;

        int fwd = 0;
        while (fwd < seamFade_ && voicedAt (p + (uint64_t) (fwd + 1)))
            ++fwd;

        const int edge = std::min (back, fwd);
        if (edge >= seamFade_) return 1.0f;
        return (float) (edge + 1) / (float) (seamFade_ + 1);
    }

    bool voicedAt (uint64_t p) const noexcept
    {
        if (p >= write_) return false;
        return f0_[(size_t) ((uint32_t) p & mask_)] > 0.0f;
    }

    float f0At (uint64_t p) const noexcept
    {
        if (p >= write_) return 0.0f;
        return f0_[(size_t) ((uint32_t) p & mask_)];
    }

    // ---- analysis: find the next epoch -------------------------------------
    // Peak-pick inside [0.7T, 1.3T] past the previous epoch. Restricting the
    // search to a window around one period is what makes placement CONSISTENT
    // - the same phase point every period - which is what grains need in order
    // to overlap coherently.
    bool nextEpoch (uint64_t from, uint64_t limit, uint64_t& epochOut) noexcept
    {
        const float f0 = f0At (from);
        if (f0 <= 0.0f) return false;

        const int T = std::clamp ((int) std::lround (fs_ / (double) f0), 8, maxPeriod_);
        const uint64_t lo = from + (uint64_t) std::max (1, (int) (0.7 * T));
        const uint64_t hi = from + (uint64_t) std::max (2, (int) (1.3 * T));
        if (hi >= limit) return false;

        uint64_t best = lo;
        float    bestV = -1.0e30f;
        for (uint64_t p = lo; p <= hi; ++p)
        {
            const float v = in_[(size_t) ((uint32_t) p & mask_)];
            if (v > bestV) { bestV = v; best = p; }
        }

        // PHASE REFINEMENT (PITCH_P0_VALIDATION.md §16). Peak-picking is only
        // consistent to a few samples on real glottal pulses - breathy or
        // double-peaked periods move the maximum around inside the pulse -
        // and every misplaced epoch makes its grain sum against its
        // neighbours out of phase. Measured on the reference take that cost
        // -1.0 dB HNR and +19% spectral flux at UNITY, with no pitch change
        // asked for. Refine the picked peak +/-8 samples to the offset whose
        // one-period window best correlates with the PREVIOUS epoch's window:
        // the train becomes phase-consistent (the property the grains need),
        // while the coarse pick still decides which pulse is the epoch, so
        // re-spacing and pitch are untouched. The refinement is relative to
        // the previous ANALYSIS epoch - never to synthesis placement - so it
        // cannot fight the re-spacing (a placement-time aligner was tried
        // first and measurably pulled shifted output back toward the source
        // pitch).
        constexpr int kRefine = 8;
        const int W = std::min (T, 320);
        const int64_t aFrom = (int64_t) from - W / 2;
        const int64_t bFrom = (int64_t) best - W / 2 - kRefine;
        const int64_t oldest = (int64_t) write_ - (int64_t) (mask_ + 1);
        if (aFrom > oldest && aFrom >= 0 && bFrom > oldest && bFrom >= 0
            && best + (uint64_t) (W / 2 + kRefine) < limit)
        {
            double aa = 0.0;
            for (int i = 0; i < W; ++i)
            {
                const double a = in_[(size_t) ((uint32_t) (uint64_t) (aFrom + i) & mask_)];
                aa += a * a;
            }
            if (aa > 1.0e-12)
            {
                int bestD = 0;
                double bestC = -1.0e30;
                for (int d = -kRefine; d <= kRefine; ++d)
                {
                    double ab = 0.0, bb = 0.0;
                    for (int i = 0; i < W; ++i)
                    {
                        const double a = in_[(size_t) ((uint32_t) (uint64_t) (aFrom + i) & mask_)];
                        const double b = in_[(size_t) ((uint32_t) (uint64_t) ((int64_t) best + d - W / 2 + i) & mask_)];
                        ab += a * b; bb += b * b;
                    }
                    const double c = ab / std::sqrt (std::max (1.0e-12, aa * bb));
                    if (c > bestC) { bestC = c; bestD = d; }
                }
                best = (uint64_t) ((int64_t) best + bestD);
            }
        }

        epochOut = best;
        return true;
    }

    // Seed epoch tracking at the start of a voiced span: take the largest peak
    // in the first period, so the first grain is placed on a real pulse rather
    // than wherever the span happened to begin.
    bool seedEpoch (uint64_t from, uint64_t limit, uint64_t& epochOut) noexcept
    {
        const float f0 = f0At (from);
        if (f0 <= 0.0f) return false;
        const int T = std::clamp ((int) std::lround (fs_ / (double) f0), 8, maxPeriod_);
        if (from + (uint64_t) T >= limit) return false;

        uint64_t best = from;
        float    bestV = -1.0e30f;
        for (uint64_t p = from; p < from + (uint64_t) T; ++p)
        {
            const float v = in_[(size_t) ((uint32_t) p & mask_)];
            if (v > bestV) { bestV = v; best = p; }
        }
        epochOut = best;
        return true;
    }

    // ---- synthesis ---------------------------------------------------------
    // Place synthesis epochs at the TARGET period, each drawing its grain from
    // the nearest analysis epoch, until everything that can touch `upTo` has
    // been added.
    void advanceSynthesis (int64_t upToSigned, int64_t base, float target) noexcept
    {
        if (upToSigned < 0) return;
        const uint64_t upTo = (uint64_t) upToSigned;

        // Never read input that has not arrived, and leave a period of margin
        // beyond the emit point so epoch search never runs off the end. The
        // margin uses the ACTIVE period, not the worst case: latency_ is a few
        // active periods, so a worst-case margin would sit BEHIND the emit
        // point and synthesis could never catch up.
        const int marginSmp = std::max (curPeriod_, latency_ - curPeriod_);
        const uint64_t safeLimit = write_ > (uint64_t) marginSmp
                                 ? write_ - (uint64_t) marginSmp : 0;

        if (! haveSynth_ || nextSynth_ + (uint64_t) (4 * curPeriod_) < (uint64_t) std::max<int64_t> (base, 0))
        {
            if (debugOn_ && haveSynth_) dbgReset_.push_back ((uint64_t) std::max<int64_t> (base, 0));
            nextSynth_ = (uint64_t) std::max<int64_t> (base, 0);
            haveSynth_ = true;
            haveEpoch_ = false;
        }

        int guard = 0;
        while (nextSynth_ < upTo && nextSynth_ < safeLimit && ++guard < 4096)
        {
            const float f0 = f0At (nextSynth_);
            if (f0 <= 0.0f)
            {
                // Unvoiced: nothing to place. Step forward and drop epoch
                // tracking so the next voiced span re-seeds on a real pulse.
                ++nextSynth_;
                haveEpoch_ = false;
                synthFrac_ = 0.0;
                continue;
            }

            const int Ta = std::clamp ((int) std::lround (fs_ / (double) f0), 8, maxPeriod_);

            // Keep the analysis epoch train up to date around this position.
            if (! haveEpoch_ || lastEpoch_ + (uint64_t) (2 * Ta) < nextSynth_)
            {
                uint64_t e;
                if (! seedEpoch (nextSynth_, safeLimit, e)) break;
                if (debugOn_) dbgReseed_.push_back (nextSynth_);
                lastEpoch_ = e;
                haveEpoch_ = true;

                // PHASE-ALIGNED ENTRY (PITCH_P0_VALIDATION.md §16). Snap the
                // synthesis grid onto the seeded pulse: the first grain's
                // content offset is then ZERO, so the wet starts in phase
                // with the dry it is about to crossfade from, instead of up
                // to a full period out - which is what made every
                // voiced/unvoiced seam a phase discontinuity, and the seams
                // are exactly where the reference A/B measured the HNR loss
                // concentrating. No hole opens: the grain's left half spans a
                // full period, which reaches back past where the grid stood.
                if (e > nextSynth_)
                {
                    nextSynth_ = e;
                    synthFrac_ = 0.0;
                    if (nextSynth_ >= upTo || nextSynth_ >= safeLimit) break;
                }
            }
            while (lastEpoch_ < nextSynth_)
            {
                uint64_t e;
                if (! nextEpoch (lastEpoch_, safeLimit, e)) break;
                lastEpoch_ = e;
            }

            // The target period, with the ratio clamped so an absurd
            // source/target combination degrades rather than explodes.
            float ratio = curShift_ > kNoShift + 1.0f
                              ? std::exp2 (curShift_ / 1200.0f)
                              : target / f0;   // legacy: crosses the latency
            ratio = std::clamp (ratio, 1.0f / kMaxRatio, kMaxRatio);
            const int Ts = std::max (4, (int) std::lround ((double) Ta / (double) ratio));

            placeGrain (lastEpoch_, nextSynth_, Ta, Ts, base, synthFrac_);

            // FRACTIONAL SPACING, error-diffused. Advancing by the rounded
            // Ts biases every period by up to half a sample - a persistent
            // few-cents offset that also drags the synthesis grid across the
            // analysis epochs, forcing extra content swaps. Diffusing the
            // rounding error keeps the average spacing exact; each grain
            // still lands on an integer sample.
            const double tsd = std::max (4.0, (double) Ta / (double) ratio);
            synthFrac_ += tsd;
            const int stepN = std::max (4, (int) synthFrac_);
            synthFrac_ -= (double) stepN;
            nextSynth_ += (uint64_t) stepN;
        }
    }

    // Hann-windowed grain of kGrainPeriods * Ta, centred on the analysis
    // epoch, added at the synthesis epoch. The window is accumulated
    // alongside so the overlap-add can be normalised.
    void placeGrain (uint64_t analysisEpoch, uint64_t synthEpoch, int Ta, int Ts,
                     int64_t emitFloor, double posFrac) noexcept
    {
        const int mode = formantMode_.load (std::memory_order_relaxed);

        // PRESERVE and SHIFT share the LPC-PSOLA pipeline: the grain is
        // flattened to its residual, the RESIDUAL is overlap-added, and the
        // envelope is re-applied by one continuous synthesis filter at emit
        // time (unwarped for preserve, warped for shift). See
        // placeGrainResidual() for why raw-grain OLA was retired.
        if (mode != kFormantOff)
        {
            placeGrainResidual (analysisEpoch, synthEpoch, Ta, Ts, emitFloor,
                                mode == kFormantShift, posFrac);
            return;
        }

        // OFF: the grain is RESAMPLED by the pitch ratio as it is placed, so
        // the pulse is compressed or stretched along with the pitch and the
        // envelope moves with it. The chipmunk/resampler behaviour, kept
        // because it is occasionally exactly what someone wants.
        // OFF reads exactly ONE input period per grain (+/- Ta/2, which after
        // resampling is +/- Ts/2 of output). Measured in P1: any wider span
        // pulls in a second pulse and the output lands 314 cents sharp.
        const int half = std::max (2, std::min (Ts / 2, maxPeriod_));
        const double step = (double) Ta / (double) std::max (1, Ts);
        const int len = 2 * half + 1;

        for (int k = -half; k <= half; ++k)
        {
            const int64_t dst = (int64_t) synthEpoch + k;
            if (dst < emitFloor) continue;                 // already emitted

            const double srcPos = (double) (int64_t) analysisEpoch
                                + ((double) k - posFrac) * step;
            if (srcPos < 0.0) continue;
            const int64_t s0 = (int64_t) std::floor (srcPos);
            if ((uint64_t) s0 + 1 >= write_) continue;

            const float frac = (float) (srcPos - (double) s0);
            const float a0 = in_[(size_t) ((uint32_t) (uint64_t) s0 & mask_)];
            const float a1 = in_[(size_t) ((uint32_t) (uint64_t) (s0 + 1) & mask_)];
            const float x  = a0 + frac * (a1 - a0);

            // Hann across the grain.
            const float ph = (float) (k + half) / (float) (len - 1);
            const float w  = 0.5f - 0.5f * std::cos (6.283185307179586f * ph);

            const uint32_t di = (uint32_t) (uint64_t) dst & mask_;
            acc_[(size_t) di] += w * x;
            win_[(size_t) di] += w;
        }
        if (debugOn_) dbgGrain_.push_back ({ synthEpoch, Ta, Ts, half, 1.0f });
        placedTo_ = std::max (placedTo_, synthEpoch + (uint64_t) half);
    }

    // ---- preserve & shift: LPC-PSOLA, residual OLA + emit-time envelope ----
    //
    // WHY RAW-GRAIN OLA WAS RETIRED for preserve (the A/B against Antares,
    // PITCH_P0_VALIDATION.md §16): overlap-adding raw grains sums time-offset
    // copies of the vocal-tract RING, and the copies never align exactly -
    // epoch picking jitters a few samples on real glottal pulses, Ts rounds
    // to integers, and every misalignment combs the spectrum differently
    // from one grain to the next. Measured on the reference take, that cost
    // -1.0 dB of HNR and +19% spectral flux WITH NO PITCH CHANGE AT ALL
    // (unity resynthesis), where Antares loses -0.2 dB / +4%. The fix is the
    // classic LPC-PSOLA architecture:
    //
    //   analyse   A(z)  = LPC of the Hann-windowed grain (autocorrelation
    //                     method, Levinson-Durbin, order ~ 2 + fs/1000),
    //   flatten   e[n]  = the residual, computed against the REAL ring
    //                     history so it is exact,
    //   move            = overlap-add the RESIDUAL grains at the target
    //                     period - residual pulses are impulsive, so
    //                     misaligned copies hurt far less than misaligned
    //                     rings,
    //   re-ring         = ONE continuous all-pole synthesis filter at emit
    //                     time, coefficients switched per synthesis epoch
    //                     (a coefficient ring travels with the grains),
    //                     state carried sample to sample - the ring is
    //                     generated once, never summed against itself.
    //
    // PRESERVE uses A(z) unwarped. SHIFT re-applies the envelope with its
    // frequency axis scaled by beta = 2^(shift/12): P(w) = E/|A(w)|^2 on a
    // dense grid, read back at w/beta, cosine-transformed to an
    // autocorrelation (positive definite by construction) and Levinson'd.
    // Warping the raw autocorrelation at scaled lags was tried first and
    // measured broken - r(tau) of near-Nyquist content interpolates into an
    // indefinite sequence and Levinson collapses (Ew 2.5e-16 against a
    // healthy 1.9e-3).
    //
    // Level for shift: filtering the (variance-E) residual through 1/A'
    // multiplies variance by rw[0]/E', so the residual is scaled by
    // sqrt((E'/E) * (r[0]/rw[0])) and output energy lands at the grain's
    // own regardless of how the warp reshaped the envelope. Preserve needs
    // no scale: the residual of A filtered through 1/A reconstructs the
    // grain's own level by definition.
    //
    // At shift = 0 the warp is skipped entirely, so shift-at-zero and
    // preserve are the SAME code path, not merely similar sounds.
    void placeGrainResidual (uint64_t analysisEpoch, uint64_t synthEpoch, int Ta, int Ts,
                             int64_t emitFloor, bool warp, double posFrac) noexcept
    {
        const int half = std::min (Ta, maxPeriod_);
        const int len  = 2 * half + 1;
        const float shiftSt = warp ? formantShift_.load (std::memory_order_relaxed) : 0.0f;
        const double beta = std::pow (2.0, (double) shiftSt / 12.0);
        const bool doWarp = warp && std::fabs (shiftSt) > 1.0e-4f;

        const int p = std::min (lpcOrder_, (len - 2) / 2);

        const int64_t s0 = (int64_t) analysisEpoch - half;    // first grain sample
        const int64_t h0 = s0 - p;                             // first history sample
        const int64_t oldest = (int64_t) write_ - (int64_t) (mask_ + 1);

        // Degenerate geometry, stream edges, or history outside the ring:
        // the grain goes into the OLA raw, with an IDENTITY filter entry so
        // the emit filter passes it through unchanged - the right sound for
        // the edge of a voiced span anyway.
        bool canModel = p >= 4 && h0 >= 0 && h0 > oldest
                        && (uint64_t) (s0 + len) <= write_
                        && (size_t) len <= lpcE_.size();
        if (dbgRawGrains_) canModel = false;      // diagnostic A/B only

        // PRESERVE'S GRAINS ARE ALWAYS RAW (identity filter entries). The
        // LPC residual + emit-filter pipeline was measured against raw OLA
        // with everything else equal and LOST on the reference take - unity
        // HNR 5.70 vs 6.14, flux +21% vs +15%, and +5 st transpose HNR 6.47
        // vs 7.30 - the per-epoch coefficient switching costs more than the
        // ring-summing it was meant to cure. The model runs only for
        // formant_mode = shift, where the envelope warp requires it.
        if (! warp)
            canModel = false;

        double E = 0.0, Ew = 0.0;
        double g = 1.0;
        const double* coefs = lpcA_.data();
        if (canModel)
        {
            // Contiguous copy: history then grain.
            for (int i = 0; i < p + len; ++i)
                lpcX_[(size_t) i] = in_[(size_t) ((uint32_t) (uint64_t) (h0 + i) & mask_)];

            // Hann-windowed copy for the autocorrelation.
            double r0 = 0.0;
            for (int i = 0; i < len; ++i)
            {
                const float ph = (float) i / (float) (len - 1);
                const float w  = 0.5f - 0.5f * std::cos (6.283185307179586f * ph);
                lpcWin_[(size_t) i] = w * lpcX_[(size_t) (p + i)];
                r0 += (double) lpcWin_[(size_t) i] * lpcWin_[(size_t) i];
            }

            if (r0 < 1.0e-12)
                canModel = false;                     // silence: nothing to model
            else
            {
                for (int k = 0; k <= p; ++k)
                {
                    double s = 0.0;
                    for (int i = 0; i + k < len; ++i)
                        s += (double) lpcWin_[(size_t) i] * (double) lpcWin_[(size_t) (i + k)];
                    // Gaussian LAG WINDOW (the G.729/AMR conditioning trick):
                    // a sum of near-pure harmonics is predictable to machine
                    // precision, so the raw prediction error can collapse to
                    // ~0 and the model degenerates into line spectra that
                    // Levinson (and the warp's lag interpolation) cannot
                    // handle. Convolving the envelope with a ~40 Hz Gaussian
                    // gives every line a finite bandwidth: E stays bounded
                    // away from zero, r(tau) decays smoothly enough to
                    // interpolate, and 40 Hz is well under any formant
                    // bandwidth this control claims to move.
                    const double lw = 6.2831853 * 40.0 * (double) k / fs_;
                    lpcR_[(size_t) k] = s * std::exp (-0.5 * lw * lw);
                }
                // A touch of ridge on top for the zero-lag term.
                lpcR_[0] *= 1.0001;

                E = levinson (lpcR_.data(), p, lpcA_.data());
                if (E <= 1.0e-9 * lpcR_[0])
                    canModel = false;

                if (canModel && doWarp)
                {
                    // THE WARP HAPPENS IN THE ENVELOPE DOMAIN: evaluate
                    // P(w) = E/|A(w)|^2 on a pi/512 grid (one complex
                    // rotation per coefficient), read it back at w/beta
                    // (hold the Nyquist value beyond the source band),
                    // cosine-transform to an autocorrelation and Levinson.
                    // Grid spacing ~47 Hz at 48 kHz matches the ~40 Hz
                    // bandwidth floor the lag window guarantees, so no peak
                    // can fall between grid points.
                    const int M = kWarpGrid;
                    for (int m = 0; m <= M; ++m)
                    {
                        const double wm = 3.141592653589793 * (double) m / (double) M;
                        const double cw = std::cos (wm), sw = std::sin (wm);
                        double cr = 1.0, ci = 0.0;          // e^{-j w k}, k = 0
                        double re = 1.0, im = 0.0;
                        for (int k = 1; k <= p; ++k)
                        {
                            const double nr = cr * cw + ci * sw;    // rotate by -w
                            const double ni = ci * cw - cr * sw;
                            cr = nr; ci = ni;
                            re -= lpcA_[(size_t) k] * cr;
                            im -= lpcA_[(size_t) k] * ci;
                        }
                        lpcP_[(size_t) m] = E / std::max (1.0e-12, re * re + im * im);
                    }
                    for (int m = 0; m <= M; ++m)
                    {
                        const double t = (double) m / beta;          // grid position of w/beta
                        const int    m0 = (int) t;
                        const double fr = t - (double) m0;
                        lpcPw_[(size_t) m] = m0 >= M ? lpcP_[(size_t) M]
                                           : (1.0 - fr) * lpcP_[(size_t) m0] + fr * lpcP_[(size_t) (m0 + 1)];
                    }
                    for (int k = 0; k <= p; ++k)
                    {
                        // Trapezoid cosine series over [0, pi]; rotation again.
                        const double ck = std::cos (3.141592653589793 * (double) k / (double) M);
                        const double sk = std::sin (3.141592653589793 * (double) k / (double) M);
                        double cr = 1.0, ci = 0.0;
                        double s = 0.5 * lpcPw_[0];
                        for (int m = 1; m < M; ++m)
                        {
                            const double nr = cr * ck - ci * sk;
                            const double ni = ci * ck + cr * sk;
                            cr = nr; ci = ni;
                            s += lpcPw_[(size_t) m] * cr;
                        }
                        s += 0.5 * lpcPw_[(size_t) M] * ((k & 1) != 0 ? -1.0 : 1.0);
                        lpcRw_[(size_t) k] = s / (double) M;
                    }
                    Ew = levinson (lpcRw_.data(), p, lpcAw_.data());

                    // RELATIVE degeneracy guard: only an actual collapse
                    // falls back to the unwarped envelope.
                    if (Ew > 1.0e-9 * lpcRw_[0] && lpcRw_[0] > 0.0)
                    {
                        coefs = lpcAw_.data();
                        g = std::clamp (
                            std::sqrt ((Ew / E) * (lpcR_[0] / std::max (1.0e-12, lpcRw_[0]))),
                            0.0625, 16.0);
                    }
                }
            }
        }

        // The upshift make-up (measured law, clamp(Ta/Ts, 1, 2) - see the
        // level suite) now rides on the RESIDUAL: overlapping residual
        // grains still average time-offset copies of the same excitation
        // pulse, and the synthesis filter is linear, so the loss and its
        // cure sit in the same place they always did.
        const float makeup = std::clamp ((float) Ta / (float) Ts, 1.0f, 2.0f);
        const float grainGain = makeup * (float) g;

        if (canModel)
        {
            // Flatten: exact residual against the real history.
            for (int n = 0; n < len; ++n)
            {
                double e = (double) lpcX_[(size_t) (p + n)];
                for (int k = 1; k <= p; ++k)
                    e -= lpcA_[(size_t) k] * (double) lpcX_[(size_t) (p + n - k)];
                lpcE_[(size_t) n] = (float) e;
            }
        }
        else
        {
            // Raw grain with an identity filter entry.
            for (int n = 0; n < len; ++n)
            {
                const int64_t sp = s0 + n;
                lpcE_[(size_t) n] = sp >= 0 && (uint64_t) sp < write_
                    ? in_[(size_t) ((uint32_t) (uint64_t) sp & mask_)] : 0.0f;
            }
        }

        for (int k = -half; k <= half; ++k)
        {
            const int64_t dst = (int64_t) synthEpoch + k;
            if (dst < emitFloor) continue;

            const float ph = (float) (k + half) / (float) (len - 1);
            const float w  = 0.5f - 0.5f * std::cos (6.283185307179586f * ph);

            // SUB-SAMPLE PLACEMENT: the error-diffused grid still lands on
            // integer samples; reading the content at the grid's fractional
            // error puts every grain at its EXACT ideal position, so grains
            // stop jittering half a sample against each other.
            const double mpos = (double) (k + half) - posFrac;
            const int    m0   = (int) std::floor (mpos);
            const double fr   = mpos - (double) m0;
            const float  e0   = m0 >= 0 && m0 < len ? lpcE_[(size_t) m0] : 0.0f;
            const float  e1   = m0 + 1 >= 0 && m0 + 1 < len ? lpcE_[(size_t) (m0 + 1)] : 0.0f;
            const float  v    = (float) ((1.0 - fr) * (double) e0 + fr * (double) e1);

            const uint32_t di = (uint32_t) (uint64_t) dst & mask_;
            acc_[(size_t) di] += grainGain * w * v;
            win_[(size_t) di] += w;
        }

        pushCoefEntry (synthEpoch, canModel ? p : 0, coefs);

        if (debugOn_) dbgGrain_.push_back ({ synthEpoch, Ta, Ts, half, grainGain });
        placedTo_ = std::max (placedTo_, synthEpoch + (uint64_t) half);
    }

    // ---- the coefficient ring and the emit-time synthesis filter -----------
    // Entries travel with the grains: one per synthesis epoch, consumed in
    // input-time order by the emit filter. Placement runs at least two
    // periods ahead of emission at every block size (advanceSynthesis's
    // margin), so an entry always exists before the samples it governs are
    // emitted - which is what keeps fixed-block exactness intact.
    static constexpr int kCoefRing = 256;

    void pushCoefEntry (uint64_t pos, int ord, const double* a) noexcept
    {
        if (coefData_.empty()) return;
        // Keep positions monotonic: a synthesis-cursor reset can re-place at
        // or before the last epoch; overwrite the newest entry rather than
        // breaking the reader's ordered walk.
        size_t idx;
        if (coefHead_ > coefTail_ && coefPos_[(size_t) ((coefHead_ - 1) & (kCoefRing - 1))] >= pos)
            idx = (size_t) ((coefHead_ - 1) & (kCoefRing - 1));
        else
        {
            idx = (size_t) (coefHead_ & (kCoefRing - 1));
            ++coefHead_;
            if (coefHead_ - coefTail_ > kCoefRing) coefTail_ = coefHead_ - kCoefRing;
        }
        coefPos_[idx] = pos;
        coefOrd_[idx] = ord;
        if (ord > 0)
            std::copy (a + 1, a + 1 + ord,
                       coefData_.begin() + (long) (idx * (size_t) (kMaxLpcOrder + 1)));
    }

    // One filter step at output position p, driven by the (normalised)
    // residual. State is the filter's OWN recent output; the caller pushes
    // dry samples through pushSynthState() on unvoiced/passthrough spans so
    // a voiced re-entry rings from reality instead of from silence.
    float synthStep (uint64_t p, float resid) noexcept
    {
        while (coefCur_ + 1 < coefHead_
               && coefPos_[(size_t) ((coefCur_ + 1) & (kCoefRing - 1))] <= p)
            ++coefCur_;
        if (coefCur_ < coefTail_) coefCur_ = coefTail_ - 1;   // aged out: identity

        double y = resid;
        if (coefCur_ >= coefTail_)
        {
            const size_t idx = (size_t) (coefCur_ & (kCoefRing - 1));
            const int ord = coefOrd_[idx];
            const double* a = coefData_.data() + idx * (size_t) (kMaxLpcOrder + 1);
            for (int k = 1; k <= ord; ++k)
                y += a[(size_t) (k - 1)]
                   * synState_[(size_t) ((synIdx_ - (uint32_t) (k - 1)) & (uint32_t) (kMaxLpcOrder - 1))];
            // Degrade, never explode: coefficient switches on a razor frame
            // can transient; the clamp bounds it at the same level the old
            // per-grain path used.
            y = std::clamp (y, -4.0, 4.0);
        }
        pushSynthState ((float) y);
        return (float) y;
    }

    void pushSynthState (float y) noexcept
    {
        synIdx_ = (synIdx_ + 1) & (uint32_t) (kMaxLpcOrder - 1);
        synState_[(size_t) synIdx_] = (double) y;
    }

    // ---- the splice-resampler (preserve, inside the band) ------------------
    // Half-band each side of unity where preserve resamples instead of
    // granulating. Correction at retune 0 lives within +/-50 cents of unity;
    // 2.5 st covers every note-sized transient the retune envelope passes
    // through, while the formant error the resample introduces stays bounded
    // at a level the ear does not attribute to character change.
    static constexpr float kSpliceBandSt = 2.5f;

    // Catmull-Rom, not linear. The §16.10 burr hunt ended here: with every
    // control-path suspect measured and cleared (no decision change, no
    // splice, no method flip, no seam, healthy window, no drift-displaced
    // source transient - dry sharpness at the clicks 10.9x against a 9.4x
    // baseline), the remaining mechanism was the read itself. Linear
    // interpolation's error is O(h^2 * x''), which is negligible on smooth
    // waveform and explodes exactly at sharp glottal closure edges, fading
    // in and out as the fractional phase drifts - a once-per-period sizzle
    // on hard-glottal material, at a rate that matches the measured 1.85
    // clicks/s and their clustering. Four taps instead of two.
    float readInterp (double pos) const noexcept
    {
        if (pos < 1.0) return 0.0f;
        const int64_t i0 = (int64_t) pos;
        if ((uint64_t) (i0 + 2) >= write_)
            return in_[(size_t) ((uint32_t) (uint64_t) std::min<int64_t> (i0, (int64_t) write_ - 1) & mask_)];
        const float fr = (float) (pos - (double) i0);
        const float xm = in_[(size_t) ((uint32_t) (uint64_t) (i0 - 1) & mask_)];
        const float x0 = in_[(size_t) ((uint32_t) (uint64_t) i0 & mask_)];
        const float x1 = in_[(size_t) ((uint32_t) (uint64_t) (i0 + 1) & mask_)];
        const float x2 = in_[(size_t) ((uint32_t) (uint64_t) (i0 + 2) & mask_)];
        return x0 + 0.5f * fr * (x1 - xm
                   + fr * (2.0f * xm - 5.0f * x0 + 4.0f * x1 - x2
                   + fr * (3.0f * (x0 - x1) + x2 - xm)));
    }

    float spliceSample (uint64_t p, int T, double Tf, double r) noexcept
    {
        spliceDrift_ += r - 1.0;

        // DRIFT BLEED (5 Sep 2026 ruling): instead of anchoring each voiced
        // span at its own entry and discharging the accumulated (r-1)*span
        // as a content reset at the next gap - measured as the near-zero-
        // shift period inversions in the field - bleed the drift back
        // continuously as a small read-rate offset. The error goes into
        // PITCH, where a few transient cents sit below notice, instead of
        // into WAVEFORM CONTINUITY, where a period inversion is
        // unambiguously audible. Proportional (drift/tau, tau 100 ms) so
        // small drifts decay without overshoot, CAPPED at 3 cents of
        // momentary detune so a heavily fragmented span can never turn the
        // bleed into an audible glide - the cap, not the fragmentation,
        // bounds the pitch excursion. The known cost, priced by the tuning
        // gates: any sustained shift above the cap is undershot by up to
        // the cap at equilibrium. Consonants stay bit-exact dry: this runs
        // only inside spliceSample, which only voiced samples reach.
        // (Bleed-to-nearest-multiple was built, measured IDENTICAL - the
        // policies coincide wherever |drift| < T/2, which is everywhere
        // that occurs - and REVERTED by ruling, 29 Aug 2026: extra
        // machinery justified only by a regime the measurement says does
        // not happen. See commit 1151b4b for the latch design and numbers.)
        //
        // SHIFT-GATED (29 Aug 2026 ruling): the bleed can bound drift only
        // while |r-1| < kBleedMaxRate - below the cap an equilibrium
        // exists; above it accumulation outruns the cap, splices do the
        // bounding anyway, and a running bleed is pure convergence tax
        // (measured on a noiseless steady tone at hard: 3.38c note-centre
        // undershoot with the bleed on vs 0.42c off). The gate is a smooth
        // TAPER of |r-1|/cap (1 below 0.7, 0 above 1.3) through a 100 ms
        // one-pole. It cannot chatter: there is no feedback path - the
        // gate depends only on the corrector-side ratio, which the bleed's
        // output-side pitch effect never reaches - so it can only be
        // DRIVEN, and a vibrato crossing the band at ~6 Hz is attenuated
        // ~4x by the ~1.6 Hz pole on top of the near-equilibrium bleed
        // already being small. Sustained correction runs untaxed; the
        // near-zero-shift blink discharges the bleed was built for keep it.
        if (driftBleed_)
        {
            const double x = std::fabs (r - 1.0) / kBleedMaxRate;
            const double gT = x <= 0.7 ? 1.0 : x >= 1.3 ? 0.0 : (1.3 - x) / 0.6;
            bleedGate_ += (gT - bleedGate_) * bleedGateK_;
            const double b = std::clamp (spliceDrift_ / bleedTau_,
                                         -kBleedMaxRate, kBleedMaxRate)
                           * (dbgBleedUngated_ ? 1.0 : bleedGate_);
            spliceDrift_ -= b;
        }

        // Trigger a period-aligned splice before the drift can outrun the
        // lookahead: jump one FRACTIONAL period (fs / f0, not the rounded T
        // - the integer round misaligned the two copies by up to half a
        // sample, which the crossfade turned into HF phase ripple), so the
        // waveform phase is unchanged, and crossfade over ~4 ms.
        if (spliceFadeLen_ == 0 && std::fabs (spliceDrift_) > 0.75 * (double) T)
        {
            spliceOldDrift_ = spliceDrift_;
            spliceDrift_   += spliceDrift_ > 0.0 ? -Tf : Tf;
            spliceFadeLen_  = std::max (16, std::min (T / 2, (int) (0.004 * fs_)));
            spliceFadePos_  = 0;
            if (debugOn_) dbgSplice_.push_back (p);
        }

        float y = readInterp ((double) p + spliceDrift_);
        if (spliceFadeLen_ > 0)
        {
            spliceOldDrift_ += r - 1.0;
            // Raised-cosine, not linear: continuous slope at both ends of
            // the fade, so the join neither starts nor stops with a corner.
            const float lin = (float) spliceFadePos_ / (float) spliceFadeLen_;
            const float a   = 0.5f - 0.5f * std::cos (3.14159265f * lin);
            const float yOld = readInterp ((double) p + spliceOldDrift_);
            y = yOld + a * (y - yOld);
            if (++spliceFadePos_ >= spliceFadeLen_) spliceFadeLen_ = 0;
        }
        return y;
    }

    // Levinson-Durbin on autocorrelation r[0..p]; writes predictor
    // coefficients into a[1..p] (x[n] ~ sum a_k x[n-k]) and returns the
    // prediction-error energy. Reflection coefficients are clamped just
    // inside the unit circle so a razor-sharp resonance stays a filter
    // rather than an oscillator.
    static double levinson (const double* r, int p, double* a) noexcept
    {
        double E = r[0];
        for (int k = 0; k <= p; ++k) a[k] = 0.0;
        if (E <= 0.0) return 0.0;

        double tmp[kMaxLpcOrder + 1];
        for (int i = 1; i <= p; ++i)
        {
            double acc = r[i];
            for (int j = 1; j < i; ++j) acc -= a[j] * r[i - j];
            double kref = acc / E;
            kref = std::clamp (kref, -0.9995, 0.9995);

            for (int j = 1; j < i; ++j) tmp[j] = a[j] - kref * a[i - j];
            for (int j = 1; j < i; ++j) a[j] = tmp[j];
            a[i] = kref;

            E *= (1.0 - kref * kref);
            if (E <= 1.0e-15) break;
        }
        return E;
    }

    void recomputeLatency() noexcept
    {
        latency_    = std::max (curPeriod_, (int) (lookahead_ * (float) curPeriod_));
        maxLatency_ = std::max (maxLatency_,
                                (int) (kLookaheadMax * (float) maxPeriod_) + maxPeriod_);
    }

    // ---- state -------------------------------------------------------------
    double fs_ = 48000.0;
    float  lookahead_ = kLookaheadDefault;
    int    pitchLag_  = 0;
    bool   debugOn_   = false;
    bool   dbgRawGrains_ = false;
    bool   dbgNoSplice_  = false;
    bool   dbgMethodState_ = false;
    std::vector<uint64_t> dbgSplice_, dbgMethodFlip_;
    std::vector<uint64_t> dbgReseed_, dbgReset_;
    std::vector<DebugGrain> dbgGrain_;
    std::vector<DebugEmit>  dbgEmit_;
    std::vector<DebugSpl>   dbgSpl_;
    std::vector<uint32_t>   dbgWinHist_;
    float dbgWinMin_ = 1.0e9f;
    int    maxPeriod_  = 2048;
    int    curPeriod_  = 600;
    int    latency_    = 1800;
    int    maxLatency_ = 6144;
    int    seamFade_   = 72;

    std::vector<float> in_, f0_, acc_, win_;
    uint32_t mask_ = 0;

    // kFormantShift scratch - allocated in prepare(), never on the audio
    // thread. kMaxLpcOrder bounds the spec's 2 + fs/1000 at any sample rate
    // this plugin can be prepared at.
    static constexpr int kMaxLpcOrder = 128;
    static constexpr int kWarpGrid    = 512;
    int lpcOrder_ = 50;
    std::vector<float>  lpcX_, lpcY_, lpcE_, lpcWin_;
    std::vector<double> lpcR_, lpcRw_, lpcA_, lpcAw_, lpcP_, lpcPw_;

    // Coefficient ring (one entry per synthesis epoch) and the continuous
    // synthesis filter's state - the emit-time half of LPC-PSOLA.
    std::vector<uint64_t> coefPos_;
    std::vector<int>      coefOrd_;
    std::vector<double>   coefData_;
    int64_t coefHead_ = 0, coefTail_ = 0, coefCur_ = -1;
    std::vector<double>   synState_;
    uint32_t synIdx_ = 0;

    // Splice-resampler state.
    float  curTarget_ = 0.0f;
    double spliceDrift_ = 0.0, spliceOldDrift_ = 0.0, spliceR_ = 0.0, spliceTf_ = 0.0;
    // 3 cents as a read-rate offset: 2^(3/1200)-1. See spliceSample.
    static constexpr double kBleedMaxRate = 0.00173465;
    double bleedTau_  = 4800.0;    // set in prepare(): 100 ms at fs
    double bleedGate_ = 1.0;       // smoothed shift gate (see spliceSample)
    double bleedGateK_ = 1.0 / 4800.0;   // 100 ms pole, set in prepare()
    bool   driftBleed_ = false;
    bool   dbgBleedUngated_ = false;
public:
    // Ring-aligned fast vibrato term (2 Sep 2026 experiment; see the
    // splice block). k in 0..2 (natural_vibrato/100).
    void setFastRing (bool on, float k) noexcept
    { fastRingOn_ = on; fastK_ = std::clamp (k, 0.0f, 2.0f); }
    void setFastRingSlowHz (float hz) noexcept { fastSlowHz_ = hz; }
private:
    bool   fastRingOn_ = false;
    float  fastK_ = 1.0f;
    double ringSlowHz_ = 0.0;      // (unused by the one-reference cut)
    float  fastSlowHz_ = 0.0f;     // the corrector's slow track, per hop
    std::vector<float> slowRing_;  // lag-compensated slow reference
    double ringSlowK_ = 1.0 / (0.14 * 48000.0);   // set in prepare
    float  carryMs_ = 0.0f;        // drift carry threshold; 0 = off
    double carryLimit_ = 0.0;      // ...in samples, set with fs
    int64_t uvRun_ = 0;            // unvoiced run length at the emit head
    float  bridgeMaxMs_ = 0.0f;    // audio-verified bridge cap; 0 = off
    float  bridgeThresh_ = 0.6f;   // periodicity bar for bridging
    double bridgeSeedT_ = 0.0;     // last accepted period (samples); 0 = disarmed
    double bridgeLen_ = 0.0;       // bridged samples in the current run
    std::vector<DbgBridge> dbgBridge_;
    int    spliceFadeLen_ = 0, spliceFadePos_ = 0, spliceT_ = 0;
    float  methodMix_ = 0.0f;      // 0 = splice, 1 = grains


    uint64_t write_   = 0;    // absolute input samples written
    int64_t  emitted_ = 0;    // input position just past the last sample emitted
    uint64_t placedTo_ = 0;

    uint64_t lastEpoch_ = 0;
    bool     haveEpoch_ = false;
    uint64_t nextSynth_ = 0;
    bool     haveSynth_ = false;
    double   synthFrac_ = 0.0;

    std::atomic<float> targetHz_ { 0.0f };
    float curShift_ = -100000.0f;   // kNoShift; per-call, audio thread
    std::atomic<int>   formantMode_ { kFormantPreserve };
    std::atomic<float> formantShift_ { 0.0f };
    std::atomic<float> mix_     { 1.0f };
    std::atomic<float> outGain_ { 1.0f };
    std::atomic<float> outDb_   { 0.0f };
};

} // namespace echojay
