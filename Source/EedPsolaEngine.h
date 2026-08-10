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

    // Fade applied on the VOICED side of a voiced/unvoiced seam. Short enough
    // to be inaudible as a level move, long enough to stop a step edge.
    static constexpr float kSeamFadeMs = 1.5f;

    // Safety ceiling on the overlap-add window sum: only ever attenuates, and
    // only once the accumulated window is far past what any sane ratio
    // produces. Never boosts, so it cannot resurrect a thin patch as noise.
    static constexpr float kWindowCeil = 0.5f;

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
        const int need = maxLatency_ + 4 * maxPeriod_ + std::max (64, maxBlockSize) + 64;
        int bits = 1;
        while ((1 << bits) < need) ++bits;
        mask_ = (uint32_t) ((1 << bits) - 1);
        const size_t sz = (size_t) (1 << bits);

        in_.assign  (sz, 0.0f);
        f0_.assign  (sz, 0.0f);      // <= 0 means UNVOICED at that sample
        acc_.assign (sz, 0.0f);
        win_.assign (sz, 0.0f);

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
        // One grain of lookahead either side, plus a period of epoch search.
        latency_    = kGrainPeriods * curPeriod_ + curPeriod_;
        maxLatency_ = std::max (maxLatency_, kGrainPeriods * maxPeriod_ + maxPeriod_);
    }

    int latencySamples() const noexcept { return latency_; }

    // ---- parameters (message thread) --------------------------------------
    void  setTargetHz (float hz) noexcept
    {
        targetHz_.store (hz <= 0.0f ? 0.0f
                                    : std::clamp (hz, kMinTargetHz, kMaxTargetHz),
                         std::memory_order_relaxed);
    }
    float getTargetHz() const noexcept { return targetHz_.load (std::memory_order_relaxed); }

    // ---- audio thread ------------------------------------------------------
    // Push n input samples with the detector's CURRENT reading, and pull the n
    // output samples that are `latencySamples()` behind them. f0 <= 0 or
    // voiced == false marks the span unvoiced.
    void process (const float* in, float* out, int n, float f0Hz, bool voiced) noexcept
    {
        const float track = (voiced && f0Hz > 0.0f) ? f0Hz : 0.0f;

        for (int i = 0; i < n; ++i)
        {
            in_[(size_t) ((uint32_t) write_ & mask_)] = in[i];
            f0_[(size_t) ((uint32_t) write_ & mask_)] = track;
            ++write_;
        }

        const float target = targetHz_.load (std::memory_order_relaxed);

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
        advanceSynthesis (base + (int64_t) n + (int64_t) curPeriod_ * kGrainPeriods, base);
        emitMixed (out, n, base);
    }

private:
    // ---- output ------------------------------------------------------------
    void emitDry (float* out, int n, int64_t base) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = base + (int64_t) i;
            if (p < 0) { out[i] = 0.0f; continue; }      // latency warm-up
            const uint32_t idx = (uint32_t) (uint64_t) p & mask_;
            out[i] = in_[(size_t) idx];
            // Clear the accumulators as we pass, so a later switch back to
            // shifting does not read stale grain content.
            acc_[(size_t) idx] = 0.0f;
            win_[(size_t) idx] = 0.0f;
        }
        emitted_ = base + (int64_t) n;
    }

    void emitMixed (float* out, int n, int64_t base) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = base + (int64_t) i;
            if (p < 0) { out[i] = 0.0f; continue; }      // latency warm-up
            const uint32_t idx = (uint32_t) (uint64_t) p & mask_;

            const float dry = in_[(size_t) idx];

            // NEVER fall back to dry inside a voiced span. Where the window
            // sum is thin - which happens at large downshifts, when grains are
            // re-spaced further apart than they are long - substituting the dry
            // signal re-injects the SOURCE pitch, and the output then measures
            // as unshifted. Flooring the divisor instead lets the thin patch
            // come out quiet, which is what a truncated vocal-tract ring
            // actually sounds like, rather than wrong.
            // No per-sample normalisation - see placeGrain. win_ is still
            // accumulated and used only as a SAFETY ceiling, so a pathological
            // pile-up of grains cannot run away; it never boosts.
            const float w   = win_[(size_t) idx];
            const float wet = acc_[(size_t) idx] / std::max (1.0f, w * kWindowCeil);

            // UNVOICED IS SACRED: the dry sample, untouched. Voiced samples
            // near the seam fade between wet and dry so the join is smooth,
            // and that fade lives entirely on the voiced side.
            const float g = seamGain ((uint64_t) p);
            out[i] = g <= 0.0f ? dry : (g >= 1.0f ? wet : dry + g * (wet - dry));

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
    void advanceSynthesis (int64_t upToSigned, int64_t base) noexcept
    {
        if (upToSigned < 0) return;
        const uint64_t upTo = (uint64_t) upToSigned;

        // Never read input that has not arrived, and leave two periods of
        // margin so epoch search never runs off the end. The margin uses the
        // ACTIVE period, not the worst case: latency_ is 3 active periods, so
        // a worst-case margin would sit BEHIND the emit point and synthesis
        // could never catch up.
        const uint64_t safeLimit = write_ > (uint64_t) (2 * curPeriod_)
                                 ? write_ - (uint64_t) (2 * curPeriod_) : 0;

        if (! haveSynth_ || nextSynth_ + (uint64_t) (4 * curPeriod_) < (uint64_t) std::max<int64_t> (base, 0))
        {
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
            const float target = targetHz_.load (std::memory_order_relaxed);
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
        const int half = std::min (Ta, maxPeriod_);
        const int len  = 2 * half + 1;

        // Level is an ENERGY problem, not an amplitude one, and getting that
        // wrong is worth 3 dB. The output is a pulse train: each grain carries
        // gain^2 * E of energy and they arrive every Ts, so output power goes
        // as gain^2 * E / Ts against the source's E / Ta. Unity therefore wants
        //
        //     gain = sqrt (Ts / Ta)
        //
        // not Ts/Ta. The amplitude-shaped correction measured -6 dB on a 2x
        // upshift and -2.9 dB at 1.5x; this lands both inside 3 dB.
        //
        // It is deliberately an OPEN-LOOP average correction rather than a
        // per-sample window-sum division. Dividing instantaneously AVERAGES
        // overlapping copies of the same pulse instead of adding them, which
        // smears the pulse train - and the pulse train is the signal.
        const float gain = std::clamp (std::sqrt ((float) Ts / (float) Ta), 0.25f, 4.0f);

        for (int k = -half; k <= half; ++k)
        {
            const uint64_t src = analysisEpoch + (uint64_t) (int64_t) k;
            const int64_t  dst = (int64_t) synthEpoch + k;
            if (dst < emitFloor) continue;                 // already emitted
            if (src >= write_) continue;

            // Hann across the grain.
            const float ph = (float) (k + half) / (float) (len - 1);
            const float w  = 0.5f - 0.5f * std::cos (6.283185307179586f * ph);

            const uint32_t di = (uint32_t) (uint64_t) dst & mask_;
            acc_[(size_t) di] += gain * w * in_[(size_t) ((uint32_t) src & mask_)];
            win_[(size_t) di] += w;
        }
        placedTo_ = std::max (placedTo_, synthEpoch + (uint64_t) half);
    }

    // ---- state -------------------------------------------------------------
    double fs_ = 48000.0;
    int    maxPeriod_  = 2048;
    int    curPeriod_  = 600;
    int    latency_    = 1800;
    int    maxLatency_ = 6144;
    int    seamFade_   = 72;

    std::vector<float> in_, f0_, acc_, win_;
    uint32_t mask_ = 0;

    uint64_t write_   = 0;    // absolute input samples written
    int64_t  emitted_ = 0;    // input position just past the last sample emitted
    uint64_t placedTo_ = 0;

    uint64_t lastEpoch_ = 0;
    bool     haveEpoch_ = false;
    uint64_t nextSynth_ = 0;
    bool     haveSynth_ = false;

    std::atomic<float> targetHz_ { 0.0f };
};

} // namespace echojay
