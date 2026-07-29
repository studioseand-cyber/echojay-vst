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
        haas        S  = delay(M+S) - M
        bass-mono   S -= lp(S)     everything under the corner collapses to mono
        wet L,R <- decode(M,S)
        mix         out = dry + mix * (wet - dry)
        trim        out *= g

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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace echojay
{

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

        lpState_  = 0.0f;
        lpState2_ = 0.0f;
        std::fill (delay_.begin(), delay_.end(), 0.0f);
        writePos_ = 0;
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

    float getWidthPercent()   const noexcept { return widthTarget_.load    (std::memory_order_relaxed); }
    float getRotationDegrees()const noexcept { return rotationTarget_.load (std::memory_order_relaxed); }
    float getBassMonoHz()     const noexcept { return bassMonoTarget_.load (std::memory_order_relaxed); }
    float getHaasMs()         const noexcept { return haasTarget_.load     (std::memory_order_relaxed); }
    float getMixPercent()     const noexcept { return mixTarget_.load      (std::memory_order_relaxed); }
    float getTrimDb()         const noexcept { return trimTarget_.load     (std::memory_order_relaxed); }

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

        const float bassHz = bassMonoTarget_.load (std::memory_order_relaxed);
        const bool  bassOn = bassHz > kMinBassMonoHz;
        const float lpCoeff = bassOn ? onePoleCoeff (bassHz, sampleRate_) : 0.0f;

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
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            widthNorm_ = widthT + (widthNorm_ - widthT) * smoothCoeff_;
            rotation_  = rotT   + (rotation_  - rotT)   * smoothCoeff_;
            haasSamps_ = haasT  + (haasSamps_ - haasT)  * smoothCoeff_;
            mixNorm_   = mixT   + (mixNorm_   - mixT)   * smoothCoeff_;
            trimGain_  = trimT  + (trimGain_  - trimT)  * smoothCoeff_;

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
            s *= widthNorm_;

            // ---- haas -------------------------------------------------------
            // The delay line is fed with the CURRENT left channel (m + s) every
            // sample whether or not the stage is doing anything, so turning the
            // delay up reads real history instead of a buffer of silence.
            //
            //   sideOut = delay_t(M + S) - M
            //           = delay_t(S) + (delay_t(M) - M)
            //
            // i.e. it delays the existing side AND injects the difference between
            // delayed and present mid. The second term is what lets it widen a
            // MONO source (where S is zero and delaying it would do nothing),
            // and at t = 0 it is identically zero, so 0 ms is an exact bypass.
            // M is never touched, so the mono sum survives untouched too.
            {
                const float in = m + s;
                delay_[(std::size_t) writePos_] = in;

                if (haasSamps_ > 0.0f)
                    s = readDelay (haasSamps_) - m;

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

    // Smoothed state — audio thread only, never touched from elsewhere.
    float widthNorm_ = 1.0f;
    float rotation_  = 0.0f;      // radians
    float haasSamps_ = 0.0f;
    float mixNorm_   = 1.0f;
    float trimGain_  = 1.0f;

    float lpState_     = 0.0f;   // the mono-maker's two subtractive stages
    float lpState2_    = 0.0f;
    float smoothCoeff_ = 0.0f;

    std::vector<float> delay_;
    int    writePos_   = 0;
    double sampleRate_ = 44100.0;
};

} // namespace echojay
