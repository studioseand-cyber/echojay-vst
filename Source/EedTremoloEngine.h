/*
    EedTremoloEngine.h  —  amplitude modulation for "EchoJay Tremolo".

    The thinnest of the four modulation faces: the LFO is the whole effect, and
    this file is the gain law, the wet/dry blend, and — since the depth pass —
    the CIRCUIT the gain is applied through (TremoloMode below). JUCE-free with
    a g++ test (test/mod_engines_test.cpp), header-only like GainEngine.

    THE GAIN LAW is the one thing worth getting right. It is written in terms of
    DEPTH, not just of the LFO output:

        gain = 1 - depth/2 + lfo/2          (lfo already carries depth)

    so the modulation always hangs DOWN from unity: the waveform's peak is
    full level and depth only decides how far the trough falls. Depth 0 is exactly
    1.0 — a default insert is bit-identical pass-through — and depth 100% reaches
    true silence at the trough. The obvious alternative, centring the modulation
    on unity, makes a tremolo BOOST on every peak, so turning depth up raises the
    track's peak level and the user has to re-gain-stage to hear the effect
    itself. That is the difference between a tremolo and a tremolo plus a fader
    move.

    Which is also why the engine reads LfoCore::smoothedDepth01() rather than the
    depth target: during a depth change the two disagree, and using the target
    would step the gain by exactly the amount the LFO's smoother was busy
    avoiding.
*/

#pragma once

#include "EedLfoCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace echojay
{

// ---------------------------------------------------------------------------
// The tremolo's CIRCUIT (DEVICE_DEPTH_PLAN.md, Modulation depth pass). The
// gain law is shared; the mode decides what the gain is applied THROUGH:
//
//   sine     the law applied directly — the device exactly as it shipped, and
//            the neutral default. (The name is the circuit family, not the
//            waveform: the `shape` dial still picks the waveform in any mode.)
//   optical  the gain chases its target through an asymmetric one-pole, the
//            way a photocell lags a bulb: quick to duck, slow to recover. The
//            edges soften and the wobble goes program-ish — even a square
//            breathes instead of chopping.
//   bias     harmonic tremolo: the signal splits at a crossover and the lows
//            and highs ride OPPOSITE phases of the LFO, so the level barely
//            moves while the tone swirls — the classic brown-face Fender
//            sound, and the marquee mode of this pass.
// ---------------------------------------------------------------------------
enum class TremoloMode
{
    Sine    = 0,
    Optical = 1,
    Bias    = 2
};

constexpr int kNumTremoloModes = 3;

inline const char* tremoloModeName (TremoloMode m) noexcept
{
    switch (m)
    {
        case TremoloMode::Optical: return "optical";
        case TremoloMode::Bias:    return "bias";
        case TremoloMode::Sine:
        default:                   return "sine";
    }
}

inline TremoloMode tremoloModeFromIndex (int i) noexcept
{
    if (i <= 0) return TremoloMode::Sine;
    if (i >= kNumTremoloModes - 1) return TremoloMode::Bias;
    return (TremoloMode) i;
}

class TremoloEngine
{
public:
    static constexpr float kMinMix = 0.0f;     // %
    static constexpr float kMaxMix = 100.0f;

    // Constants of the two new circuits, deliberately not knobs.
    static constexpr float kBiasCrossoverHz = 800.0f;   // where lows and highs part
    static constexpr float kOptoFallMs      = 6.0f;     // photocell lights fast...
    static constexpr float kOptoRiseMs      = 45.0f;    // ...and dims slowly

    TremoloEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        lfo_.prepare (sampleRate_);

        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));

        // One-pole coefficients for the two circuits. y += a * (x - y) form,
        // so a is the FRACTION covered per sample.
        constexpr double kTwoPi = 6.28318530717958647692;
        xoverCoeff_   = (float) (1.0 - std::exp (-kTwoPi * kBiasCrossoverHz / sampleRate_));
        optoFallCoeff_ = (float) (1.0 - std::exp (-1.0 / (kOptoFallMs * 0.001 * sampleRate_)));
        optoRiseCoeff_ = (float) (1.0 - std::exp (-1.0 / (kOptoRiseMs * 0.001 * sampleRate_)));

        reset();
    }

    void reset() noexcept
    {
        lfo_.reset();
        mix_ = mixTarget_.load (std::memory_order_relaxed) * 0.01f;
        xover_[0] = xover_[1] = 0.0f;
        opto_[0]  = opto_[1]  = 1.0f;   // a resting photocell passes unity
    }

    // The LFO is the device's rate/depth/shape/stereo-phase surface; the
    // processor dials it directly rather than this class re-wrapping every
    // setter, which would be six forwarding functions that can drift.
    LfoCore&       lfo()       noexcept { return lfo_; }
    const LfoCore& lfo() const noexcept { return lfo_; }

    void  setMixPercent (float pct) noexcept
    {
        mixTarget_.store (std::clamp (pct, kMinMix, kMaxMix), std::memory_order_relaxed);
    }
    float getMixPercent() const noexcept { return mixTarget_.load (std::memory_order_relaxed); }

    void setMode (TremoloMode m) noexcept
    {
        mode_.store ((int) m, std::memory_order_relaxed);
    }
    TremoloMode getMode() const noexcept
    {
        return tremoloModeFromIndex (mode_.load (std::memory_order_relaxed));
    }

    // ---- audio thread ------------------------------------------------------
    // In-place. `right` may be null for mono, which runs the left LFO only.
    //
    // `sine` takes exactly the path the device shipped with — the two new
    // circuits branch AFTER the shared gain law, so the neutral mode cannot
    // drift by a rounding term when they change.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float mixT = mixTarget_.load (std::memory_order_relaxed) * 0.01f;
        const auto  mode = getMode();

        for (int i = 0; i < numSamples; ++i)
        {
            float mL = 0.0f, mR = 0.0f;
            lfo_.nextStereo (mL, mR);

            const float d = lfo_.smoothedDepth01();
            mix_ = mixT + (mix_ - mixT) * smoothCoeff_;

            left[i] = processSample (0, left[i], d, mL, mode);
            if (right != nullptr)
                right[i] = processSample (1, right[i], d, mR, mode);
        }
    }

    // ---- pure helper (what the g++ test pins) ------------------------------
    // depth01: the smoothed depth, 0..1. lfoValue: the LFO output, ALREADY
    // scaled by that same depth, -depth..+depth.
    static float gainFor (float depth01, float lfoValue) noexcept
    {
        return std::clamp (1.0f - 0.5f * depth01 + 0.5f * lfoValue, 0.0f, 1.0f);
    }

private:
    float processSample (int ch, float in, float d, float m, TremoloMode mode) noexcept
    {
        if (mode == TremoloMode::Bias)
        {
            // Split at the crossover; the lows ride the LFO, the highs ride
            // its MIRROR — the LFO is zero-mean, so the mirrored tap is the
            // same waveform half a cycle out, which is what makes the sum's
            // level nearly still while the tone rocks between dark and bright.
            // One-pole split: high is defined as in - low, so both gains at
            // unity reconstruct the input and depth 0 stays transparent.
            xover_[ch] += xoverCoeff_ * (in - xover_[ch]);
            const float low  = xover_[ch];
            const float high = in - low;

            const float wet = low * gainFor (d, m) + high * gainFor (d, -m);
            return in + mix_ * (wet - in);
        }

        float g = gainFor (d, m);

        if (mode == TremoloMode::Optical)
        {
            // The photocell: quick toward darkness, slow back toward unity.
            // The asymmetry IS the sound — the dip stays crisp, the recovery
            // blooms.
            const float a = g < opto_[ch] ? optoFallCoeff_ : optoRiseCoeff_;
            opto_[ch] += a * (g - opto_[ch]);
            g = opto_[ch];
        }

        return in + mix_ * (in * g - in);
    }

    LfoCore lfo_;

    std::atomic<float> mixTarget_ { 100.0f };
    std::atomic<int>   mode_      { (int) TremoloMode::Sine };

    float  mix_          = 1.0f;
    float  smoothCoeff_  = 0.0f;
    float  xoverCoeff_   = 0.0f;
    float  optoFallCoeff_ = 0.0f;
    float  optoRiseCoeff_ = 0.0f;
    float  xover_[2]     { 0.0f, 0.0f };
    float  opto_[2]      { 1.0f, 1.0f };
    double sampleRate_   = 44100.0;
};

} // namespace echojay
