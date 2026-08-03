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

    * THE L/R SPREAD IS THE `spread` PARAM, default 50% == the quarter cycle
      (90 degrees) the device shipped with — wide, and mono-compatible where a
      full counter-phase would notch. It was a fixed constant until the depth
      pass; now it is in the ParamSchema like every other control, with the old
      constant as its neutral default so existing sessions sound identical.

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

// ---------------------------------------------------------------------------
// The chorus's CHARACTER (DEVICE_DEPTH_PLAN.md, Modulation depth pass). A
// spec applied on top of what the user dialled, not a separate engine:
//
//   classic    nothing scaled — the device exactly as it shipped, and the
//              neutral default.
//   ensemble   two extra voices on top of the dial, each voice staggered a
//              couple of ms behind the last, and the outer voices sweeping
//              further than the inner ones (the detune skew). That per-voice
//              pitch spread is what a string machine's triple-LFO does, and it
//              is the lushness the mode is named for.
//   dimension  the sweep multiplied by ZERO — no LFO, no pitch wobble at all.
//              The voices sit at fixed staggered delays and the right channel
//              reads the stagger pattern REVERSED, so the two ears hear
//              differently-combed copies: the classic "dimension" widen, a
//              stereo double with nothing moving. Rate/depth/sync do nothing
//              here, which is the point, and the editor takes those dials off
//              the panel.
// ---------------------------------------------------------------------------
enum class ChorusMode
{
    Classic   = 0,
    Ensemble  = 1,
    Dimension = 2
};

constexpr int kNumChorusModes = 3;

struct ChorusModeSpec
{
    int   extraVoices;   // added on top of the dialled voice count
    float sweepScale;    // multiplies the LFO sweep; 0 = no wobble at all
    float detune;        // outer voices sweep this fraction further than inner
    float staggerMs;     // fixed per-voice delay offset, on top of delay_ms
    bool  mirrored;      // right channel reads the stagger reversed + half a
                         // step late, so the two ears get different combs
};

inline ChorusModeSpec chorusModeSpec (ChorusMode m) noexcept
{
    switch (m)
    {
        //                       extra  sweep  detune stagger  mirrored
        case ChorusMode::Ensemble:
            return               { 2,   1.0f,  0.30f, 1.7f,    false };

        case ChorusMode::Dimension:
            return               { 0,   0.0f,  0.0f,  2.3f,    true  };

        case ChorusMode::Classic:
        default:
            return               { 0,   1.0f,  0.0f,  0.0f,    false };
    }
}

inline const char* chorusModeName (ChorusMode m) noexcept
{
    switch (m)
    {
        case ChorusMode::Ensemble:  return "ensemble";
        case ChorusMode::Dimension: return "dimension";
        case ChorusMode::Classic:
        default:                    return "classic";
    }
}

inline ChorusMode chorusModeFromIndex (int i) noexcept
{
    if (i <= 0) return ChorusMode::Classic;
    if (i >= kNumChorusModes - 1) return ChorusMode::Dimension;
    return (ChorusMode) i;
}

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
    static constexpr float kMinSpread  =   0.0f;    // % — 0 is both channels in step
    static constexpr float kMaxSpread  = 100.0f;    //     100 is counter-phase
    static constexpr float kMinToneDb  =  -6.0f;    // tilt on the wet only
    static constexpr float kMaxToneDb  =   6.0f;

    // Constants of the algorithm, deliberately not knobs (see the header note).
    static constexpr float kMaxSweepMs     = 8.0f;

    // The neutral spread: 50% maps to the quarter-cycle (90 degree) channel
    // offset the device shipped with, so spread's default IS the old constant.
    static constexpr float kDefaultSpreadPct = 50.0f;

    // Ensemble adds voices past the dial's ceiling; the lines are sized for it.
    static constexpr int   kMaxVoicesTotal = kMaxVoices + 2;
    static constexpr float kMaxStaggerMs   = 2.5f;

    // Where the tone tilt pivots. Wet-only, one pole each way — a tilt, not
    // an EQ; the knob is a shade, and a shade does not need corners.
    static constexpr float kTonePivotHz    = 800.0f;

    ChorusEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        lfo_.prepare (sampleRate_);
        // Chorus wants a smooth sweep whatever the shape dial says elsewhere;
        // the delay time is an interpolation index, and a stepped one is a click.
        lfo_.setShape (LfoCore::kSine);

        // Headroom for the longest base delay plus its full sweep plus the
        // deepest voice's stagger, plus a couple of samples for the
        // interpolator to read behind.
        const int maxSamples = (int) std::ceil (((double) kMaxDelayMs + (double) kMaxSweepMs
                                                 + (double) kMaxStaggerMs * kMaxVoicesTotal)
                                                * 0.001 * sampleRate_) + 8;
        line_[0].prepare (maxSamples);
        line_[1].prepare (maxSamples);

        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));

        toneLpCoeff_ = (float) (1.0 - std::exp (-6.28318530717958647692 * kTonePivotHz
                                                / sampleRate_));
        reset();
    }

    void reset() noexcept
    {
        lfo_.reset();
        line_[0].clear();
        line_[1].clear();
        fbState_[0] = fbState_[1] = 0.0f;
        toneLp_[0]  = toneLp_[1]  = 0.0f;

        delayMs_  = delayMsTarget_.load (std::memory_order_relaxed);
        feedback_ = feedbackTarget_.load (std::memory_order_relaxed) * 0.01f;
        mix_      = mixTarget_.load (std::memory_order_relaxed) * 0.01f;
        spread01_ = spreadTarget_.load (std::memory_order_relaxed) * 0.005f;

        const float t = toneTarget_.load (std::memory_order_relaxed);
        toneLo_ = std::pow (10.0f, -t * 0.5f * 0.05f);
        toneHi_ = std::pow (10.0f,  t * 0.5f * 0.05f);
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
    void setMode (ChorusMode m) noexcept
    {
        mode_.store ((int) m, std::memory_order_relaxed);
    }
    void setSpreadPercent (float pct) noexcept
    {
        spreadTarget_.store (std::clamp (pct, kMinSpread, kMaxSpread),
                             std::memory_order_relaxed);
    }
    void setToneDb (float db) noexcept
    {
        toneTarget_.store (std::clamp (db, kMinToneDb, kMaxToneDb),
                           std::memory_order_relaxed);
    }

    float getDelayMs()          const noexcept { return delayMsTarget_.load  (std::memory_order_relaxed); }
    int   getVoices()           const noexcept { return voices_.load         (std::memory_order_relaxed); }
    float getFeedbackPercent()  const noexcept { return feedbackTarget_.load (std::memory_order_relaxed); }
    float getMixPercent()       const noexcept { return mixTarget_.load      (std::memory_order_relaxed); }
    float getSpreadPercent()    const noexcept { return spreadTarget_.load   (std::memory_order_relaxed); }
    float getToneDb()           const noexcept { return toneTarget_.load     (std::memory_order_relaxed); }
    ChorusMode getMode() const noexcept
    {
        return chorusModeFromIndex (mode_.load (std::memory_order_relaxed));
    }

    // The voice count actually running: the mode may add to the dial. Public
    // because it is what the editor's hint reports.
    int effectiveVoices() const noexcept
    {
        return std::min (kMaxVoicesTotal,
                         getVoices() + chorusModeSpec (getMode()).extraVoices);
    }

    // ---- audio thread ------------------------------------------------------
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (line_[0].size <= 0) return;          // never prepared

        const auto  spec   = chorusModeSpec (getMode());
        const int   voices = std::min (kMaxVoicesTotal,
                                       voices_.load (std::memory_order_relaxed)
                                       + spec.extraVoices);
        const float dTgt   = delayMsTarget_.load  (std::memory_order_relaxed);
        const float fTgt   = feedbackTarget_.load (std::memory_order_relaxed) * 0.01f;
        const float mTgt   = mixTarget_.load      (std::memory_order_relaxed) * 0.01f;
        // Spread is stored in % of the counter-phase maximum: 100% is half a
        // cycle, so % -> cycles is 0.005.
        const float sTgt   = spreadTarget_.load   (std::memory_order_relaxed) * 0.005f;
        const float norm   = 1.0f / std::sqrt ((float) voices);

        // The tone gains move once per block toward their targets — a tilt is
        // a shade, and a shade sliding over a block is inaudible where a
        // per-sample pow() would be real CPU. Skipped entirely while flat, so
        // tone 0 leaves the wet path bit-identical to what shipped.
        const float t = toneTarget_.load (std::memory_order_relaxed);
        toneLo_ = std::pow (10.0f, -t * 0.5f * 0.05f);
        toneHi_ = std::pow (10.0f,  t * 0.5f * 0.05f);
        toneActive_ = t != 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            lfo_.advance();

            delayMs_  = dTgt + (delayMs_  - dTgt) * smoothCoeff_;
            feedback_ = fTgt + (feedback_ - fTgt) * smoothCoeff_;
            mix_      = mTgt + (mix_      - mTgt) * smoothCoeff_;
            spread01_ = sTgt + (spread01_ - sTgt) * smoothCoeff_;

            const float sweepMs   = sweepMsFor (delayMs_) * spec.sweepScale;
            const float toSamples = (float) (0.001 * sampleRate_);

            left[i] = processChannel (0, left[i], voices, 0.0f,
                                      delayMs_, sweepMs, toSamples, norm, spec);

            if (right != nullptr)
                right[i] = processChannel (1, right[i], voices, spread01_,
                                           delayMs_, sweepMs, toSamples, norm, spec);
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
                          float norm, const ChorusModeSpec& spec) noexcept
    {
        // Bound what re-enters the line. At 95% feedback a transient would
        // otherwise take a very long time to decay, and any DC that sneaks in
        // grows without limit; clamping the state costs nothing and makes the
        // top of the feedback range safe to actually use.
        const float fed = std::clamp (in + feedback_ * fbState_[ch], -4.0f, 4.0f);
        line_[ch].push (fed);

        // The detune skew's per-voice factor: outer voices sweep further than
        // inner ones, centred so the AVERAGE excursion (and so the ensemble's
        // overall pitch) stays where the depth dial put it.
        const float half = 0.5f * (float) (voices - 1);

        float sum = 0.0f;
        for (int v = 0; v < voices; ++v)
        {
            const float voiceOffset = (float) v / (float) voices;
            const float centred     = half > 0.0f ? ((float) v - half) / half : 0.0f;
            const float mod = lfo_.valueAt (voiceOffset + channelOffset01)
                            * (1.0f + spec.detune * centred);

            // Dimension's width lives here: the right channel reads the voice
            // stagger reversed AND pushed half a step later. The half step is
            // what does the work — a pure reversal is a permutation, the
            // voices still SUM to the identical comb, and the channels come
            // out bit-equal (measured, not guessed: the g++ test pins it).
            // Offset, the right ear gets a genuinely different comb, which is
            // the decorrelation the mode is named for.
            const float stagSteps = (spec.mirrored && ch == 1)
                                  ? (float) (voices - 1 - v) + 0.5f
                                  : (float) v;
            const float vBase = baseMs + spec.staggerMs * stagSteps;

            sum += line_[ch].read ((vBase + sweepMs * mod) * toSamples);
        }

        // What goes AROUND the loop is the voices' MEAN, not their 1/sqrt(n) sum.
        // The output normalisation assumes the voices are decorrelated, which they
        // are — but a feedback loop has to be bounded in the WORST case, where they
        // are not: at 4 voices, 1/sqrt(n) puts the worst-case loop gain at 0.95*2,
        // and the effect howls up into the safety clamp instead of ringing. The
        // mean keeps the loop gain at exactly the feedback the user dialled, so
        // 95% is a long ring rather than a runaway.
        fbState_[ch] = sum / (float) voices;

        float wet = sum * norm;

        // The tone tilt, wet only, skipped while flat (see process()).
        if (toneActive_)
        {
            toneLp_[ch] += toneLpCoeff_ * (wet - toneLp_[ch]);
            wet = toneLp_[ch] * toneLo_ + (wet - toneLp_[ch]) * toneHi_;
        }

        return in + mix_ * (wet - in);
    }

    LfoCore lfo_;
    Line    line_[2];

    std::atomic<float> delayMsTarget_  { 12.0f };
    std::atomic<int>   voices_         { 2 };
    std::atomic<float> feedbackTarget_ { 0.0f };
    std::atomic<float> mixTarget_      { 50.0f };
    std::atomic<int>   mode_           { (int) ChorusMode::Classic };
    std::atomic<float> spreadTarget_   { kDefaultSpreadPct };
    std::atomic<float> toneTarget_     { 0.0f };

    // Audio-thread state only.
    float  delayMs_     = 12.0f;
    float  feedback_    = 0.0f;
    float  mix_         = 0.5f;
    float  spread01_    = 0.25f;
    float  fbState_[2]  { 0.0f, 0.0f };
    float  toneLp_[2]   { 0.0f, 0.0f };
    float  toneLpCoeff_ = 0.0f;
    float  toneLo_      = 1.0f;
    float  toneHi_      = 1.0f;
    bool   toneActive_  = false;
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
