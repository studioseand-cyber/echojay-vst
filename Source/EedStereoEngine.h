/*
    EedStereoEngine.h  —  the shared DSP core for the STEREO cluster
    (BUILTIN_SUITE_PLAN.md §4, Wave 1 Session A).

    One core, several faces: "EchoJay Stereo Width" and "EchoJay Stereoizer" are
    both this engine with a different subset of knobs exposed. JUCE-free like
    EqEngine/GainEngine, so it unit-tests under plain g++
    (test/stereo_engine_test.cpp) with no plugin build in the way.

    ------------------------------------------------------------------------
    THE ONE INVARIANT: everything here except rotation and trim touches the SIDE
    signal ONLY.
    ------------------------------------------------------------------------
    With L = M + S and R = M - S, the mono sum is L + R = 2M for any S at all.
    So width, bass-mono, the Haas stage and the dry/wet mix — every one of which
    only ever rewrites S — leave the mono sum mathematically UNCHANGED. That is
    what "mono-safe" means here, and it is a property of the structure rather
    than something tuned in afterwards: a broadcast fold-down of a Stereoizer'd
    track is the same fold-down it would have been without it, with no comb
    filtering. The g++ test pins this over randomised parameter sets.

    Rotation and output trim are the deliberate exceptions and are documented as
    such below.

    ------------------------------------------------------------------------
    Signal order, per sample
    ------------------------------------------------------------------------
        M,S  <- encode(L,R)
        rotate      (M,S)          tilt the image; NOT mono-safe, by nature
        width       S *= w         0% mono .. 100% unity .. 200% double
                    (multiband: S is split low/mid/high with Linkwitz-Riley
                     and each band gets its OWN width — see WidthMode below)
        widen       haas | comb | dimension — the Stereoizer's character stage,
                    each of which only ever REWRITES S (see StereoizerMode)
        bass-mono   S -= lp(S)     everything under the corner collapses to mono
        wet L,R <- decode(M,S)
        mix         out = dry + mix * (wet - dry)
        trim        out *= g

    THE DEPTH PASS (DEVICE_DEPTH_PLAN.md, Stereo) adds the two mode switches and
    keeps the invariant by construction: the multiband splitter runs on the SIDE
    signal only (the mid never meets a filter), and all three widen modes write
    S and nothing else. Their neutral settings — WidthMode::Full, all band
    widths 100, StereoizerMode::Haas — are bit-for-bit the engine that shipped
    before they existed.

    Bass-mono sits AFTER the Haas stage on purpose. The Haas stage injects
    delay_t(M) - M into the side, and at 15 ms that difference is enormous down
    at 30 Hz — run the mono-maker first and the widener puts the bass straight
    back into the sides, which is exactly the failure the control exists to
    prevent. Last stage wins, so last stage it is.

    Every parameter is smoothed per sample. A value that jumps inside a block is
    a click, and an AI move can land mid-playback — the smoother is what makes a
    dialled move a fade rather than an event.

    Real-time contract, same as EqEngine: process() never allocates, never locks,
    never blocks. prepare() owns the one allocation (the delay line).
*/

#pragma once

// For Biquad / LinkwitzRiley4 / LR4Allpass: the SAME crossover the 4-Band
// Compressor splits with, reused rather than re-derived — its "low + high sums
// to a pure allpass" property is exactly what makes multiband width transparent
// at unity, and it is already pinned by test/dynamics_core_test.cpp.
#include "EedDynamicsCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
// How width is applied (the Stereo Width face's `mode` param).
//   Full      — one width for the whole spectrum; the engine as it shipped.
//   Multiband — the SIDE signal is split low/mid/high with LR4 crossovers and
//               each band gets its own width. The mid is never filtered, so the
//               mono sum survives exactly; at unity widths the recombined side
//               is a pure allpass of the input side (magnitude-flat).
// ---------------------------------------------------------------------------
enum class WidthMode
{
    Full      = 0,
    Multiband = 1,
};

constexpr int kNumWidthModes = 2;

inline const char* widthModeName (WidthMode m) noexcept
{
    return m == WidthMode::Multiband ? "multiband" : "full";
}

inline WidthMode widthModeFromIndex (int i) noexcept
{
    return i == 1 ? WidthMode::Multiband : WidthMode::Full;
}

// ---------------------------------------------------------------------------
// The Stereoizer's character stage (its `mode` param). Every one of these only
// ever REWRITES THE SIDE, so the exact mono fold-down holds in all three.
//   Haas      — time displacement: side = delay(M+S) - M. The engine as it
//               shipped; haas_ms sets the displacement.
//   Comb      — allpass widening: a short fixed allpass chain of the mid is
//               pushed into the side. Complementary comb filtering between L
//               and R with NO pre-delay smear; haas_ms is not used.
//   Dimension — chorused widen: a slow modulated (detuned) copy is pushed into
//               the side. The moving delay keeps any comb notches moving, so it
//               reads as lushness rather than filtering; haas_ms is not used.
// ---------------------------------------------------------------------------
enum class StereoizerMode
{
    Haas      = 0,
    Comb      = 1,
    Dimension = 2,
};

constexpr int kNumStereoizerModes = 3;

inline const char* stereoizerModeName (StereoizerMode m) noexcept
{
    switch (m)
    {
        case StereoizerMode::Comb:      return "comb";
        case StereoizerMode::Dimension: return "dimension";
        case StereoizerMode::Haas:
        default:                        return "haas";
    }
}

inline StereoizerMode stereoizerModeFromIndex (int i) noexcept
{
    if (i <= 0) return StereoizerMode::Haas;
    if (i >= kNumStereoizerModes - 1) return StereoizerMode::Dimension;
    return (StereoizerMode) i;
}

class StereoEngine
{
public:
    // Advertised limits. The ParamSchemas of both devices publish these SAME
    // numbers — one source, two readers: the schema is the contract the model
    // and the server see, this is what enforces it in DSP.
    static constexpr float kMinWidthPct    =   0.0f;   // mono
    static constexpr float kMaxWidthPct    = 200.0f;   // double the side

    static constexpr float kMinRotationDeg = -45.0f;
    static constexpr float kMaxRotationDeg =  45.0f;

    static constexpr float kMinBassMonoHz  =   0.0f;   // 0 == OFF, not "0 Hz"
    static constexpr float kMaxBassMonoHz  = 500.0f;

    static constexpr float kMinHaasMs      =   0.0f;   // 0 == exact bypass
    static constexpr float kMaxHaasMs      =  40.0f;   // past ~40 ms it is echo

    static constexpr float kMinMixPct      =   0.0f;
    static constexpr float kMaxMixPct      = 100.0f;

    static constexpr float kMinTrimDb      = -24.0f;
    static constexpr float kMaxTrimDb      =  24.0f;

    // The two multiband crossovers. The ranges deliberately DO NOT overlap
    // (800 < 1000), so low < high is guaranteed by clamping alone and a swapped
    // pair cannot produce a band of zero width.
    static constexpr float kMinXoverLowHz  =   40.0f;
    static constexpr float kMaxXoverLowHz  =  800.0f;
    static constexpr float kMinXoverHighHz = 1000.0f;
    static constexpr float kMaxXoverHighHz = 12000.0f;

    StereoEngine() = default;

    // Allocates the delay line. Call from prepareToPlay, never from the audio
    // thread.
    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

        // ~20 ms to cover the remaining distance: long enough that a full-scale
        // move is a fade rather than a step, short enough to still feel immediate.
        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));

        // Room for the longest advertised delay at this rate, plus a guard for
        // the interpolator's second tap. Sized from kMaxHaasMs rather than from
        // the current setting, so dialling the delay up later never reallocates
        // on the audio thread.
        const int maxDelaySamples = (int) std::ceil (kMaxHaasMs * 0.001 * sampleRate_) + 4;
        delay_.assign ((std::size_t) std::max (maxDelaySamples, 8), 0.0f);
        writePos_ = 0;

        // The comb mode's fixed allpass chain. Two Schroeder allpasses with
        // incommensurate delays, short enough (3-5 ms) that the widening fuses
        // with the source instead of reading as a pre-delay — that absence of
        // smear is the mode's whole reason to exist next to Haas.
        combAp1_.prepare (sampleRate_, 2.9);
        combAp2_.prepare (sampleRate_, 4.3);

        // Multiband crossovers get real coefficients before the first block.
        xoverLowCur_ = xoverHighCur_ = -1.0f;
        updateCrossovers();

        reset();
    }

    // Jump to the targets and clear the tail. A restored session must not
    // audibly ramp in from the previous instance's values.
    void reset() noexcept
    {
        widthNorm_ = widthTarget_.load (std::memory_order_relaxed) * 0.01f;
        rotation_  = rotationTarget_.load (std::memory_order_relaxed) * kDegToRad;
        haasSamps_ = msToSamples (haasTarget_.load (std::memory_order_relaxed));
        mixNorm_   = mixTarget_.load (std::memory_order_relaxed) * 0.01f;
        trimGain_  = dbToGain (trimTarget_.load (std::memory_order_relaxed));

        widthLowNorm_  = widthLowTarget_.load  (std::memory_order_relaxed) * 0.01f;
        widthMidNorm_  = widthMidTarget_.load  (std::memory_order_relaxed) * 0.01f;
        widthHighNorm_ = widthHighTarget_.load (std::memory_order_relaxed) * 0.01f;

        lpState_  = 0.0f;
        lpState2_ = 0.0f;
        std::fill (delay_.begin(), delay_.end(), 0.0f);
        writePos_ = 0;

        splitLow_.reset();
        splitHigh_.reset();
        apHighComp_.reset();
        combAp1_.reset();
        combAp2_.reset();
        dimPhase_ = 0.0f;
    }

    // ---- parameters (message thread) --------------------------------------
    void setWidthPercent (float pct) noexcept
    {
        widthTarget_.store (std::clamp (pct, kMinWidthPct, kMaxWidthPct),
                            std::memory_order_relaxed);
    }

    // Positive rotates the image to the RIGHT (a centred source at +45 deg ends
    // up hard right). Preserves M^2 + S^2, so the image tilts without changing
    // total power — but it DOES rewrite M, which is why it is the one stage that
    // changes the mono sum.
    void setRotationDegrees (float deg) noexcept
    {
        rotationTarget_.store (std::clamp (deg, kMinRotationDeg, kMaxRotationDeg),
                               std::memory_order_relaxed);
    }

    // 0 disables the filter outright rather than running a 0 Hz one — an "off"
    // that still costs a filter is an off that still colours the signal.
    void setBassMonoHz (float hz) noexcept
    {
        bassMonoTarget_.store (std::clamp (hz, kMinBassMonoHz, kMaxBassMonoHz),
                               std::memory_order_relaxed);
    }

    void setHaasMs (float ms) noexcept
    {
        haasTarget_.store (std::clamp (ms, kMinHaasMs, kMaxHaasMs),
                           std::memory_order_relaxed);
    }

    void setMixPercent (float pct) noexcept
    {
        mixTarget_.store (std::clamp (pct, kMinMixPct, kMaxMixPct),
                          std::memory_order_relaxed);
    }

    void setTrimDb (float db) noexcept
    {
        trimTarget_.store (std::clamp (db, kMinTrimDb, kMaxTrimDb),
                           std::memory_order_relaxed);
    }

    // ---- the depth pass's switches and knobs -------------------------------
    // Modes are plain atomic ints, read once per block: a torn read is a block
    // of the other mode, which is exactly what a legitimate switch mid-play is.
    void setWidthMode (WidthMode m) noexcept
    {
        widthMode_.store ((int) m, std::memory_order_relaxed);
    }

    void setStereoizerMode (StereoizerMode m) noexcept
    {
        stereoizerMode_.store ((int) m, std::memory_order_relaxed);
    }

    void setWidthLowPercent (float pct) noexcept
    {
        widthLowTarget_.store (std::clamp (pct, kMinWidthPct, kMaxWidthPct),
                               std::memory_order_relaxed);
    }

    void setWidthMidPercent (float pct) noexcept
    {
        widthMidTarget_.store (std::clamp (pct, kMinWidthPct, kMaxWidthPct),
                               std::memory_order_relaxed);
    }

    void setWidthHighPercent (float pct) noexcept
    {
        widthHighTarget_.store (std::clamp (pct, kMinWidthPct, kMaxWidthPct),
                                std::memory_order_relaxed);
    }

    void setXoverLowHz (float hz) noexcept
    {
        xoverLowTarget_.store (std::clamp (hz, kMinXoverLowHz, kMaxXoverLowHz),
                               std::memory_order_relaxed);
    }

    void setXoverHighHz (float hz) noexcept
    {
        xoverHighTarget_.store (std::clamp (hz, kMinXoverHighHz, kMaxXoverHighHz),
                                std::memory_order_relaxed);
    }

    float getWidthPercent()   const noexcept { return widthTarget_.load    (std::memory_order_relaxed); }
    float getRotationDegrees()const noexcept { return rotationTarget_.load (std::memory_order_relaxed); }
    float getBassMonoHz()     const noexcept { return bassMonoTarget_.load (std::memory_order_relaxed); }
    float getHaasMs()         const noexcept { return haasTarget_.load     (std::memory_order_relaxed); }
    float getMixPercent()     const noexcept { return mixTarget_.load      (std::memory_order_relaxed); }
    float getTrimDb()         const noexcept { return trimTarget_.load     (std::memory_order_relaxed); }

    WidthMode      getWidthMode()      const noexcept { return widthModeFromIndex      (widthMode_.load      (std::memory_order_relaxed)); }
    StereoizerMode getStereoizerMode() const noexcept { return stereoizerModeFromIndex (stereoizerMode_.load (std::memory_order_relaxed)); }

    float getWidthLowPercent()  const noexcept { return widthLowTarget_.load  (std::memory_order_relaxed); }
    float getWidthMidPercent()  const noexcept { return widthMidTarget_.load  (std::memory_order_relaxed); }
    float getWidthHighPercent() const noexcept { return widthHighTarget_.load (std::memory_order_relaxed); }
    float getXoverLowHz()       const noexcept { return xoverLowTarget_.load  (std::memory_order_relaxed); }
    float getXoverHighHz()      const noexcept { return xoverHighTarget_.load (std::memory_order_relaxed); }

    // ---- audio thread ------------------------------------------------------
    // In-place. `right` may be null for mono, in which case there is no stereo
    // field to work on at all: width, rotation, bass-mono and the Haas stage are
    // SKIPPED rather than approximated, and only the output trim applies.
    // Faking width from one channel would mean inventing a second channel the
    // host never asked for and cannot hear.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float widthT = widthTarget_.load    (std::memory_order_relaxed) * 0.01f;
        const float rotT   = rotationTarget_.load (std::memory_order_relaxed) * kDegToRad;
        const float haasT  = msToSamples (haasTarget_.load (std::memory_order_relaxed));
        const float mixT   = mixTarget_.load      (std::memory_order_relaxed) * 0.01f;
        const float trimT  = dbToGain (trimTarget_.load (std::memory_order_relaxed));

        const float wLowT  = widthLowTarget_.load  (std::memory_order_relaxed) * 0.01f;
        const float wMidT  = widthMidTarget_.load  (std::memory_order_relaxed) * 0.01f;
        const float wHighT = widthHighTarget_.load (std::memory_order_relaxed) * 0.01f;

        const auto wMode  = widthModeFromIndex (widthMode_.load (std::memory_order_relaxed));
        const auto szMode = stereoizerModeFromIndex (stereoizerMode_.load (std::memory_order_relaxed));
        const bool multiband = wMode == WidthMode::Multiband;

        // Crossover moves replace coefficients and keep filter state, so a
        // dragged crossover is a sweep rather than a click. Checked per block:
        // a coefficient recompute per sample would buy nothing audible.
        if (multiband) updateCrossovers();

        const float bassHz = bassMonoTarget_.load (std::memory_order_relaxed);
        const bool  bassOn = bassHz > kMinBassMonoHz;
        const float lpCoeff = bassOn ? onePoleCoeff (bassHz, sampleRate_) : 0.0f;

        // The dimension mode's LFO advance, per sample. Rate and depth are fixed
        // character constants rather than knobs: the mode IS the setting, the
        // width and mix knobs scale how much of it you hear.
        const float dimPhaseInc = kDimRateHz * kTwoPi / (float) sampleRate_;
        const float dimCentre   = msToSamples (kDimCentreMs);
        const float dimDepth    = msToSamples (kDimDepthMs);

        if (right == nullptr)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                trimGain_ = trimT + (trimGain_ - trimT) * smoothCoeff_;
                left[i] *= trimGain_;
            }
            // Keep the smoothers tracking anyway, so switching back to stereo
            // does not then glide from a stale value.
            widthNorm_ = widthT; rotation_ = rotT; haasSamps_ = haasT; mixNorm_ = mixT;
            widthLowNorm_ = wLowT; widthMidNorm_ = wMidT; widthHighNorm_ = wHighT;
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            widthNorm_ = widthT + (widthNorm_ - widthT) * smoothCoeff_;
            rotation_  = rotT   + (rotation_  - rotT)   * smoothCoeff_;
            haasSamps_ = haasT  + (haasSamps_ - haasT)  * smoothCoeff_;
            mixNorm_   = mixT   + (mixNorm_   - mixT)   * smoothCoeff_;
            trimGain_  = trimT  + (trimGain_  - trimT)  * smoothCoeff_;

            widthLowNorm_  = wLowT  + (widthLowNorm_  - wLowT)  * smoothCoeff_;
            widthMidNorm_  = wMidT  + (widthMidNorm_  - wMidT)  * smoothCoeff_;
            widthHighNorm_ = wHighT + (widthHighNorm_ - wHighT) * smoothCoeff_;

            const float dryL = left[i];
            const float dryR = right[i];

            float m = 0.5f * (dryL + dryR);
            float s = 0.5f * (dryL - dryR);

            // ---- rotation ---------------------------------------------------
            // Negated so that POSITIVE degrees tilt the image right; the raw M/S
            // rotation matrix turns the other way, and "+ means right" is the
            // only convention a user or a model can guess correctly.
            if (rotation_ != 0.0f)
            {
                const float c = std::cos (-rotation_);
                const float sn = std::sin (-rotation_);
                const float mR = m * c - s * sn;
                const float sR = m * sn + s * c;
                m = mR;
                s = sR;
            }

            // ---- width ------------------------------------------------------
            if (multiband)
            {
                // The split runs on the SIDE only; M passes by untouched, which
                // is the whole mono-safety argument in one line. The low band
                // gets the second split's allpass so all three bands share the
                // same phase history and sum flat at unity — the identical
                // compensation FourBandSplitter documents.
                float lowRaw = 0.0f, rest = 0.0f, mid = 0.0f, high = 0.0f;
                splitLow_.process (s, lowRaw, rest);
                const float low = apHighComp_.process (lowRaw);
                splitHigh_.process (rest, mid, high);

                s = low * widthLowNorm_ + mid * widthMidNorm_ + high * widthHighNorm_;
            }
            else
            {
                s *= widthNorm_;
            }

            // ---- widen (haas | comb | dimension) ---------------------------
            // The delay line is fed with the CURRENT left channel (m + s) every
            // sample whether or not the stage is doing anything, so turning the
            // delay up reads real history instead of a buffer of silence.
            //
            //   haas: sideOut = delay_t(M + S) - M
            //                 = delay_t(S) + (delay_t(M) - M)
            //
            // i.e. it delays the existing side AND injects the difference between
            // delayed and present mid. The second term is what lets it widen a
            // MONO source (where S is zero and delaying it would do nothing),
            // and at t = 0 it is identically zero, so 0 ms is an exact bypass.
            // M is never touched, so the mono sum survives untouched too.
            //
            // Comb and dimension follow the same construction — inject a
            // decorrelated copy of the mid into the side, never touch the mid —
            // and both scale their injection by the WIDTH smoother, so width 0
            // still means mono and the knob keeps one meaning across all modes.
            {
                const float in = m + s;
                delay_[(std::size_t) writePos_] = in;

                if (szMode == StereoizerMode::Comb)
                {
                    const float ap = combAp2_.process (combAp1_.process (m));
                    s += kCombDepth * widthNorm_ * (ap - m);
                }
                else if (szMode == StereoizerMode::Dimension)
                {
                    dimPhase_ += dimPhaseInc;
                    if (dimPhase_ > kTwoPi) dimPhase_ -= kTwoPi;

                    const float t = dimCentre + dimDepth * std::sin (dimPhase_);
                    s += kDimDepth * widthNorm_ * (readDelay (t) - m);
                }
                else if (haasSamps_ > 0.0f)
                {
                    s = readDelay (haasSamps_) - m;
                }

                if (++writePos_ >= (int) delay_.size()) writePos_ = 0;
            }

            // ---- bass mono --------------------------------------------------
            // Two SUBTRACTIVE one-poles in series: each stage removes what its
            // lowpass passed, so each is a 6 dB/oct highpass and the pair is
            // 12 dB/oct. One stage alone is only -14 dB an octave and a half
            // down, which is not "mono below it" by any honest reading — the
            // sides would still be audibly there.
            //
            // Subtractive rather than a designed highpass because it is EXACT at
            // the edges: with the filter off, or well above the corner, what
            // comes out is the input, not something within a tolerance of it.
            //
            // Runs LAST of the side stages so that it also cleans up the low end
            // the Haas stage just injected (see the header note on ordering).
            if (bassOn)
            {
                lpState_ += (s - lpState_) * lpCoeff;
                if (! (std::fabs (lpState_) > 1.0e-25f)) lpState_ = 0.0f;   // denormal guard
                s -= lpState_;

                lpState2_ += (s - lpState2_) * lpCoeff;
                if (! (std::fabs (lpState2_) > 1.0e-25f)) lpState2_ = 0.0f;
                s -= lpState2_;
            }

            // ---- decode + mix + trim ----------------------------------------
            const float wetL = m + s;
            const float wetR = m - s;

            // Both sides of this blend have the SAME mono sum (mix only ever
            // chose between two signals whose M is identical), so any mix
            // position is mono-safe — no delay compensation needed on the dry
            // path, because nothing delayed the mid.
            left[i]  = (dryL + (wetL - dryL) * mixNorm_) * trimGain_;
            right[i] = (dryR + (wetR - dryR) * mixNorm_) * trimGain_;
        }
    }

    // ---- pure helpers (what the g++ test pins) ----------------------------
    static float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

    // One-pole lowpass coefficient for a -3 dB corner at `hz`.
    static float onePoleCoeff (float hz, double sampleRate) noexcept
    {
        if (hz <= 0.0f || sampleRate <= 0.0) return 0.0f;
        constexpr float kTwoPi = 6.28318530717958647692f;
        const float x = std::exp (-kTwoPi * hz / (float) sampleRate);
        return std::clamp (1.0f - x, 0.0f, 1.0f);
    }

    // L/R -> M/S. The 0.5 lives on the encode side so that decode is a bare
    // add/subtract and encode->decode is exactly unity.
    static void encode (float l, float r, float& m, float& s) noexcept
    {
        m = 0.5f * (l + r);
        s = 0.5f * (l - r);
    }

    static void decode (float m, float s, float& l, float& r) noexcept
    {
        l = m + s;
        r = m - s;
    }

    // The rotation matrix on its own, so the test can pin the convention
    // ("+45 puts a centred source hard right") without running a block.
    static void rotate (float deg, float& m, float& s) noexcept
    {
        const float th = -std::clamp (deg, kMinRotationDeg, kMaxRotationDeg) * kDegToRad;
        const float c = std::cos (th), sn = std::sin (th);
        const float mR = m * c - s * sn;
        const float sR = m * sn + s * c;
        m = mR;
        s = sR;
    }

private:
    static constexpr float kDegToRad = 0.01745329251994329577f;
    static constexpr float kTwoPi    = 6.28318530717958647692f;

    // The comb and dimension modes' character constants. Fixed, not dialable:
    // each mode is one sound, and WIDTH scales how much of it reaches the side.
    static constexpr float kCombDepth   = 0.5f;   // side injection gain, comb
    static constexpr float kDimDepth    = 0.5f;   // side injection gain, dimension
    static constexpr float kDimRateHz   = 0.35f;  // slow enough to read as lush
    static constexpr float kDimCentreMs = 9.0f;   // fused, not an echo
    static constexpr float kDimDepthMs  = 2.5f;   // ~10 cents of detune at peak

    // Schroeder allpass: unity magnitude at every frequency, phase smeared
    // around its delay — which is exactly "filtered widening with no level
    // change". w[n] = x[n] + g*w[n-D]; y[n] = w[n-D] - g*w[n].
    class SchroederAllpass
    {
    public:
        void prepare (double sampleRate, double ms)
        {
            const int n = std::max (8, (int) std::ceil (ms * 0.001 * sampleRate));
            buf_.assign ((std::size_t) n, 0.0f);
            pos_ = 0;
        }

        void reset() noexcept { std::fill (buf_.begin(), buf_.end(), 0.0f); pos_ = 0; }

        float process (float x) noexcept
        {
            if (buf_.empty()) return x;
            const float d = buf_[(std::size_t) pos_];
            float w = x + kG * d;
            if (! (std::fabs (w) > 1.0e-25f)) w = 0.0f;   // denormal guard
            buf_[(std::size_t) pos_] = w;
            if (++pos_ >= (int) buf_.size()) pos_ = 0;
            return d - kG * w;
        }

    private:
        static constexpr float kG = 0.55f;
        std::vector<float> buf_;
        int pos_ = 0;
    };

    // Replace the crossover coefficients only when a target actually moved —
    // setCutoff keeps the filter state, so a dialled crossover sweeps.
    void updateCrossovers() noexcept
    {
        const float lo = xoverLowTarget_.load  (std::memory_order_relaxed);
        const float hi = xoverHighTarget_.load (std::memory_order_relaxed);

        if (lo != xoverLowCur_)
        {
            xoverLowCur_ = lo;
            splitLow_.setCutoff (sampleRate_, (double) lo);
        }
        if (hi != xoverHighCur_)
        {
            xoverHighCur_ = hi;
            splitHigh_.setCutoff (sampleRate_, (double) hi);
            apHighComp_.setCutoff (sampleRate_, (double) hi);
        }
    }

    float msToSamples (float ms) const noexcept
    {
        return std::max (0.0f, ms) * 0.001f * (float) sampleRate_;
    }

    // Linearly interpolated read, `d` samples behind the sample just written.
    // d == 0 returns that sample verbatim, which is what makes 0 ms an exact
    // bypass rather than a one-sample-off approximation.
    float readDelay (float d) const noexcept
    {
        const int size = (int) delay_.size();
        if (size <= 0) return 0.0f;

        const float maxD = (float) (size - 2);
        const float dd = std::clamp (d, 0.0f, maxD > 0.0f ? maxD : 0.0f);

        const int   i0 = (int) dd;
        const float fr = dd - (float) i0;

        int a = writePos_ - i0;
        while (a < 0) a += size;
        int b = a - 1;
        while (b < 0) b += size;

        return delay_[(std::size_t) a] + (delay_[(std::size_t) b] - delay_[(std::size_t) a]) * fr;
    }

    std::atomic<float> widthTarget_    { 100.0f };
    std::atomic<float> rotationTarget_ {   0.0f };
    std::atomic<float> bassMonoTarget_ {   0.0f };
    std::atomic<float> haasTarget_     {   0.0f };
    std::atomic<float> mixTarget_      { 100.0f };
    std::atomic<float> trimTarget_     {   0.0f };

    // The depth pass. Modes stored as ints: std::atomic<enum class> is legal
    // but int is what the whole suite already does (see ReverbEngine).
    std::atomic<int>   widthMode_       { (int) WidthMode::Full };
    std::atomic<int>   stereoizerMode_  { (int) StereoizerMode::Haas };
    std::atomic<float> widthLowTarget_  { 100.0f };
    std::atomic<float> widthMidTarget_  { 100.0f };
    std::atomic<float> widthHighTarget_ { 100.0f };
    std::atomic<float> xoverLowTarget_  { 150.0f };
    std::atomic<float> xoverHighTarget_ { 2500.0f };

    // Smoothed state — audio thread only, never touched from elsewhere.
    float widthNorm_ = 1.0f;
    float rotation_  = 0.0f;      // radians
    float haasSamps_ = 0.0f;
    float mixNorm_   = 1.0f;
    float trimGain_  = 1.0f;

    float widthLowNorm_  = 1.0f;
    float widthMidNorm_  = 1.0f;
    float widthHighNorm_ = 1.0f;

    float lpState_     = 0.0f;   // the mono-maker's two subtractive stages
    float lpState2_    = 0.0f;
    float smoothCoeff_ = 0.0f;

    // Multiband: two LR4 splits of the SIDE plus the low band's phase
    // compensation at the upper crossover (see the width stage).
    LinkwitzRiley4 splitLow_, splitHigh_;
    LR4Allpass     apHighComp_;
    float xoverLowCur_  = -1.0f;   // last cutoffs actually installed
    float xoverHighCur_ = -1.0f;

    // Comb + dimension state.
    SchroederAllpass combAp1_, combAp2_;
    float dimPhase_ = 0.0f;

    std::vector<float> delay_;
    int    writePos_   = 0;
    double sampleRate_ = 44100.0;
};

} // namespace echojay
