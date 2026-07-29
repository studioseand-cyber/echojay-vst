/*
    EedLfoCore.h  —  the shared modulation core for cluster C
    (BUILTIN_SUITE_PLAN.md §4: "a shared LFO (rate, depth, phase, waveform)").

    Tremolo, Auto Pan, Chorus and Phaser are four faces on THIS. They differ only
    in what they do with the number it produces: scale a gain, steer a pan, sweep
    a delay time, sweep an allpass corner. Everything about being an LFO — rate in
    Hz or locked to the host tempo, depth, waveform, the L/R phase offset, and the
    smoothing that keeps all of that click-free — is written once, here.

    JUCE-free, like EqEngine and GainEngine: it unit-tests under plain g++
    (test/lfo_core_test.cpp) with no dependency on the plugin build. Header-only
    because every function is a handful of lines on a per-sample path.

    Four things it has to get right, all of them easy to get subtly wrong:

    * SMOOTHING, three separate kinds. (1) The phase INCREMENT is smoothed, so
      turning RATE glides the modulation instead of stepping it. (2) DEPTH is
      smoothed, so dialling depth to 0 fades the effect out rather than cutting
      it. (3) The waveform OUTPUT is smoothed, which is what makes SQUARE and SAW
      usable at all: their discontinuity is a full-scale step, and a full-scale
      step in a gain coefficient is a click on every cycle. A few ms of one-pole
      turns that edge into a fast ramp and leaves sine and triangle untouched.

    * PHASE CONTINUITY. Rate changes move the increment, never the phase. A
      waveform whose phase jumps when you turn a knob clicks; one whose frequency
      glides does not.

    * ZERO-MEAN SHAPES. Every waveform is bipolar and averages zero over a cycle,
      including the saw — which is therefore centred (rising through 0 at phase 0)
      rather than the naive 2p-1 ramp that starts at -1. A shape with DC in it
      would make a tremolo quieter and an auto-pan off-centre just by switching
      waveform, which reads as a bug rather than a sound.

    * TEMPO SYNC that degrades gracefully. Host tempo arrives per block from the
      playhead; with no host and no tempo, 120 BPM stands in, so a synced device
      auditioned standalone still modulates instead of freezing.

    Real-time contract, same as the other cores: nothing here allocates, locks or
    blocks. Parameters are plain atomics written from the message thread and read
    on the audio thread.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace echojay
{

class LfoCore
{
public:
    // ---- advertised limits --------------------------------------------------
    // The SAME numbers each device's ParamSchema publishes. The schema is the
    // contract the model and the server see; this is what enforces it in DSP.
    // They are written once, in one place, so a change to one that misses the
    // other is impossible rather than merely visible.
    static constexpr float kMinRateHz    = 0.01f;
    static constexpr float kMaxRateHz    = 20.0f;
    static constexpr float kMinDepth     = 0.0f;    // %
    static constexpr float kMaxDepth     = 100.0f;  // %
    static constexpr float kMinPhaseDeg  = 0.0f;
    static constexpr float kMaxPhaseDeg  = 360.0f;

    // ---- waveform -----------------------------------------------------------
    enum Shape
    {
        kSine = 0, kTriangle = 1, kSquare = 2, kSaw = 3,
        kNumShapes = 4
    };

    static const char* shapeName (int s) noexcept
    {
        switch (clampShape (s))
        {
            case kTriangle: return "TRI";
            case kSquare:   return "SQR";
            case kSaw:      return "SAW";
            case kSine:
            default:        return "SINE";
        }
    }

    // ---- tempo divisions ----------------------------------------------------
    // Beats per CYCLE, one beat = one quarter note. Ordered slowest to fastest so
    // the index reads as a dial: turning it up speeds the modulation up, which is
    // the only ordering a human or a model can predict without a lookup table.
    struct Division { float beats; const char* name; };

    static constexpr int kNumDivisions   = 13;
    static constexpr int kDefaultDivision = 6;      // "1/4"

    static const Division& division (int i) noexcept
    {
        static const Division table[kNumDivisions] = {
            { 16.0f,      "4/1"   },
            {  8.0f,      "2/1"   },
            {  4.0f,      "1/1"   },
            {  2.666667f, "1/1T"  },
            {  2.0f,      "1/2"   },
            {  1.333333f, "1/2T"  },
            {  1.0f,      "1/4"   },
            {  0.666667f, "1/4T"  },
            {  0.5f,      "1/8"   },
            {  0.333333f, "1/8T"  },
            {  0.25f,     "1/16"  },
            {  0.166667f, "1/16T" },
            {  0.125f,    "1/32"  },
        };
        return table[clampDivision (i)];
    }

    LfoCore() = default;

    // ---- lifecycle ----------------------------------------------------------
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

        // ~20 ms to cover the remaining distance for rate and depth: long enough
        // that a full-range move is a glide, short enough that it still feels
        // immediate. Same constant the other EchoJay cores use.
        constexpr double kParamTau = 0.020;
        paramCoeff_ = (float) std::exp (-1.0 / (kParamTau * sampleRate_));

        updateOutputCoeff();
        reset();
    }

    // Jump to the targets and restart the cycle. Called on transport stop and
    // after a state load, so a restored session starts AT its values rather than
    // gliding into them from wherever the previous instance was.
    void reset() noexcept
    {
        phase_       = 0.0f;
        incSmoothed_ = phaseIncrement();
        depthSmooth_ = depth01();

        const int   sh  = shape();
        const float off = stereoOffset01();
        outL_ = shapeAt (sh, phase_);
        outR_ = shapeAt (sh, phase_ + off);
    }

    // ---- parameters (message thread) ---------------------------------------
    void setRateHz (float hz) noexcept
    {
        rateHz_.store (std::clamp (hz, kMinRateHz, kMaxRateHz), std::memory_order_relaxed);
    }

    void setTempoSync (bool on) noexcept { sync_.store (on, std::memory_order_relaxed); }

    void setDivisionIndex (int i) noexcept
    {
        divisionIdx_.store (clampDivision (i), std::memory_order_relaxed);
    }

    // Host tempo, pushed once per block from the playhead. Out-of-range or absent
    // tempo leaves the previous value standing rather than freezing the LFO.
    void setTempoBpm (double bpm) noexcept
    {
        if (bpm >= 20.0 && bpm <= 999.0)
            tempoBpm_.store (bpm, std::memory_order_relaxed);
    }

    void setDepthPercent (float pct) noexcept
    {
        depthPct_.store (std::clamp (pct, kMinDepth, kMaxDepth), std::memory_order_relaxed);
    }

    void setShape (int s) noexcept { shape_.store (clampShape (s), std::memory_order_relaxed); }

    // How far the RIGHT channel's phase leads the left. 0 = both channels in
    // step (a mono-compatible wobble), 180 = fully counter-phase.
    void setStereoPhaseDeg (float deg) noexcept
    {
        stereoDeg_.store (std::clamp (deg, kMinPhaseDeg, kMaxPhaseDeg),
                          std::memory_order_relaxed);
    }

    // Output smoothing time. Only SQUARE and SAW really need it; it is exposed so
    // a device that wants a harder edge (or none) can say so.
    void setSmoothingMs (float ms) noexcept
    {
        smoothMs_.store (std::clamp (ms, 0.0f, 50.0f), std::memory_order_relaxed);
        updateOutputCoeff();
    }

    float getRateHz()        const noexcept { return rateHz_.load      (std::memory_order_relaxed); }
    bool  getTempoSync()     const noexcept { return sync_.load        (std::memory_order_relaxed); }
    int   getDivisionIndex() const noexcept { return divisionIdx_.load (std::memory_order_relaxed); }
    float getDepthPercent()  const noexcept { return depthPct_.load    (std::memory_order_relaxed); }
    int   getShape()         const noexcept { return shape_.load       (std::memory_order_relaxed); }
    float getStereoPhaseDeg()const noexcept { return stereoDeg_.load   (std::memory_order_relaxed); }
    float getSmoothingMs()   const noexcept { return smoothMs_.load    (std::memory_order_relaxed); }
    double getTempoBpm()     const noexcept { return tempoBpm_.load    (std::memory_order_relaxed); }

    // The frequency actually running, whichever way rate is being specified.
    // Public because it is what a readout shows and what the tests pin.
    float effectiveRateHz() const noexcept
    {
        if (! sync_.load (std::memory_order_relaxed))
            return rateHz_.load (std::memory_order_relaxed);

        const double beats = (double) division (divisionIdx_.load (std::memory_order_relaxed)).beats;
        const double bpm   = tempoBpm_.load (std::memory_order_relaxed);
        const double hz    = bpm / (60.0 * beats);

        // A 1/32 at 200 BPM is 26.7 Hz, past the advertised ceiling. Clamping
        // keeps a synced device inside the same range a manual rate can reach,
        // so nothing downstream has to cope with two different limits.
        return (float) std::clamp (hz, (double) kMinRateHz, (double) kMaxRateHz);
    }

    // ---- audio thread -------------------------------------------------------
    // Step the phase one sample and update the smoothed rate/depth. Call ONCE per
    // sample, then read as many taps as you need with valueAt().
    void advance() noexcept
    {
        const float incTarget = phaseIncrement();
        incSmoothed_ = incTarget + (incSmoothed_ - incTarget) * paramCoeff_;

        const float depthTarget = depth01();
        depthSmooth_ = depthTarget + (depthSmooth_ - depthTarget) * paramCoeff_;

        phase_ = wrap01 (phase_ + incSmoothed_);
    }

    // The waveform at an arbitrary phase offset (in cycles, 0..1), scaled by the
    // smoothed depth. This is the multi-tap entry point: Chorus spreads its
    // voices with it, Phaser offsets its right channel with it.
    //
    // NOT output-smoothed — a tap is stateless by construction. The devices that
    // let the user pick SQUARE or SAW use nextStereo() instead, which is.
    float valueAt (float phaseOffset01) const noexcept
    {
        return shapeAt (shape(), phase_ + phaseOffset01) * depthSmooth_;
    }

    // Advance and produce the two channel values in one call: the common case,
    // and the one that applies output smoothing so SQUARE and SAW do not click.
    void nextStereo (float& l, float& r) noexcept
    {
        advance();

        const int   sh  = shape();
        const float off = stereoOffset01();

        const float rawL = shapeAt (sh, phase_);
        const float rawR = shapeAt (sh, phase_ + off);

        outL_ = rawL + (outL_ - rawL) * outCoeff_;
        outR_ = rawR + (outR_ - rawR) * outCoeff_;

        l = outL_ * depthSmooth_;
        r = outR_ * depthSmooth_;
    }

    // Mono convenience: the left channel of nextStereo().
    float next() noexcept
    {
        float l = 0.0f, r = 0.0f;
        nextStereo (l, r);
        return l;
    }

    // Where the cycle currently is, 0..1. Used by editors to draw a moving dot.
    float currentPhase() const noexcept { return phase_; }

    // The depth the audio path is ACTUALLY using right now (post-smoothing).
    // Tremolo needs it: its gain law is defined in terms of depth, not just of
    // the LFO output, so reading the target instead would step when depth moves.
    float smoothedDepth01() const noexcept { return depthSmooth_; }

    // ---- pure helpers (what the g++ test pins) -----------------------------
    static int clampShape    (int s) noexcept { return std::clamp (s, 0, kNumShapes - 1); }
    static int clampDivision (int i) noexcept { return std::clamp (i, 0, kNumDivisions - 1); }

    // Fold any phase into [0, 1).
    static float wrap01 (float p) noexcept
    {
        p -= std::floor (p);
        return (p < 0.0f || p >= 1.0f) ? 0.0f : p;   // guards -0.0 and rounding
    }

    // Every shape bipolar (-1..+1), zero-mean, and rising through 0 at phase 0
    // so switching waveform does not shift the modulation's centre.
    static float shapeAt (int shapeIndex, float phase) noexcept
    {
        const float p = wrap01 (phase);

        switch (clampShape (shapeIndex))
        {
            case kTriangle:
                // 0 -> +1 -> 0 -> -1 -> 0, matching sine's zero-crossings.
                if (p < 0.25f) return 4.0f * p;
                if (p < 0.75f) return 2.0f - 4.0f * p;
                return 4.0f * p - 4.0f;

            case kSquare:
                return p < 0.5f ? 1.0f : -1.0f;

            case kSaw:
                // Centred rising ramp: -1 at phase 0.5, 0 at phase 0, +1 just
                // before phase 0.5. The naive 2p-1 would start at -1 and carry a
                // half-cycle of offset relative to every other shape.
                return 2.0f * wrap01 (p + 0.5f) - 1.0f;

            case kSine:
            default:
                return std::sin (p * 6.28318530717958647692f);
        }
    }

    // -1..+1 mapped to 0..1, for a device that wants a unipolar modulator.
    static float toUnipolar (float bipolar) noexcept { return 0.5f * (bipolar + 1.0f); }

private:
    int   shape() const noexcept { return shape_.load (std::memory_order_relaxed); }
    float depth01() const noexcept
    {
        return depthPct_.load (std::memory_order_relaxed) * 0.01f;
    }
    float stereoOffset01() const noexcept
    {
        return stereoDeg_.load (std::memory_order_relaxed) * (1.0f / 360.0f);
    }

    float phaseIncrement() const noexcept
    {
        return (float) ((double) effectiveRateHz() / sampleRate_);
    }

    void updateOutputCoeff() noexcept
    {
        const double ms = (double) smoothMs_.load (std::memory_order_relaxed);
        // 0 ms means "no smoothing": a coefficient of 0 keeps nothing of the old
        // value, which is the hard edge, rather than a division by zero.
        outCoeff_ = ms <= 0.0 ? 0.0f
                              : (float) std::exp (-1.0 / (ms * 0.001 * sampleRate_));
    }

    // ---- parameters ---------------------------------------------------------
    std::atomic<float>  rateHz_      { 1.0f };
    std::atomic<bool>   sync_        { false };
    std::atomic<int>    divisionIdx_ { kDefaultDivision };
    std::atomic<double> tempoBpm_    { 120.0 };   // stands in with no host tempo
    std::atomic<float>  depthPct_    { 50.0f };
    std::atomic<int>    shape_       { kSine };
    std::atomic<float>  stereoDeg_   { 0.0f };
    std::atomic<float>  smoothMs_    { 1.5f };

    // ---- audio-thread state (never touched from elsewhere) -----------------
    float  phase_       = 0.0f;
    float  incSmoothed_ = 0.0f;
    float  depthSmooth_ = 0.5f;
    float  outL_        = 0.0f;
    float  outR_        = 0.0f;

    float  paramCoeff_  = 0.0f;
    float  outCoeff_    = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
