/*
    EedChorusEngine.h  —  modulated multi-voice delay for "EchoJay Chorus".

    JUCE-free, header-only, g++-tested (test/mod_engines_test.cpp). The delay
    lines are the only allocation and they are sized in prepare(); process()
    never allocates, locks or blocks.

    Four decisions worth reading before changing anything here:

    * SWEEP SCALES WITH THE BASE DELAY. The modulation is +/- min(8 ms, 90% of
      the base delay) rather than a fixed +/- 8 ms. A fixed sweep at a 2 ms base
      delay would drive the read pointer past the write pointer on every trough,
      so the bottom of the waveform flattens against the clamp and the chorus
      turns into a lopsided warble that gets worse the shorter you set it. Tying
      the sweep to the base keeps the modulation symmetrical at every setting,
      which is why the device stays usable across its whole delay range.

    * VOICES ARE SPREAD EVENLY ROUND THE CYCLE — voice v sits at phase v/voices.
      Stacking them at the same phase would just make one louder voice; spreading
      them is what produces the ensemble.

    * THE L/R SPREAD IS A FIXED 90 DEGREES, not a knob. It is a constant of the
      algorithm (like the sweep range), and by house rule a device does not ship
      a control that is not in its ParamSchema. A quarter cycle is the classic
      chorus spread: wide, and it stays mono-compatible where 180 would notch.

    * VOICES SUM AT 1/sqrt(n). The voices are decorrelated, so power adds while
      amplitude does not; 1/n would make a 4-voice chorus quieter than a 1-voice
      one, and no normalisation at all would make it louder. Either way the user
      pays for a level change they did not ask for when they turn VOICES.
*/

#pragma once

#include "EedLfoCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace echojay
{

class ChorusEngine
{
public:
    // Advertised ranges — the ParamSchema publishes these SAME numbers.
    static constexpr float kMinDelayMs =  1.0f;
    static constexpr float kMaxDelayMs = 50.0f;
    static constexpr int   kMinVoices  =  1;
    static constexpr int   kMaxVoices  =  4;
    static constexpr float kMinFeedback = -95.0f;   // % (negative = inverted)
    static constexpr float kMaxFeedback =  95.0f;
    static constexpr float kMinMix     =   0.0f;    // %
    static constexpr float kMaxMix     = 100.0f;

    // Constants of the algorithm, deliberately not knobs (see the header note).
    static constexpr float kMaxSweepMs     = 8.0f;
    static constexpr float kStereoSpread01 = 0.25f;  // 90 degrees

    ChorusEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        lfo_.prepare (sampleRate_);
        // Chorus wants a smooth sweep whatever the shape dial says elsewhere;
        // the delay time is an interpolation index, and a stepped one is a click.
        lfo_.setShape (LfoCore::kSine);

        // Headroom for the longest base delay plus its full sweep, plus a couple
        // of samples for the interpolator to read behind.
        const int maxSamples = (int) std::ceil (((double) kMaxDelayMs + (double) kMaxSweepMs)
                                                * 0.001 * sampleRate_) + 8;
        line_[0].prepare (maxSamples);
        line_[1].prepare (maxSamples);

        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));
        reset();
    }

    void reset() noexcept
    {
        lfo_.reset();
        line_[0].clear();
        line_[1].clear();
        fbState_[0] = fbState_[1] = 0.0f;

        delayMs_  = delayMsTarget_.load (std::memory_order_relaxed);
        feedback_ = feedbackTarget_.load (std::memory_order_relaxed) * 0.01f;
        mix_      = mixTarget_.load (std::memory_order_relaxed) * 0.01f;
    }

    LfoCore&       lfo()       noexcept { return lfo_; }
    const LfoCore& lfo() const noexcept { return lfo_; }

    // ---- parameters (message thread) ---------------------------------------
    void setDelayMs (float ms) noexcept
    {
        delayMsTarget_.store (std::clamp (ms, kMinDelayMs, kMaxDelayMs),
                              std::memory_order_relaxed);
    }
    void setVoices (int v) noexcept
    {
        voices_.store (std::clamp (v, kMinVoices, kMaxVoices), std::memory_order_relaxed);
    }
    void setFeedbackPercent (float pct) noexcept
    {
        feedbackTarget_.store (std::clamp (pct, kMinFeedback, kMaxFeedback),
                               std::memory_order_relaxed);
    }
    void setMixPercent (float pct) noexcept
    {
        mixTarget_.store (std::clamp (pct, kMinMix, kMaxMix), std::memory_order_relaxed);
    }

    float getDelayMs()          const noexcept { return delayMsTarget_.load  (std::memory_order_relaxed); }
    int   getVoices()           const noexcept { return voices_.load         (std::memory_order_relaxed); }
    float getFeedbackPercent()  const noexcept { return feedbackTarget_.load (std::memory_order_relaxed); }
    float getMixPercent()       const noexcept { return mixTarget_.load      (std::memory_order_relaxed); }

    // ---- audio thread ------------------------------------------------------
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (line_[0].size <= 0) return;          // never prepared

        const int   voices = voices_.load  (std::memory_order_relaxed);
        const float dTgt   = delayMsTarget_.load  (std::memory_order_relaxed);
        const float fTgt   = feedbackTarget_.load (std::memory_order_relaxed) * 0.01f;
        const float mTgt   = mixTarget_.load      (std::memory_order_relaxed) * 0.01f;
        const float norm   = 1.0f / std::sqrt ((float) voices);

        for (int i = 0; i < numSamples; ++i)
        {
            lfo_.advance();

            delayMs_  = dTgt + (delayMs_  - dTgt) * smoothCoeff_;
            feedback_ = fTgt + (feedback_ - fTgt) * smoothCoeff_;
            mix_      = mTgt + (mix_      - mTgt) * smoothCoeff_;

            const float sweepMs   = sweepMsFor (delayMs_);
            const float toSamples = (float) (0.001 * sampleRate_);

            left[i] = processChannel (0, left[i], voices, 0.0f,
                                      delayMs_, sweepMs, toSamples, norm);

            if (right != nullptr)
                right[i] = processChannel (1, right[i], voices, kStereoSpread01,
                                           delayMs_, sweepMs, toSamples, norm);
        }
    }

    // ---- pure helper (what the g++ test pins) ------------------------------
    // The sweep never exceeds 90% of the base delay, so the modulation stays
    // symmetrical instead of flattening against the read-pointer clamp.
    static float sweepMsFor (float baseDelayMs) noexcept
    {
        return std::min (kMaxSweepMs, std::max (0.0f, baseDelayMs) * 0.9f);
    }

private:
    // A fractional-read circular buffer. Linear interpolation: the sweep is slow
    // and the artefact is a gentle HF loss, which is what a chorus wants anyway.
    struct Line
    {
        std::vector<float> buf;
        int size  = 0;
        int write = 0;

        void prepare (int n)
        {
            size = std::max (8, n);
            buf.assign ((size_t) size, 0.0f);
            write = 0;
        }
        void clear() noexcept
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            write = 0;
        }
        void push (float x) noexcept
        {
            buf[(size_t) write] = x;
            if (++write >= size) write = 0;
        }
        // Call AFTER push: delay 1 is the sample just written.
        float read (float delaySamples) const noexcept
        {
            const float d = std::clamp (delaySamples, 1.0f, (float) (size - 2));
            float rp = (float) write - 1.0f - d;
            while (rp < 0.0f) rp += (float) size;

            const int i0 = (int) rp;
            const float frac = rp - (float) i0;
            int i1 = i0 + 1;
            if (i1 >= size) i1 -= size;

            return buf[(size_t) i0] + frac * (buf[(size_t) i1] - buf[(size_t) i0]);
        }
    };

    float processChannel (int ch, float in, int voices, float channelOffset01,
                          float baseMs, float sweepMs, float toSamples,
                          float norm) noexcept
    {
        // Bound what re-enters the line. At 95% feedback a transient would
        // otherwise take a very long time to decay, and any DC that sneaks in
        // grows without limit; clamping the state costs nothing and makes the
        // top of the feedback range safe to actually use.
        const float fed = std::clamp (in + feedback_ * fbState_[ch], -4.0f, 4.0f);
        line_[ch].push (fed);

        float sum = 0.0f;
        for (int v = 0; v < voices; ++v)
        {
            const float voiceOffset = (float) v / (float) voices;
            const float mod = lfo_.valueAt (voiceOffset + channelOffset01);
            sum += line_[ch].read ((baseMs + sweepMs * mod) * toSamples);
        }

        // What goes AROUND the loop is the voices' MEAN, not their 1/sqrt(n) sum.
        // The output normalisation assumes the voices are decorrelated, which they
        // are — but a feedback loop has to be bounded in the WORST case, where they
        // are not: at 4 voices, 1/sqrt(n) puts the worst-case loop gain at 0.95*2,
        // and the effect howls up into the safety clamp instead of ringing. The
        // mean keeps the loop gain at exactly the feedback the user dialled, so
        // 95% is a long ring rather than a runaway.
        fbState_[ch] = sum / (float) voices;

        const float wet = sum * norm;
        return in + mix_ * (wet - in);
    }

    LfoCore lfo_;
    Line    line_[2];

    std::atomic<float> delayMsTarget_  { 12.0f };
    std::atomic<int>   voices_         { 2 };
    std::atomic<float> feedbackTarget_ { 0.0f };
    std::atomic<float> mixTarget_      { 50.0f };

    // Audio-thread state only.
    float  delayMs_     = 12.0f;
    float  feedback_    = 0.0f;
    float  mix_         = 0.5f;
    float  fbState_[2]  { 0.0f, 0.0f };
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
