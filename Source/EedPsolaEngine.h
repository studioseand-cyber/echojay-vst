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
    // See placeGrainWarped() for the measured details, including why the warp
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
        nextSynth_ = 0; haveSynth_ = false;
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

    void setPitchLagSamples (int lag) noexcept { pitchLag_ = std::max (0, lag); }
    int  getPitchLagSamples() const noexcept   { return pitchLag_; }

    // ---- audio thread ------------------------------------------------------
    // Push n input samples with the detector's CURRENT reading, and pull the n
    // output samples that are `latencySamples()` behind them. f0 <= 0 or
    // voiced == false marks the span unvoiced.
    // `targetHz` is passed PER CALL rather than read from an atomic, so a
    // caller can slice a block at hop boundaries and give each slice the target
    // that hop actually decided. Passing 0 means passthrough. The atomic setter
    // remains for the fixed-target diagnostic path.
    void process (const float* in, float* out, int n, float f0Hz, bool voiced,
                  float targetHz) noexcept
    {
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

        const float track = (voiced && f0Hz > 0.0f) ? f0Hz : 0.0f;

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
        for (int i = 0; i < n; ++i)
        {
            const int64_t at = (int64_t) write_ - (int64_t) n + i - (int64_t) lag;
            if (at < 0 || at < emitted_) continue;
            f0_[(size_t) ((uint32_t) (uint64_t) at & mask_)] = track;
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
        emitMixed (out, n, base);
    }

private:
    // ---- output ------------------------------------------------------------
    void emitDry (float* out, int n, int64_t base) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = base + (int64_t) i;
            if (p < 0) { out[i] = 0.0f; if (debugOn_) dbgEmit_.push_back ({ -1.0f, 0.0f }); continue; }
            const uint32_t idx = (uint32_t) (uint64_t) p & mask_;
            const float og = outGain_.load (std::memory_order_relaxed);
            out[i] = og == 1.0f ? in_[(size_t) idx] : in_[(size_t) idx] * og;
            // Clear the accumulators as we pass, so a later switch back to
            // shifting does not read stale grain content.
            acc_[(size_t) idx] = 0.0f;
            win_[(size_t) idx] = 0.0f;
            if (debugOn_) dbgEmit_.push_back ({ -1.0f, 0.0f });
        }
        emitted_ = base + (int64_t) n;
    }

    void emitMixed (float* out, int n, int64_t base) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = base + (int64_t) i;
            if (p < 0) { out[i] = 0.0f; if (debugOn_) dbgEmit_.push_back ({ -1.0f, 0.0f }); continue; }
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
            const float wet = acc_[(size_t) idx] / std::max (w, kWindowFloor);

            // UNVOICED IS SACRED: the dry sample, untouched. Voiced samples
            // near the seam fade between wet and dry so the join is smooth,
            // and that fade lives entirely on the voiced side.
            const float g = seamGain ((uint64_t) p);
            if (debugOn_) dbgEmit_.push_back ({ g, w });
            float y = g <= 0.0f ? dry : (g >= 1.0f ? wet : dry + g * (wet - dry));

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
            }
            while (lastEpoch_ < nextSynth_)
            {
                uint64_t e;
                if (! nextEpoch (lastEpoch_, safeLimit, e)) break;
                lastEpoch_ = e;
            }

            // The target period, with the ratio clamped so an absurd
            // source/target combination degrades rather than explodes.
            float ratio = target / f0;
            ratio = std::clamp (ratio, 1.0f / kMaxRatio, kMaxRatio);
            const int Ts = std::max (4, (int) std::lround ((double) Ta / (double) ratio));

            placeGrain (lastEpoch_, nextSynth_, Ta, Ts, base);
            nextSynth_ += (uint64_t) Ts;
        }
    }

    // Hann-windowed grain of kGrainPeriods * Ta, centred on the analysis
    // epoch, added at the synthesis epoch. The window is accumulated
    // alongside so the overlap-add can be normalised.
    void placeGrain (uint64_t analysisEpoch, uint64_t synthEpoch, int Ta, int Ts,
                     int64_t emitFloor) noexcept
    {
        const int mode = formantMode_.load (std::memory_order_relaxed);

        // SHIFT branches before anything else touches the grain, so the
        // preserve and off paths below stay bit-identical to what was measured
        // and banked - formant_mode = preserve is the shipped default and its
        // output must not move by one bit for this feature's sake.
        if (mode == kFormantShift)
        {
            placeGrainWarped (analysisEpoch, synthEpoch, Ta, Ts, emitFloor);
            return;
        }

        const bool preserve = mode != kFormantOff;

        // PRESERVE: the grain is copied at 1:1, so the pulse keeps its own
        // duration and therefore its own spectral envelope. Re-spacing changes
        // only how OFTEN pulses arrive. Formants stay put - the whole point.
        //
        // OFF: the grain is RESAMPLED by the pitch ratio as it is placed, so
        // the pulse is compressed or stretched along with the pitch and the
        // envelope moves with it. This is the chipmunk/resampler behaviour,
        // kept because it is occasionally exactly what someone wants - and
        // because having both makes the formant work audible rather than a
        // claim in a comment.
        // OFF reads exactly ONE input period per grain (+/- Ta/2, which after
        // resampling is +/- Ts/2 of output). Measured: any wider span pulls in
        // a second pulse and the output lands 314 cents sharp - Ts, Ta/2 and Ta
        // all read 359.7 Hz where 300 was asked for, while Ts/2 reads 300.02.
        // PRESERVE reads +/-Ta at 1:1 - two periods, the standard TD-PSOLA
        // grain. OFF reads +/-Ta/2, ONE period, resampled by the pitch ratio:
        // measured in P1, a wider span pulls in a second pulse and the output
        // lands 314 cents sharp.
        const int half = preserve ? std::min (Ta, maxPeriod_)
                                  : std::max (2, std::min (Ts / 2, maxPeriod_));
        const double step = preserve ? 1.0 : (double) Ta / (double) std::max (1, Ts);

        const int len = 2 * half + 1;

        // Level make-up ON TOP of the per-sample window-sum normalisation.
        //
        // The normalisation (emitMixed) makes output amplitude invariant to
        // grain length - which is the click fix: PRESERVE used to apply an
        // open-loop sqrt(Ts/Ta) here with NO per-sample normalisation, so
        // every ordinary f0 update changed the next grain's length and gain
        // and the overlap-add stepped to a new amplitude ~20 samples after
        // the hop: 329 audible steps in 60 s of the acapella, 65% amplitude
        // steps (tools/pitch_click_test). An open-loop gain is correct on
        // average and wrong at every transition; the window sum is correct
        // per sample by construction.
        //
        // What the division alone gets wrong is upshift LEVEL: overlapping
        // grains carry time-offset copies of the same pulse, and averaging
        // misaligned copies loses energy - measured -2.92 dB at a fifth up,
        // -6.05 dB at an octave up (test/psola_engine_test.cpp). This gain
        // rides on top of the normalisation, so unlike the old one it can
        // never step the seam: where grains overlap, out = sum(g w x)/sum(w),
        // a crossfade between neighbouring gains over ~Ta samples.
        // A hop's typical sub-cent f0 move changes it by <0.1%.
        //
        // The LAW was picked by measurement, not derivation: linear Ta/Ts
        // against sqrt(Ta/Ts) across the suite's nine ratios - linear holds
        // every upshift within +1.16 dB of dry (octave-up -0.03), sqrt drifts
        // to -3.04 dB by the octave. Down-shifts are left uncompensated
        // (clamp floor 1): their loss is floored window GAPS, not averaging,
        // and boosting reconstructed patches would amplify the wrong thing.
        // Ceiling 2 = one octave of make-up, the "extreme for a corrector"
        // end of the tested range.
        const float gain = preserve
            ? std::clamp ((float) Ta / (float) Ts, 1.0f, 2.0f)
            : 1.0f;

        for (int k = -half; k <= half; ++k)
        {
            const int64_t dst = (int64_t) synthEpoch + k;
            if (dst < emitFloor) continue;                 // already emitted

            const double srcPos = (double) (int64_t) analysisEpoch + (double) k * step;
            if (srcPos < 0.0) continue;
            const int64_t s0 = (int64_t) std::floor (srcPos);
            if ((uint64_t) s0 + 1 >= write_) continue;

            // Linear interpolation: only ever used when OFF is resampling the
            // grain (step == 1 lands exactly on s0), so it costs nothing in the
            // mode that matters for quality.
            const float frac = (float) (srcPos - (double) s0);
            const float a0 = in_[(size_t) ((uint32_t) (uint64_t) s0 & mask_)];
            const float a1 = in_[(size_t) ((uint32_t) (uint64_t) (s0 + 1) & mask_)];
            const float x  = a0 + frac * (a1 - a0);

            // Hann across the grain.
            const float ph = (float) (k + half) / (float) (len - 1);
            const float w  = 0.5f - 0.5f * std::cos (6.283185307179586f * ph);

            const uint32_t di = (uint32_t) (uint64_t) dst & mask_;
            acc_[(size_t) di] += gain * w * x;
            win_[(size_t) di] += w;
        }
        if (debugOn_) dbgGrain_.push_back ({ synthEpoch, Ta, Ts, half, gain });
        placedTo_ = std::max (placedTo_, synthEpoch + (uint64_t) half);
    }

    // ---- kFormantShift: LPC envelope warp, per grain -----------------------
    //
    // The pipeline the spec prescribes, executed on each two-period grain:
    //
    //   analyse   A(z)  = LPC of the Hann-windowed grain (autocorrelation
    //                     method, Levinson-Durbin, order lpcOrder_),
    //   flatten   e[n]  = x[n] - sum a_k x[n-k]   - the residual, computed
    //                     against the REAL ring history so it is exact,
    //   shift             the residual grain is copied 1:1 and re-spaced at
    //                     the target period by the caller - the same PSOLA
    //                     move as preserve, applied to the flat signal,
    //   re-apply  A'(z) = the envelope with its frequency axis scaled by
    //                     beta = 2^(shift/12): P(w) = E/|A(w)|^2 evaluated on
    //                     a dense grid, read back at w/beta, cosine-
    //                     transformed to an autocorrelation and Levinson'd.
    //                     No pole root-finding, no FFT, and the resampled
    //                     curve is the SMOOTH model envelope - see the block
    //                     comment inside for why warping the raw
    //                     autocorrelation at scaled lags was tried first and
    //                     measured broken.
    //
    // Level: with the autocorrelation method the prediction-error energy is
    // E = r[0] * prod(1 - k_i^2), and filtering the (variance-E) residual
    // through 1/A' multiplies variance by rw[0]/E'. Scaling the residual by
    // sqrt((E'/E) * (r[0]/rw[0])) therefore lands the output back at the
    // grain's own energy regardless of how the warp reshaped the envelope.
    //
    // Continuity through zero is structural, not tuned: at shift = 0 the
    // envelope round trip reproduces A' ~= A, the residual is re-filtered by
    // (nearly) the filter that made it, and the control approaches the
    // preserve sound smoothly as it approaches 0 - measured, shift 0 and
    // preserve read the same formant within the tracker's resolution.
    //
    // The synthesis filter is seeded with the DRY history before the grain
    // rather than zeros: the Hann window silences the grain edges, and the
    // warped filter's start-up transient decays inside the half-period the
    // window is still rising through.
    void placeGrainWarped (uint64_t analysisEpoch, uint64_t synthEpoch, int Ta, int Ts,
                           int64_t emitFloor) noexcept
    {
        const int half = std::min (Ta, maxPeriod_);
        const int len  = 2 * half + 1;
        const float shiftSt = formantShift_.load (std::memory_order_relaxed);
        const double beta = std::pow (2.0, (double) shiftSt / 12.0);

        // Effective order: never more than the grain can support, and the
        // scaled lags must stay inside the autocorrelation's support.
        int p = std::min (lpcOrder_, (len - 2) / 2);
        if (beta > 1.0)
            p = std::min (p, (int) ((double) (len - 2) / beta));

        const int64_t s0 = (int64_t) analysisEpoch - half;    // first grain sample
        const int64_t h0 = s0 - p;                             // first history sample
        const int64_t oldest = (int64_t) write_ - (int64_t) (mask_ + 1);

        // Degenerate geometry, stream edges, or history outside the ring:
        // place the grain as a plain 1:1 copy (the preserve geometry), which
        // is the right sound for the edge of a voiced span anyway.
        bool canModel = p >= 4 && h0 >= 0 && h0 > oldest
                        && (uint64_t) (s0 + len) <= write_
                        && (size_t) len <= lpcE_.size();

        double E = 0.0, Ew = 0.0;
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

                // THE WARP HAPPENS IN THE ENVELOPE DOMAIN, deliberately.
                //
                // The first cut interpolated the raw autocorrelation at
                // scaled lags (r(beta*k), the time-compression identity).
                // Measured, that collapses at some warp factors: r(tau) of
                // anything with near-Nyquist content oscillates at the
                // sub-sample scale, interpolating it yields an INDEFINITE
                // sequence, and Levinson degenerates through its reflection
                // clamps - Ew came out 2.5e-16 against a healthy 1.9e-3, and
                // the grain silently fell back to unwarped. The envelope
                // itself is smooth by construction (the lag window above
                // floors every peak's bandwidth), so THAT is the safe thing
                // to resample:
                //
                //   1. evaluate the model envelope P(w) = E/|A(w)|^2 on a
                //      dense grid (one complex rotation per coefficient),
                //   2. read it at w/beta - linear interpolation of a smooth
                //      curve; beyond the source band, hold the Nyquist value,
                //   3. cosine-transform back to an autocorrelation - which is
                //      positive definite BY CONSTRUCTION, being the transform
                //      of a nonnegative spectrum - and Levinson that.
                //
                // Grid spacing pi/512 is ~47 Hz at 48 kHz, matched to the
                // ~40 Hz bandwidth floor the lag window guarantees, so no
                // peak can fall between grid points.
                const int M = kWarpGrid;
                for (int m = 0; m <= M; ++m)
                {
                    const double wm = 3.141592653589793 * (double) m / (double) M;
                    // A(e^{-jw}) = 1 - sum a_k e^{-jwk}, via phase rotation.
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

                // RELATIVE degeneracy guard, not absolute: a very small E on
                // clean periodic material means the model is very GOOD, and
                // the gain below carries the scale. Only an actual collapse
                // drops the grain to the plain-copy path.
                if (E <= 1.0e-9 * lpcR_[0] || Ew <= 1.0e-9 * lpcRw_[0] || lpcRw_[0] <= 0.0)
                    canModel = false;
            }
        }

        const float makeup = std::clamp ((float) Ta / (float) Ts, 1.0f, 2.0f);

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

            // Re-apply, warped. The residual is scaled so output energy lands
            // at the grain's own: variance E in, filtered through 1/A' which
            // amplifies white variance by rw[0]/Ew, must land at r[0] - so
            // g^2 = (Ew/E) * (r[0]/rw[0]). The filter starts from the dry
            // history.
            const double g = std::clamp (
                std::sqrt ((Ew / E) * (lpcR_[0] / std::max (1.0e-12, lpcRw_[0]))),
                0.0625, 16.0);
            for (int i = 0; i < p; ++i)
                lpcY_[(size_t) i] = lpcX_[(size_t) i];
            for (int n = 0; n < len; ++n)
            {
                double y = g * (double) lpcE_[(size_t) n];
                for (int k = 1; k <= p; ++k)
                    y += lpcAw_[(size_t) k] * (double) lpcY_[(size_t) (p + n - k)];
                // A pathological frame must degrade, not explode: the warp can
                // push a near-unit-circle pole harder than the source ever did.
                y = std::clamp (y, -4.0, 4.0);
                lpcY_[(size_t) (p + n)] = (float) y;
            }
        }
        else
        {
            // Plain 1:1 copy - the preserve geometry without the model.
            for (int n = 0; n < len; ++n)
            {
                const int64_t sp = s0 + n;
                lpcY_[(size_t) (p + n)] = sp >= 0 && (uint64_t) sp < write_
                    ? in_[(size_t) ((uint32_t) (uint64_t) sp & mask_)] : 0.0f;
            }
        }

        for (int k = -half; k <= half; ++k)
        {
            const int64_t dst = (int64_t) synthEpoch + k;
            if (dst < emitFloor) continue;

            const float ph = (float) (k + half) / (float) (len - 1);
            const float w  = 0.5f - 0.5f * std::cos (6.283185307179586f * ph);

            const uint32_t di = (uint32_t) (uint64_t) dst & mask_;
            acc_[(size_t) di] += makeup * w * lpcY_[(size_t) (p + k + half)];
            win_[(size_t) di] += w;
        }
        if (debugOn_) dbgGrain_.push_back ({ synthEpoch, Ta, Ts, half, makeup });
        placedTo_ = std::max (placedTo_, synthEpoch + (uint64_t) half);
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
    std::vector<uint64_t> dbgReseed_, dbgReset_;
    std::vector<DebugGrain> dbgGrain_;
    std::vector<DebugEmit>  dbgEmit_;
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

    uint64_t write_   = 0;    // absolute input samples written
    int64_t  emitted_ = 0;    // input position just past the last sample emitted
    uint64_t placedTo_ = 0;

    uint64_t lastEpoch_ = 0;
    bool     haveEpoch_ = false;
    uint64_t nextSynth_ = 0;
    bool     haveSynth_ = false;

    std::atomic<float> targetHz_ { 0.0f };
    std::atomic<int>   formantMode_ { kFormantPreserve };
    std::atomic<float> formantShift_ { 0.0f };
    std::atomic<float> mix_     { 1.0f };
    std::atomic<float> outGain_ { 1.0f };
    std::atomic<float> outDb_   { 0.0f };
};

} // namespace echojay
