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

// ---------------------------------------------------------------------------
// Which gain stage the device runs (the `mode` param of "EchoJay Gain").
//   Stereo  — level + pan, the device as it shipped.
//   MidSide — an extra stage BEFORE level/pan: independent gain on the mid
//             (centre) and side (stereo difference). Level and pan still apply
//             after it, so the two modes compose rather than replace.
// ---------------------------------------------------------------------------
enum class GainMode
{
    Stereo  = 0,
    MidSide = 1,
};

constexpr int kNumGainModes = 2;

inline const char* gainModeName (GainMode m) noexcept
{
    return m == GainMode::MidSide ? "mid_side" : "stereo";
}

inline GainMode gainModeFromIndex (int i) noexcept
{
    return i == 1 ? GainMode::MidSide : GainMode::Stereo;
}

class GainEngine
{
public:
    // Advertised ranges. The SAME numbers the ParamSchema publishes — the schema
    // is the contract the model and server see, this is what enforces it in DSP.
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb =  24.0f;

    // The M/S trims cut to the same -60 floor (that is what buys "remove the
    // centre" and "collapse to mono") but BOOST only to +6: they compose with
    // level_db, and 24 + 24 stacked is a +48 dB device, which no utility
    // should be. +6 on the side is exactly the 200% ceiling Stereo Width
    // advertises, so the two devices agree about how wide "as wide as it goes"
    // is.
    static constexpr float kMaxMsDb = 6.0f;

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

    // ---- the depth pass ----------------------------------------------------
    void setMode (GainMode m) noexcept
    {
        mode_.store ((int) m, std::memory_order_relaxed);
    }

    void setMidDb (float db) noexcept
    {
        midDbTarget_.store (std::clamp (db, kMinDb, kMaxMsDb),
                            std::memory_order_relaxed);
    }

    void setSideDb (float db) noexcept
    {
        sideDbTarget_.store (std::clamp (db, kMinDb, kMaxMsDb),
                             std::memory_order_relaxed);
    }

    // Sum to mono. Implemented as "fade the side to zero" rather than a hard
    // L=R=M switch, so toggling it mid-play is a collapse, not a click.
    void setMono (bool on) noexcept
    {
        mono_.store (on, std::memory_order_relaxed);
    }

    // Polarity, per channel. The target is +/-1 and it goes through the same
    // smoother as every gain, so a flip is a fast fade through zero rather than
    // a discontinuity.
    void setPhaseLeft  (bool inverted) noexcept { phaseLeft_.store  (inverted, std::memory_order_relaxed); }
    void setPhaseRight (bool inverted) noexcept { phaseRight_.store (inverted, std::memory_order_relaxed); }

    GainMode getMode()   const noexcept { return gainModeFromIndex (mode_.load (std::memory_order_relaxed)); }
    float getMidDb()     const noexcept { return midDbTarget_.load  (std::memory_order_relaxed); }
    float getSideDb()    const noexcept { return sideDbTarget_.load (std::memory_order_relaxed); }
    bool  getMono()      const noexcept { return mono_.load       (std::memory_order_relaxed); }
    bool  getPhaseLeft() const noexcept { return phaseLeft_.load  (std::memory_order_relaxed); }
    bool  getPhaseRight()const noexcept { return phaseRight_.load (std::memory_order_relaxed); }

    // ---- audio thread ------------------------------------------------------
    // In-place stereo process. `right` may be null for mono; pan, the M/S stage
    // and the mono sum are then IGNORED rather than approximated — there is no
    // stereo field to work on — while the polarity flip and the level, which
    // are properties of one wire, still apply.
    //
    // Signal order for stereo: polarity -> M/S gains -> mono sum -> level ->
    // pan. Polarity runs FIRST so that flip-one-side-and-sum — the classic
    // phase check — actually cancels; level and pan run last so the two modes
    // compose instead of replacing each other.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float targetGain = dbToGain (levelDbTarget_.load (std::memory_order_relaxed));
        const float targetPan  = panTarget_.load (std::memory_order_relaxed);

        // In stereo mode the M/S gains TARGET unity rather than being skipped:
        // switching modes then fades the stage out through the same smoother
        // that faded it in, instead of stepping.
        const bool  midSide     = gainModeFromIndex (mode_.load (std::memory_order_relaxed)) == GainMode::MidSide;
        const float targetMid   = midSide ? dbToGain (midDbTarget_.load  (std::memory_order_relaxed)) : 1.0f;
        const float targetSide  = midSide ? dbToGain (sideDbTarget_.load (std::memory_order_relaxed)) : 1.0f;
        const float targetMono  = mono_.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
        const float targetPolL  = phaseLeft_.load  (std::memory_order_relaxed) ? -1.0f : 1.0f;
        const float targetPolR  = phaseRight_.load (std::memory_order_relaxed) ? -1.0f : 1.0f;

        // With every depth stage at rest (targets AND smoothers at unity), run
        // the exact loop the device shipped with. This is not an optimisation
        // so much as a promise: an existing session that never touches the new
        // params gets bit-identical output, not output within an M/S encode-
        // decode rounding of it.
        const bool depthIdle = targetMid  == 1.0f && midG_    == 1.0f
                            && targetSide == 1.0f && sideG_   == 1.0f
                            && targetMono == 0.0f && monoAmt_ == 0.0f
                            && targetPolL == 1.0f && polL_    == 1.0f
                            && targetPolR == 1.0f && polR_    == 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            // One-pole toward the target: coeff is the fraction of the remaining
            // distance kept each sample.
            gain_ = targetGain + (gain_ - targetGain) * smoothCoeff_;
            pan_  = targetPan  + (pan_  - targetPan)  * smoothCoeff_;

            if (right == nullptr)
            {
                polL_ = targetPolL + (polL_ - targetPolL) * smoothCoeff_;
                left[i] *= gain_ * polL_;
                continue;
            }

            if (depthIdle)
            {
                float gl, gr;
                panGains (pan_, gl, gr);
                left[i]  *= gain_ * gl;
                right[i] *= gain_ * gr;
                continue;
            }

            midG_    = targetMid  + (midG_    - targetMid)  * smoothCoeff_;
            sideG_   = targetSide + (sideG_   - targetSide) * smoothCoeff_;
            monoAmt_ = targetMono + (monoAmt_ - targetMono) * smoothCoeff_;
            polL_    = targetPolL + (polL_    - targetPolL) * smoothCoeff_;
            polR_    = targetPolR + (polR_    - targetPolR) * smoothCoeff_;

            float l = left[i]  * polL_;
            float r = right[i] * polR_;

            float m = 0.5f * (l + r);
            float s = 0.5f * (l - r);

            m *= midG_;
            s *= sideG_ * (1.0f - monoAmt_);

            l = m + s;
            r = m - s;

            float gl, gr;
            panGains (pan_, gl, gr);
            left[i]  = l * gain_ * gl;
            right[i] = r * gain_ * gr;
        }

        if (right == nullptr)
        {
            // Keep the stereo-only smoothers tracking, so switching back to a
            // stereo bus does not then glide from stale values.
            midG_ = targetMid; sideG_ = targetSide; monoAmt_ = targetMono;
            polR_ = targetPolR;
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

        const bool midSide = gainModeFromIndex (mode_.load (std::memory_order_relaxed)) == GainMode::MidSide;
        midG_    = midSide ? dbToGain (midDbTarget_.load  (std::memory_order_relaxed)) : 1.0f;
        sideG_   = midSide ? dbToGain (sideDbTarget_.load (std::memory_order_relaxed)) : 1.0f;
        monoAmt_ = mono_.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
        polL_    = phaseLeft_.load  (std::memory_order_relaxed) ? -1.0f : 1.0f;
        polR_    = phaseRight_.load (std::memory_order_relaxed) ? -1.0f : 1.0f;
    }

    std::atomic<float> levelDbTarget_ { 0.0f };
    std::atomic<float> panTarget_     { 0.0f };

    // The depth pass. Mode stored as an int, same as the rest of the suite.
    std::atomic<int>   mode_         { (int) GainMode::Stereo };
    std::atomic<float> midDbTarget_  { 0.0f };
    std::atomic<float> sideDbTarget_ { 0.0f };
    std::atomic<bool>  mono_         { false };
    std::atomic<bool>  phaseLeft_    { false };
    std::atomic<bool>  phaseRight_   { false };

    // Smoothed state — audio thread only, never touched from elsewhere.
    float  gain_        = 1.0f;
    float  pan_         = 0.0f;
    float  midG_        = 1.0f;
    float  sideG_       = 1.0f;
    float  monoAmt_     = 0.0f;
    float  polL_        = 1.0f;
    float  polR_        = 1.0f;
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
