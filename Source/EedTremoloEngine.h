/*
    EedTremoloEngine.h  —  amplitude modulation for "EchoJay Tremolo".

    The thinnest of the four modulation faces: the LFO is the whole effect, and
    this file is only the gain law and the wet/dry blend. JUCE-free with a g++
    test (test/mod_engines_test.cpp), header-only like GainEngine.

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

class TremoloEngine
{
public:
    static constexpr float kMinMix = 0.0f;     // %
    static constexpr float kMaxMix = 100.0f;

    TremoloEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        lfo_.prepare (sampleRate_);

        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));
        reset();
    }

    void reset() noexcept
    {
        lfo_.reset();
        mix_ = mixTarget_.load (std::memory_order_relaxed) * 0.01f;
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

    // ---- audio thread ------------------------------------------------------
    // In-place. `right` may be null for mono, which runs the left LFO only.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float mixT = mixTarget_.load (std::memory_order_relaxed) * 0.01f;

        for (int i = 0; i < numSamples; ++i)
        {
            float mL = 0.0f, mR = 0.0f;
            lfo_.nextStereo (mL, mR);

            const float d = lfo_.smoothedDepth01();
            mix_ = mixT + (mix_ - mixT) * smoothCoeff_;

            const float gL = gainFor (d, mL);
            left[i] += mix_ * (left[i] * gL - left[i]);

            if (right != nullptr)
            {
                const float gR = gainFor (d, mR);
                right[i] += mix_ * (right[i] * gR - right[i]);
            }
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
    LfoCore lfo_;

    std::atomic<float> mixTarget_ { 100.0f };

    float  mix_         = 1.0f;
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
