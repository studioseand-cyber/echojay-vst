/*
    EedGainEngine.h  —  level + pan for "EchoJay Gain".

    JUCE-free, like EqEngine: a plain C++ core with a g++ unit test
    (test/gain_engine_test.cpp) and no dependency on the plugin build. Header-only
    because every function is a handful of lines and inlining them matters on a
    per-sample loop; there is no EedGainEngine.cpp on purpose.

    Two things it has to get right, both easy to get subtly wrong:

    * Pan law. Centre must be UNITY — inserting this device at its defaults has to
      leave the signal bit-identical, or it is not a utility. Constant-power
      (sin/cos) keeps gl^2 + gr^2 constant so a pan sweep holds its loudness,
      scaled so centre lands on 1.0 rather than 0.707. A linear pan instead dips
      ~3 dB in the middle: the classic "why did it get quieter when I centred it".

    * Smoothing. Level and pan are written from the message thread while the audio
      thread reads them. Jumping a gain coefficient inside a block is an audible
      click, so both ramp per sample toward their target. That ramp is what makes
      an AI move landing mid-playback a fade instead of an event.

    Real-time contract, same as EqEngine: process() never allocates, never locks,
    never blocks. Parameters are plain atomics, read once at the top of a block.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace echojay
{

class GainEngine
{
public:
    // Advertised ranges. The SAME numbers the ParamSchema publishes — the schema
    // is the contract the model and server see, this is what enforces it in DSP.
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb =  24.0f;

    GainEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        // ~20 ms to cover the remaining distance: long enough that a full-scale
        // level jump is a fade rather than a step, short enough that a move still
        // feels immediate.
        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));
        snap();
    }

    // Jump straight to the targets (transport stop / fresh state load), so a
    // restored session does not audibly ramp in from the previous values.
    void reset() noexcept { snap(); }

    // ---- parameters (message thread) --------------------------------------
    void setLevelDb (float db) noexcept
    {
        levelDbTarget_.store (clampDb (db), std::memory_order_relaxed);
    }

    // -1 = hard left, 0 = centre, +1 = hard right.
    void setPan (float p) noexcept
    {
        panTarget_.store (std::clamp (p, -1.0f, 1.0f), std::memory_order_relaxed);
    }

    float getLevelDb() const noexcept { return levelDbTarget_.load (std::memory_order_relaxed); }
    float getPan()     const noexcept { return panTarget_.load (std::memory_order_relaxed); }

    // ---- audio thread ------------------------------------------------------
    // In-place stereo process. `right` may be null for mono; pan is then IGNORED
    // rather than applied as an attenuation — there is no second channel to pan
    // into, so honouring it would just silently quiet the signal at hard pans.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float targetGain = dbToGain (levelDbTarget_.load (std::memory_order_relaxed));
        const float targetPan  = panTarget_.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            // One-pole toward the target: coeff is the fraction of the remaining
            // distance kept each sample.
            gain_ = targetGain + (gain_ - targetGain) * smoothCoeff_;
            pan_  = targetPan  + (pan_  - targetPan)  * smoothCoeff_;

            if (right != nullptr)
            {
                float gl, gr;
                panGains (pan_, gl, gr);
                left[i]  *= gain_ * gl;
                right[i] *= gain_ * gr;
            }
            else
            {
                left[i] *= gain_;
            }
        }
    }

    // ---- pure helpers (what the g++ test pins) ----------------------------
    static float clampDb (float db) noexcept { return std::clamp (db, kMinDb, kMaxDb); }

    // -60 dB is the advertised floor and is treated as TRUE silence, so a
    // "mute it" move actually mutes instead of leaving -60 dB of bleed.
    static float dbToGain (float db) noexcept
    {
        if (db <= kMinDb) return 0.0f;
        return std::pow (10.0f, db * 0.05f);
    }

    // Constant-power pan, normalised so centre is unity.
    //   theta sweeps 0 .. pi/2 as pan goes -1 .. +1
    //   gl = sqrt(2)*cos(theta), gr = sqrt(2)*sin(theta)
    // => centre: gl = gr = 1.0 (bit-identical pass-through)
    // => hard pan: the surviving side reaches +3 dB, holding constant power.
    static void panGains (float pan, float& gl, float& gr) noexcept
    {
        constexpr float kHalfPi = 1.57079632679489661923f;
        constexpr float kRoot2  = 1.41421356237309504880f;

        const float theta = (std::clamp (pan, -1.0f, 1.0f) + 1.0f) * 0.5f * kHalfPi;
        gl = kRoot2 * std::cos (theta);
        gr = kRoot2 * std::sin (theta);
    }

private:
    void snap() noexcept
    {
        gain_ = dbToGain (levelDbTarget_.load (std::memory_order_relaxed));
        pan_  = panTarget_.load (std::memory_order_relaxed);
    }

    std::atomic<float> levelDbTarget_ { 0.0f };
    std::atomic<float> panTarget_     { 0.0f };

    // Smoothed state — audio thread only, never touched from elsewhere.
    float  gain_        = 1.0f;
    float  pan_         = 0.0f;
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
