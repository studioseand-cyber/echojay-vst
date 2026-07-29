/*
    EedTemplateEngine.h  —  SKELETON. Copy to Source/, rename Template -> your
    device. Not compiled where it sits (see DeviceTemplate/README.md).

    The JUCE-free DSP core. Same discipline as EqEngine and GainEngine:
      * no JUCE, so it unit-tests under plain g++ (add test/<name>_engine_test.cpp),
      * parameters published as atomics, read once per block,
      * process() never allocates, never locks, never blocks,
      * every parameter SMOOTHED — a value that jumps inside a block is a click,
        and an AI move can land mid-playback.

    Skip the engine only when the DSP is a single arithmetic op; see
    EedPhaseInvertProcessor for that documented exception.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace echojay
{

class TemplateEngine
{
public:
    // Keep the advertised limits next to the DSP that enforces them. The
    // ParamSchema publishes these SAME numbers — one source, two readers.
    static constexpr float kMinAmount = 0.0f;
    static constexpr float kMaxAmount = 100.0f;

    TemplateEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        constexpr double tau = 0.020;              // ~20 ms parameter glide
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));
        reset();
    }

    // Jump to the targets: a restored session must not audibly ramp in.
    void reset() noexcept
    {
        amount_ = amountTarget_.load (std::memory_order_relaxed);
    }

    // ---- parameters (message thread) --------------------------------------
    void setAmount (float v) noexcept
    {
        amountTarget_.store (std::clamp (v, kMinAmount, kMaxAmount),
                             std::memory_order_relaxed);
    }
    float getAmount() const noexcept { return amountTarget_.load (std::memory_order_relaxed); }

    // ---- audio thread ------------------------------------------------------
    // `right` may be null for mono. Decide deliberately what a stereo-only
    // parameter means with one channel rather than letting it fall out.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float target = amountTarget_.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            amount_ = target + (amount_ - target) * smoothCoeff_;

            // TODO: your DSP. `amount_` is the smoothed, per-sample value.
            (void) left; (void) right; (void) i;
        }
    }

private:
    std::atomic<float> amountTarget_ { 0.0f };

    float  amount_      = 0.0f;      // smoothed; audio thread only
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
