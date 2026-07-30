/*
    EedReverbEngine.h  —  the DSP behind "EchoJay Reverb".

    A feedback delay network, built entirely out of EedDelayCore.h. That is the
    whole reason the core exists: a reverb IS a network of delays and allpasses,
    so once the primitive is written and tested, the hardest device in the suite
    is an arrangement problem rather than a DSP-from-scratch problem.

    JUCE-free, with a g++ test (test/reverb_engine_test.cpp).

    THE SIGNAL PATH

        in L/R -> predelay line (one per channel)
                    |
                    +--> EARLY: 7 fixed taps per channel, read straight out of
                    |           the predelay line at predelay + tap. Distinct
                    |           tap times and sign patterns per side, so the two
                    |           channels are decorrelated from the first
                    |           millisecond rather than only once the tail
                    |           builds.
                    |
                    +--> LATE:  4 Schroeder allpasses per channel (diffusion)
                                -> 8-line FDN with a Hadamard feedback matrix
                                -> damping LP + low-cut HP inside every line
        wet = early/late blend -> M/S width -> dry/wet mix

    WHY AN FDN AND NOT A FREEVERB. Freeverb is 8 parallel combs into 4 series
    allpasses per channel: cheap, and it rings, because parallel combs only
    ever mix at the output. An FDN mixes every line into every other line on
    every pass through the matrix, so echo density grows exponentially instead
    of linearly. That density is the difference between "a room" and "a metallic
    stack of delays".

    THE FOUR THINGS THAT MAKE OR BREAK IT

    * THE MATRIX MUST BE ORTHOGONAL. Feedback stability of an FDN is exactly
      "max per-line gain < 1", but ONLY if the mixing matrix preserves energy.
      Hadamard does, and its fast form is 24 adds — no multiplies at all beyond
      one normalisation. Pick a non-orthogonal matrix and the thing can diverge
      at settings that measured fine.

    * DECAY IS PER LINE, NOT GLOBAL. A line of length L must lose 60 dB in the
      requested RT60, so g = 10^(-3L/T60). A SHORT line recirculates more often
      per second, so it needs a HIGHER per-pass gain to take the same time to
      die. Give every line the same gain instead and the short ones fade out
      first, leaving the tail to the few longest lines — the density collapses
      partway through the decay and what is left is audible discrete echoes.

    * THE LINES MUST BE MUTUALLY INCOMMENSURATE. Lengths sharing a common factor
      put their echoes on top of each other and the density collapses into a
      pitch. The base lengths below are deliberately non-harmonic.

    * MODULATION IS NOT OPTIONAL. A static FDN has fixed modes, and a fixed mode
      is a resonance you can hear as a ringing note. A few samples of slow,
      per-line-decorrelated wobble smears the modes into a band. That is why
      mod_depth defaults to on rather than to zero.

    LATENCY: none. Predelay is a delay, not lookahead — the dry signal leaves
    immediately. Nothing here needs host compensation.

    Real-time contract: process() never allocates, locks or blocks. Parameters
    are plain atomics; the ones that need derived state (line lengths, decay
    gains, filter coefficients) are recomputed at block rate behind a dirty flag,
    never per sample.
*/

#pragma once

#include "EedDelayCore.h"

#include <atomic>
#include <cmath>

namespace echojay
{

// ---------------------------------------------------------------------------
// THE ALGORITHM — the marquee control (DEVICE_DEPTH_PLAN.md, Time).
//
// One FDN, five spaces. What makes them different is NOT five networks: it is
// the handful of numbers that decide what a network sounds like, and an FDN's
// character lives almost entirely in four of them —
//
//   * how LONG the lines are (the geometry of the space),
//   * how far apart the EARLY taps are spread, and how loud they are at all,
//   * how much DIFFUSION smears the input before it reaches the network,
//   * how bright the tail stays, and how much it MOVES.
//
// So an algorithm is a spec, applied on top of whatever the user dialled, and
// the dialled values keep meaning what they say. `decay_s` is still the RT60 in
// all five, which is the property that makes them comparable: switching from
// hall to plate at 3 seconds gives two different 3-second reverbs rather than
// silently changing the length as well as the character.
//
// HALL IS THE NEUTRAL ONE, and that is why it is the default. It is not a tuning
// of the network — it IS the network, exactly as it shipped before algorithms
// existed: every multiplier below is 1 and every bias is 0. So a saved session
// written before this param existed restores byte-for-byte the reverb it was
// mixed with, and the other four are departures from a known reference rather
// than five equally arbitrary presets. (test/reverb_engine_test.cpp measures
// that neutrality, so it cannot be broken by a later edit to the table.)
//
//   room     short dense lines, a tight early cluster, plenty of it. A real
//            small space: you hear the walls.
//   hall     long lines, a wide early spread, smooth. Big and slow. THE REFERENCE.
//   plate    NO distinct early reflections (a plate has no walls to reflect off,
//            just a sheet of steel), very diffuse, bright. The classic.
//   spring   short, resonant, and DISPERSIVE — highs arrive later than lows,
//            which is the "boing". Modulated hard, because a spring tank is
//            physically never still.
//   ambience very short, mostly early, barely any tail. Space without reverb.
// ---------------------------------------------------------------------------
enum class ReverbAlgorithm
{
    Room     = 0,
    Hall     = 1,
    Plate    = 2,
    Spring   = 3,
    Ambience = 4
};

constexpr int kNumReverbAlgorithms = 5;

struct ReverbAlgorithmSpec
{
    float lineScale;      // multiplies the FDN's line lengths: the size of the space
    float tapScale;       // multiplies the early-reflection tap times: their spread
    float earlyGain;      // scale on the early cluster; 0 means "no walls" (plate)
    float earlyLateBias;  // pushed onto the dialled early/late balance, then clamped
    float diffusionBias;  // pushed onto the dialled diffusion, then clamped
    float dampScale;      // multiplies the damping corner: brightness of the tail
    float modScale;       // multiplies the modulation depth: how much it moves
    float dispersion;     // per-line allpass gain; > 0 is a spring's frequency smear
};

inline ReverbAlgorithmSpec reverbAlgorithmSpec (ReverbAlgorithm a) noexcept
{
    switch (a)
    {
        //                  line   tap   early   el      diff    damp   mod   disp
        case ReverbAlgorithm::Room:
            return          { 0.55f, 0.62f, 1.15f, -0.28f,  0.00f, 1.05f, 0.9f, 0.0f };

        // earlyGain 0 is the definition of a plate rather than a taste: there are
        // no early reflections at all, only the diffuse sheet. The early/late bias
        // pushes the blend to the late side so a user who leaves early/late where
        // it was still hears a plate rather than a gap.
        case ReverbAlgorithm::Plate:
            return          { 0.68f, 1.00f, 0.00f,  0.30f,  0.25f, 1.55f, 1.3f, 0.0f };

        // Dispersion is what a spring IS. The extra per-line allpass makes the
        // loop's delay frequency-dependent, so every pass smears the highs a
        // little further behind the lows and the tail chirps.
        case ReverbAlgorithm::Spring:
            return          { 0.38f, 0.50f, 0.45f,  0.15f, -0.18f, 0.85f, 2.6f, 0.62f };

        case ReverbAlgorithm::Ambience:
            return          { 0.26f, 0.60f, 1.25f, -0.55f, -0.12f, 1.20f, 0.6f, 0.0f };

        // THE REFERENCE: all ones and zeros, which is the network as it shipped.
        case ReverbAlgorithm::Hall:
        default:
            return          { 1.00f, 1.00f, 1.00f,  0.00f,  0.00f, 1.00f, 1.0f, 0.0f };
    }
}

inline const char* reverbAlgorithmName (ReverbAlgorithm a) noexcept
{
    switch (a)
    {
        case ReverbAlgorithm::Hall:     return "hall";
        case ReverbAlgorithm::Plate:    return "plate";
        case ReverbAlgorithm::Spring:   return "spring";
        case ReverbAlgorithm::Ambience: return "ambience";
        case ReverbAlgorithm::Room:
        default:                        return "room";
    }
}

inline ReverbAlgorithm reverbAlgorithmFromIndex (int i) noexcept
{
    if (i <= 0) return ReverbAlgorithm::Room;
    if (i >= kNumReverbAlgorithms - 1) return ReverbAlgorithm::Ambience;
    return (ReverbAlgorithm) i;
}

// The diffusion percentage at which the network's own allpass gains are used
// EXACTLY as designed. Below it the tail thins, above it thickens — and because
// it is also the schema's default, a session saved before `diffusion` existed
// restores the diffusion it was mixed with rather than a new one.
constexpr float kReverbDiffusionUnityPct = 75.0f;

class ReverbEngine
{
public:
    // ---- advertised ranges (the SAME numbers the ParamSchema publishes) -----
    static constexpr float kMinSizePct     = 0.0f;
    static constexpr float kMaxSizePct     = 100.0f;
    static constexpr float kMinDecaySec    = 0.1f;
    static constexpr float kMaxDecaySec    = 20.0f;
    static constexpr float kMinPredelayMs  = 0.0f;
    static constexpr float kMaxPredelayMs  = 200.0f;
    static constexpr float kMinDampingPct  = 0.0f;
    static constexpr float kMaxDampingPct  = 100.0f;
    static constexpr float kMinLowCutHz    = 20.0f;
    static constexpr float kMaxLowCutHz    = 1000.0f;
    static constexpr float kMinWidthPct    = 0.0f;
    static constexpr float kMaxWidthPct    = 100.0f;
    static constexpr float kMinEarlyLatePct= 0.0f;
    static constexpr float kMaxEarlyLatePct= 100.0f;
    static constexpr float kMinModDepthPct = 0.0f;
    static constexpr float kMaxModDepthPct = 100.0f;
    static constexpr float kMinMixPct      = 0.0f;
    static constexpr float kMaxMixPct      = 100.0f;
    static constexpr float kMinDuckPct     = 0.0f;
    static constexpr float kMaxDuckPct     = 100.0f;
    static constexpr float kMinDiffusionPct= 0.0f;
    static constexpr float kMaxDiffusionPct= 100.0f;

    static constexpr int kNumLines = 8;
    static constexpr int kNumTaps  = 7;    // early reflections, per channel
    static constexpr int kNumDiff  = 4;    // diffusion allpasses, per channel

    ReverbEngine() = default;

    // ---- lifecycle ---------------------------------------------------------
    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

        // The predelay line also serves the early reflections, so it has to hold
        // the longest predelay PLUS the latest tap.
        const double preSeconds = (double) kMaxPredelayMs / 1000.0
                                + kTapMsL[kNumTaps - 1] / 1000.0 + 0.02;
        predelayL_.prepare (sampleRate_, preSeconds);
        predelayR_.prepare (sampleRate_, preSeconds);

        for (int i = 0; i < kNumLines; ++i)
        {
            // Full length plus modulation headroom; `size` and the algorithm's
            // line scale only ever shorten.
            lines_[i].prepare (sampleRate_, kLineMs[i] / 1000.0 + 0.01);
            lineDelay_[i].prepare (sampleRate_, 0.100);   // size changes GLIDE
            lp_[i].reset();
            hp_[i].reset();

            // The spring's dispersion allpass, one per line, inside the loop so
            // the smear ACCUMULATES per pass — which is what makes a spring chirp
            // rather than merely sound filtered. Sized for the longest of the
            // fixed lengths below; only `spring` ever switches it on.
            disp_[i].prepare (sampleRate_, 0.010);
            disp_[i].setDelaySamples ((int) (kDispMs[i] * sampleRate_ * 0.001));
        }

        for (int i = 0; i < kNumDiff; ++i)
        {
            diffL_[i].prepare (sampleRate_, 0.030);
            diffR_[i].prepare (sampleRate_, 0.030);
            diffL_[i].setDelaySamples ((int) (kDiffMsL[i] * sampleRate_ * 0.001));
            diffR_[i].setDelaySamples ((int) (kDiffMsR[i] * sampleRate_ * 0.001));
            // The gains are set by recompute() now, from diffusion + the algorithm.
        }

        ducker_.prepare (sampleRate_);
        duckSmooth_.prepare (sampleRate_, 0.005);

        predelaySmooth_.prepare (sampleRate_, 0.050);
        mixSmooth_.prepare (sampleRate_, 0.020);
        widthSmooth_.prepare (sampleRate_, 0.020);
        elSmooth_.prepare (sampleRate_, 0.020);

        recompute();
        reset();
    }

    // Empty the network and snap every smoother. A restored session then starts
    // AT its settings instead of sweeping into them, and a transport stop does
    // not leave the previous take's tail sitting in the lines.
    void reset() noexcept
    {
        predelayL_.reset();
        predelayR_.reset();

        for (int i = 0; i < kNumLines; ++i)
        {
            lines_[i].reset();
            lp_[i].reset();
            hp_[i].reset();
            lineDelay_[i].snap ((float) targetLineSamples_[i]);

            // Per-line modulation phases spread evenly around the circle, so no
            // two lines wobble together (which would just detune the whole tail
            // in unison and change nothing about the modes).
            modPhase_[i] = (double) i / (double) kNumLines;
        }

        for (int i = 0; i < kNumLines; ++i) disp_[i].reset();
        for (int i = 0; i < kNumDiff; ++i) { diffL_[i].reset(); diffR_[i].reset(); }

        ducker_.reset();
        duckSmooth_.snap (1.0f);

        predelaySmooth_.snap ((float) (predelayMs_.load (std::memory_order_relaxed)
                                       * sampleRate_ * 0.001));
        mixSmooth_.snap (mixPct_.load (std::memory_order_relaxed) * 0.01f);
        widthSmooth_.snap (widthPct_.load (std::memory_order_relaxed) * 0.01f);
        elSmooth_.snap (earlyLatePct_.load (std::memory_order_relaxed) * 0.01f);
    }

    // ---- parameters (message thread) ---------------------------------------
    // The four that change derived state raise a dirty flag rather than
    // recomputing here: recompute() touches 8 filters and 8 gains, and doing
    // that from the message thread while the audio thread is mid-block is a
    // race for no benefit.
    void setSizePct (float p) noexcept
    {
        sizePct_.store (clampf (p, kMinSizePct, kMaxSizePct), std::memory_order_relaxed);
        dirty_.store (true, std::memory_order_relaxed);
    }

    void setDecaySeconds (float s) noexcept
    {
        decaySec_.store (clampf (s, kMinDecaySec, kMaxDecaySec), std::memory_order_relaxed);
        dirty_.store (true, std::memory_order_relaxed);
    }

    void setDampingPct (float p) noexcept
    {
        dampingPct_.store (clampf (p, kMinDampingPct, kMaxDampingPct), std::memory_order_relaxed);
        dirty_.store (true, std::memory_order_relaxed);
    }

    void setLowCutHz (float hz) noexcept
    {
        lowCutHz_.store (clampf (hz, kMinLowCutHz, kMaxLowCutHz), std::memory_order_relaxed);
        dirty_.store (true, std::memory_order_relaxed);
    }

    // THE ALGORITHM. Everything it changes is derived state (line lengths, tap
    // times, diffusion gains, damping corners, dispersion), so it raises the same
    // dirty flag the other four do rather than touching 8 filters from the
    // message thread while the audio thread is mid-block.
    void setAlgorithm (ReverbAlgorithm a) noexcept
    {
        algorithm_.store ((int) a, std::memory_order_relaxed);
        dirty_.store (true, std::memory_order_relaxed);
    }

    ReverbAlgorithm getAlgorithm() const noexcept
    {
        return reverbAlgorithmFromIndex (algorithm_.load (std::memory_order_relaxed));
    }

    void setDiffusionPct (float p) noexcept
    {
        diffusionPct_.store (clampf (p, kMinDiffusionPct, kMaxDiffusionPct),
                             std::memory_order_relaxed);
        dirty_.store (true, std::memory_order_relaxed);
    }

    float getDiffusionPct() const noexcept
    {
        return diffusionPct_.load (std::memory_order_relaxed);
    }

    // Duck needs no dirty flag: the Ducker recomputes nothing, it just follows.
    void setDuckPct (float p) noexcept
    {
        duckPct_.store (clampf (p, kMinDuckPct, kMaxDuckPct), std::memory_order_relaxed);
    }

    float getDuckPct() const noexcept { return duckPct_.load (std::memory_order_relaxed); }

    // The gain the ducker is currently applying to the wet path, linear — for a
    // readout, on the same benign racy single-float contract as the rest of the
    // suite's meters.
    float duckWetGain() const noexcept { return ducker_.wetGain(); }

    void setPredelayMs (float ms) noexcept    { predelayMs_.store (clampf (ms, kMinPredelayMs, kMaxPredelayMs), std::memory_order_relaxed); }
    void setWidthPct (float p) noexcept       { widthPct_.store (clampf (p, kMinWidthPct, kMaxWidthPct), std::memory_order_relaxed); }
    void setEarlyLatePct (float p) noexcept   { earlyLatePct_.store (clampf (p, kMinEarlyLatePct, kMaxEarlyLatePct), std::memory_order_relaxed); }
    void setModDepthPct (float p) noexcept    { modDepthPct_.store (clampf (p, kMinModDepthPct, kMaxModDepthPct), std::memory_order_relaxed); }
    void setMixPct (float p) noexcept         { mixPct_.store (clampf (p, kMinMixPct, kMaxMixPct), std::memory_order_relaxed); }

    float getSizePct()       const noexcept { return sizePct_.load (std::memory_order_relaxed); }
    float getDecaySeconds()  const noexcept { return decaySec_.load (std::memory_order_relaxed); }
    float getPredelayMs()    const noexcept { return predelayMs_.load (std::memory_order_relaxed); }
    float getDampingPct()    const noexcept { return dampingPct_.load (std::memory_order_relaxed); }
    float getLowCutHz()      const noexcept { return lowCutHz_.load (std::memory_order_relaxed); }
    float getWidthPct()      const noexcept { return widthPct_.load (std::memory_order_relaxed); }
    float getEarlyLatePct()  const noexcept { return earlyLatePct_.load (std::memory_order_relaxed); }
    float getModDepthPct()   const noexcept { return modDepthPct_.load (std::memory_order_relaxed); }
    float getMixPct()        const noexcept { return mixPct_.load (std::memory_order_relaxed); }

    // The damping filter sits INSIDE the loop, so it shortens the decay it is
    // filtering. This is what the tail actually does, as opposed to what the
    // decay knob asked for, and it is exposed so the editor can be honest about
    // it rather than showing a number the ear disagrees with.
    double effectiveDecaySeconds() const noexcept
    {
        const double asked = (double) decaySec_.load (std::memory_order_relaxed);
        const double damp  = (double) dampingPct_.load (std::memory_order_relaxed) * 0.01;
        return asked * (1.0 - 0.45 * damp);
    }

    // Room size in metres-ish, for the readout. The longest line at full size is
    // ~69 ms, which is a round trip of ~23 m of air.
    //
    // Scaled by the ALGORITHM's line scale, because that is what actually sets the
    // geometry: an `ambience` at size 100 is a small bright space and a `hall` at
    // size 100 is a concert hall, and a readout that gave both the same number
    // would be describing the dial rather than the room.
    double roomSizeMetres() const noexcept
    {
        const double base = 3.0 + 20.0 * (double) sizePct_.load (std::memory_order_relaxed) * 0.01;
        return base * (double) reverbAlgorithmSpec (getAlgorithm()).lineScale;
    }

    // ---- audio thread ------------------------------------------------------
    // In-place stereo. `right` may be null: the network then runs from the one
    // channel and only the left output is written.
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (left == nullptr || numSamples <= 0) return;

        if (dirty_.exchange (false, std::memory_order_relaxed))
            recompute();

        const bool stereo = (right != nullptr);

        // Read every parameter ONCE per block.
        predelaySmooth_.setTarget ((float) (predelayMs_.load (std::memory_order_relaxed)
                                            * sampleRate_ * 0.001));
        mixSmooth_.setTarget (mixPct_.load (std::memory_order_relaxed) * 0.01f);
        widthSmooth_.setTarget (widthPct_.load (std::memory_order_relaxed) * 0.01f);
        elSmooth_.setTarget (earlyLatePct_.load (std::memory_order_relaxed) * 0.01f);

        for (int i = 0; i < kNumLines; ++i)
            lineDelay_[i].setTarget ((float) targetLineSamples_[i]);

        // Modulation depth in samples. Small on purpose: this is here to smear
        // the network's modes, not to detune the room — except in `spring`, whose
        // modScale is deliberately large because a spring tank is never still.
        const float modSamples = modDepthPct_.load (std::memory_order_relaxed)
                               * 0.01f * kMaxModSamples * modScale_;

        ducker_.setAmountPct (duckPct_.load (std::memory_order_relaxed));

        // Read once per block, like everything else: the algorithm's contribution
        // to the early/late balance and the early cluster's own level.
        const float earlyGain = earlyGain_;
        const float elBias    = elBias_;
        const bool  dispersing = dispersing_;

        for (int n = 0; n < numSamples; ++n)
        {
            const float inL = left[n];
            const float inR = stereo ? right[n] : inL;

            const float mix   = mixSmooth_.next();
            const float width = widthSmooth_.next();
            const float pre   = predelaySmooth_.next();

            // The algorithm's bias, applied to the dialled balance and clamped.
            // Biased rather than overwritten: `plate` leans late and `ambience`
            // leans early, and the dial still moves the whole thing from there.
            const float el = std::clamp (elSmooth_.next() + elBias, 0.0f, 1.0f);

            // The DUCKER's sidechain is the DRY input, taken here before anything
            // is done to it. Smoothed by a short ramp so a per-sample gain change
            // on a wide reverb tail cannot buzz.
            const float duck = duckSmooth_.next();
            duckSmooth_.setTarget (ducker_.process (inL, inR));

            // ---- predelay (one line, also the early-reflection source) -----
            predelayL_.push (inL);
            predelayR_.push (inR);

            const double preD = (double) pre;
            const float preL = predelayL_.readCubic (preD);
            const float preR = predelayR_.readCubic (preD);

            // ---- early reflections ------------------------------------------
            float erL = 0.0f, erR = 0.0f;
            for (int k = 0; k < kNumTaps; ++k)
            {
                erL += kTapGain[k] * kTapSignL[k] * predelayL_.readCubic (preD + tapSamplesL_[k]);
                erR += kTapGain[k] * kTapSignR[k] * predelayR_.readCubic (preD + tapSamplesR_[k]);
            }
            // The algorithm's early level. `plate` sets this to zero, which is not
            // a taste but the definition: a plate has no walls to reflect off.
            erL *= kEarlyScale * earlyGain;
            erR *= kEarlyScale * earlyGain;

            // ---- diffusion ---------------------------------------------------
            float dL = preL, dR = preR;
            for (int k = 0; k < kNumDiff; ++k)
            {
                dL = diffL_[k].process (dL);
                dR = diffR_[k].process (dR);
            }

            // ---- FDN ---------------------------------------------------------
            float y[kNumLines];
            for (int i = 0; i < kNumLines; ++i)
            {
                modPhase_[i] += modInc_[i];
                if (modPhase_[i] >= 1.0) modPhase_[i] -= 1.0;

                const double d = (double) lineDelay_[i].next()
                               + (double) (modSamples * fastSine (modPhase_[i]));
                y[i] = lines_[i].readCubic (d);
            }

            // The output tap is the RAW line outputs, taken before the loop's
            // gain and filtering: L from the even lines, R from the odd ones, so
            // the two sides are built from different line lengths and are
            // decorrelated by construction rather than by a width control.
            const float lateL = (y[0] - y[2] + y[4] - y[6]) * kLateScale;
            const float lateR = (y[1] - y[3] + y[5] - y[7]) * kLateScale;

            // Feedback: per-line decay gain, damping, low cut, the spring's
            // dispersion, then the matrix.
            float f[kNumLines];
            for (int i = 0; i < kNumLines; ++i)
            {
                float v = lineGain_[i] * hp_[i].process (lp_[i].process (y[i]));

                // SKIPPED, not run at zero gain: a Schroeder allpass with g = 0 is
                // still a pure M-sample delay, so leaving it in the loop would
                // lengthen every line and quietly change the tuning of the four
                // algorithms that do not want it.
                if (dispersing) v = disp_[i].process (v);

                f[i] = v;
            }

            hadamard8 (f);

            for (int i = 0; i < kNumLines; ++i)
                lines_[i].push (flushDenorm (f[i] + kInjectScale * kInjectSign[i]
                                                  * ((i & 1) ? dR : dL)));

            // ---- early / late blend, width, mix -----------------------------
            float wetL = erL * (1.0f - el) + lateL * el;
            float wetR = erR * (1.0f - el) + lateR * el;

            // M/S: width 0 collapses the tail to mono (useful for a mono-safe
            // mix), width 100 leaves the network's own stereo alone. It never
            // widens PAST what the network produced, because a tail wider than
            // the room is where reverbs start sounding phasey.
            const float m = 0.5f * (wetL + wetR);
            const float s = 0.5f * (wetL - wetR) * width;
            wetL = m + s;
            wetR = m - s;

            // Ducking is applied to the WET ONLY, after the network and before the
            // mix. On the dry side it would be a compressor; on the network's
            // input it would shorten the tail instead of lowering it, and the
            // swell in the gaps — the entire point — would never happen.
            wetL *= duck;
            wetR *= duck;

            left[n] = inL * (1.0f - mix) + wetL * mix;
            if (stereo) right[n] = inR * (1.0f - mix) + wetR * mix;
        }
    }

    // ---- pure helpers (what the g++ test pins) ------------------------------
    // Fast in-place 8-point Hadamard, normalised to be orthogonal. Three
    // butterfly stages: 24 add/sub and 8 multiplies, no matrix at all.
    static void hadamard8 (float* v) noexcept
    {
        for (int stride = 1; stride < kNumLines; stride <<= 1)
            for (int i = 0; i < kNumLines; i += stride * 2)
                for (int j = 0; j < stride; ++j)
                {
                    const float a = v[i + j];
                    const float b = v[i + j + stride];
                    v[i + j]          = a + b;
                    v[i + j + stride] = a - b;
                }

        constexpr float norm = 0.35355339059327376f;   // 1/sqrt(8)
        for (int i = 0; i < kNumLines; ++i) v[i] *= norm;
    }

    // Per-line feedback gain for a requested RT60: a line of length L must lose
    // 60 dB in T60 seconds, so g = 10^(-3L/T60).
    static float decayGain (double lineSeconds, double rt60Seconds) noexcept
    {
        if (rt60Seconds <= 0.0) return 0.0f;
        const double g = std::pow (10.0, -3.0 * lineSeconds / rt60Seconds);
        // Never exactly 1: the matrix is orthogonal, so max|g| < 1 is precisely
        // the stability condition, and equality is the one value that is not
        // guaranteed to decay.
        return (float) std::min (g, 0.9995);
    }

    // Damping percentage -> low-pass corner, logarithmically. 0% leaves the tail
    // open at 18 kHz; 100% pulls it down to 800 Hz, which is a heavily furnished
    // room rather than a filter sweep.
    static double dampingCutoffHz (double dampingPct) noexcept
    {
        const double t = std::clamp (dampingPct, 0.0, 100.0) * 0.01;
        return 18000.0 * std::pow (800.0 / 18000.0, t);
    }

    // Size percentage -> the fraction of each line's full length actually used.
    // The floor is 25% rather than 0: an FDN with near-zero line lengths is not
    // a small room, it is a comb filter with an audible pitch.
    static double sizeFactor (double sizePct) noexcept
    {
        return 0.25 + 0.0075 * std::clamp (sizePct, 0.0, 100.0);
    }

private:
    static float clampf (float v, float lo, float hi) noexcept
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Parabolic sine, phase in [0,1). Within ~5% of std::sin, which is far more
    // accuracy than a mode-smearing wobble needs, and it costs no library call
    // in a loop that would otherwise make eight of them per sample.
    static float fastSine (double phase01) noexcept
    {
        const double x = phase01 * 2.0 - 1.0;              // -1..1 == -pi..pi
        return (float) (4.0 * x * (1.0 - std::fabs (x)));
    }

    void recompute() noexcept
    {
        const auto   spec  = reverbAlgorithmSpec (getAlgorithm());
        const double sizeF = sizeFactor ((double) sizePct_.load (std::memory_order_relaxed))
                           * (double) spec.lineScale;
        const double rt60  = (double) decaySec_.load (std::memory_order_relaxed);
        const double lowCut = (double) lowCutHz_.load (std::memory_order_relaxed);

        // The algorithm scales the damping CORNER, not the damping percentage: a
        // plate is brighter than a hall at the same damping setting, which is a
        // property of the plate rather than a different number on the dial.
        const double dampHz = std::clamp (
            dampingCutoffHz ((double) dampingPct_.load (std::memory_order_relaxed))
                * (double) spec.dampScale,
            200.0, 20000.0);

        // The DISPERSION allpass sits inside each line's loop, so it lengthens
        // that loop. Its delay is therefore part of the line's length for the
        // decay-gain calculation, or a spring would decay measurably faster than
        // the RT60 it was asked for.
        const bool   dispersing = spec.dispersion > 0.0f;
        dispersing_ = dispersing;

        for (int i = 0; i < kNumLines; ++i)
        {
            const double lineSec = kLineMs[i] * 0.001 * sizeF;
            const double dispSec = dispersing ? kDispMs[i] * 0.001 : 0.0;

            targetLineSamples_[i] = std::max (8.0, lineSec * sampleRate_);
            lineGain_[i]          = decayGain (lineSec + dispSec, rt60);

            lp_[i].setCutoff (dampHz, sampleRate_);
            hp_[i].setCutoff (lowCut, sampleRate_);

            disp_[i].setGain (dispersing ? spec.dispersion : 0.0f);

            // Modulation rates: slow, and mutually incommensurate for the same
            // reason the line lengths are. Two lines wobbling at the same rate
            // stay locked to each other and smear nothing.
            modInc_[i] = kModRateHz[i] / sampleRate_;
        }

        for (int k = 0; k < kNumTaps; ++k)
        {
            const double tapF = 0.001 * sizeF * (double) spec.tapScale * sampleRate_;
            tapSamplesL_[k] = kTapMsL[k] * tapF;
            tapSamplesR_[k] = kTapMsR[k] * tapF;
        }

        // DIFFUSION. The allpass gain IS the density: a Schroeder allpass with a
        // low gain passes a transient through nearly intact, and one near 0.8
        // smears it into a cloud. The algorithm biases what the dial asked for,
        // then it is clamped well below 1 — an allpass at unity gain is an
        // oscillator, not a diffuser.
        const float diff01 = std::clamp (
            diffusionPct_.load (std::memory_order_relaxed) * 0.01f + spec.diffusionBias,
            0.0f, 1.0f);

        // Mapped so that kReverbDiffusionUnityPct lands on EXACTLY 1.0 — the
        // network's own gains — with room to thin below and thicken above. The
        // ceiling is well under 1: an allpass at unity gain is an oscillator.
        const float unity01 = kReverbDiffusionUnityPct * 0.01f;
        const float scale   = kDiffScaleMin
                            + (1.0f - kDiffScaleMin) * diff01 / unity01;

        for (int k = 0; k < kNumDiff; ++k)
        {
            const float g = std::clamp (kDiffGain[k] * scale, 0.05f, 0.82f);
            diffL_[k].setGain (g);
            diffR_[k].setGain (g);
        }

        earlyGain_ = spec.earlyGain;
        elBias_    = spec.earlyLateBias;
        modScale_  = spec.modScale;
    }

    // ---- the network's fixed geometry --------------------------------------
    // Line lengths in ms at full size. Deliberately non-harmonic: lengths with a
    // common factor stack their echoes and the density collapses into a pitch.
    static constexpr double kLineMs[kNumLines] =
        { 26.79, 31.71, 37.33, 43.21, 49.79, 56.33, 62.11, 68.53 };

    // Sign pattern for the input injection. Mixing signs decorrelates what each
    // line initially sees, so the first pass through the matrix already has
    // something to cancel rather than eight copies of the same thing.
    static constexpr float kInjectSign[kNumLines] =
        { 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f };

    static constexpr float kModRateHz[kNumLines] =
        { 0.63f, 0.79f, 0.91f, 1.13f, 1.27f, 1.49f, 1.61f, 1.83f };

    // Early reflections. Different times AND different sign patterns per side —
    // the same taps with a gain difference would just be a panned mono cluster.
    static constexpr double kTapMsL[kNumTaps] = { 4.7,  9.7, 15.3, 22.1, 30.7, 41.3, 52.9 };
    static constexpr double kTapMsR[kNumTaps] = { 6.1, 11.9, 18.7, 26.3, 35.9, 46.7, 58.3 };
    static constexpr float  kTapGain[kNumTaps] =
        { 1.0f, 0.82f, 0.67f, 0.55f, 0.45f, 0.37f, 0.30f };
    static constexpr float  kTapSignL[kNumTaps] = { 1, -1,  1, -1,  1, -1,  1 };
    static constexpr float  kTapSignR[kNumTaps] = { 1,  1, -1, -1,  1, -1, -1 };

    // Diffusion allpasses (Dattorro-style: two short, two longer). The gains are
    // the BASE the diffusion control scales — see recompute().
    static constexpr double kDiffMsL[kNumDiff]  = { 4.77, 3.59, 12.73,  9.31 };
    static constexpr double kDiffMsR[kNumDiff]  = { 5.31, 3.93, 13.79, 10.11 };
    static constexpr float  kDiffGain[kNumDiff] = { 0.70f, 0.70f, 0.625f, 0.625f };

    // What diffusion at 0% leaves of those gains. Not zero: an allpass chain with
    // no gain is four pure delays in the input path, which is a pre-echo rather
    // than "less diffusion".
    static constexpr float  kDiffScaleMin = 0.30f;

    // The spring's per-line dispersion allpasses. SHORT and mutually
    // incommensurate, like everything else in the network: a few milliseconds is
    // enough for the frequency-dependent delay that makes highs lag lows, and
    // longer would read as a second set of echoes rather than as a smear.
    static constexpr double kDispMs[kNumLines] =
        { 1.31, 1.73, 2.11, 2.53, 2.97, 3.41, 3.89, 4.37 };

    // Output trims, set by measurement rather than by taste. Two competing
    // requirements: at 100% wet the reverb has to sit in the same loudness
    // league as the dry it replaces (or the mix knob is useless over most of its
    // travel), while an impulse at ANY size/decay must still peak below unity (a
    // reverb that clips on a transient is a reverb nobody trusts). These land at
    // roughly -7 dB wet-to-dry on sustained material and ~0.6 peak on an
    // impulse. test/reverb_engine_test.cpp pins both ends.
    static constexpr float kEarlyScale  = 1.20f;
    static constexpr float kLateScale   = 1.05f;
    static constexpr float kInjectScale = 0.35355339f;   // 1/sqrt(8)

    static constexpr float kMaxModSamples = 6.0f;

    // ---- targets (message thread writes, audio thread reads) ---------------
    std::atomic<float> sizePct_      { 55.0f };
    std::atomic<float> decaySec_     { 2.0f };
    std::atomic<float> predelayMs_   { 20.0f };
    std::atomic<float> dampingPct_   { 45.0f };
    std::atomic<float> lowCutHz_     { 80.0f };
    std::atomic<float> widthPct_     { 100.0f };
    std::atomic<float> earlyLatePct_ { 70.0f };
    std::atomic<float> modDepthPct_  { 25.0f };
    std::atomic<float> mixPct_       { 30.0f };
    std::atomic<float> duckPct_      { 0.0f };
    std::atomic<float> diffusionPct_ { kReverbDiffusionUnityPct };

    // Stored as an int rather than as the enum: std::atomic<enum class> is legal
    // but needs a lock-free guarantee the standard does not hand out for it, and
    // an int is what every other selector in this codebase stores.
    std::atomic<int>   algorithm_ { (int) ReverbAlgorithm::Hall };

    std::atomic<bool>  dirty_ { true };

    // ---- derived (recompute(), block rate) ---------------------------------
    double targetLineSamples_[kNumLines] { };
    float  lineGain_[kNumLines]          { };
    double modInc_[kNumLines]            { };
    double tapSamplesL_[kNumTaps]        { };
    double tapSamplesR_[kNumTaps]        { };

    // The algorithm's contributions, resolved once per recompute so the per-sample
    // loop never touches the spec table.
    float earlyGain_  = 1.0f;
    float elBias_     = 0.0f;
    float modScale_   = 1.0f;
    bool  dispersing_ = false;

    // ---- audio-thread state -------------------------------------------------
    DelayLine        predelayL_, predelayR_;
    DelayLine        lines_[kNumLines];
    OnePoleLowpass   lp_[kNumLines];
    OnePoleHighpass  hp_[kNumLines];
    Smoother         lineDelay_[kNumLines];
    SchroederAllpass diffL_[kNumDiff], diffR_[kNumDiff];
    SchroederAllpass disp_[kNumLines];
    Ducker           ducker_;
    Smoother         predelaySmooth_, mixSmooth_, widthSmooth_, elSmooth_, duckSmooth_;
    double           modPhase_[kNumLines] { };

    double sampleRate_ = 44100.0;
};

} // namespace echojay
