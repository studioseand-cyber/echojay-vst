/*
    EedPhaserEngine.h  —  swept allpass cascade for "EchoJay Phaser".

    JUCE-free, header-only, g++-tested (test/mod_engines_test.cpp). No
    allocation anywhere: the cascade is a fixed-size array sized to kMaxStages.

    HOW IT MAKES NOTCHES. A first-order allpass passes every frequency at full
    level but rotates its phase, by 180 degrees across the spectrum with the
    half-way point at its corner frequency. Cascade N of them, sum the result with
    the dry signal, and everything that came out half a cycle late cancels: N/2
    notches, sitting wherever the cascade's corner currently is. Sweep the corner
    with the LFO and the notches sweep with it. That is the entire effect — there
    is no filter bank and nothing is being boosted.

    Three things this file has to be careful about:

    * THE CORNER MOVES IN OCTAVES, not in Hz. fc = centre * 2^(2 * lfo), so the
      sweep sounds the same width at 200 Hz as at 4 kHz. A linear-in-Hz sweep of
      the same size is inaudible low down and violent up top, because pitch is
      logarithmic and notches are heard as pitch.

    * THE COEFFICIENT IS RAMPED, not recomputed per sample. tan() per sample per
      channel is real CPU for a modulation effect, so the target is computed every
      16 samples and the coefficient walks to it linearly across the interval.
      Linear ramping rather than a one-pole because a one-pole LAGS, and a lagging
      corner at 20 Hz modulation shrinks the sweep the faster you set it — the
      knob would then mean two different things at two different rates.

    * WET IS 0.5 * (dry + cascade). The sum of two unity-gain paths is +6 dB
      wherever they agree, so halving keeps MIX at 100% level-matched with MIX at
      0% and the user hears the phasing rather than a volume jump.
*/

#pragma once

#include "EedLfoCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace echojay
{

// ---------------------------------------------------------------------------
// The phaser's CHARACTER (DEVICE_DEPTH_PLAN.md, Modulation depth pass). A
// spec applied on top of what the user dialled, not a separate signal path:
//
//   modern   nothing scaled — the full clean cascade, the device exactly as it
//            shipped, and the neutral default.
//   vintage  the cascade capped at 4 stages (two broad notches, like the
//            classic pedals), the feedback pushed ~30% hotter than the dial,
//            and a one-pole low-pass on the wet path so the swirl sits warm
//            and dark instead of glassy.
//   stereo   the right channel's sweep CENTRE lifted half an octave, so its
//            notches land between the left's and the sweep crosses the field
//            instead of riding it — over and above the stereo_spread phase
//            offset, which works in every mode.
// ---------------------------------------------------------------------------
enum class PhaserMode
{
    Modern  = 0,
    Vintage = 1,
    Stereo  = 2
};

constexpr int kNumPhaserModes = 3;

struct PhaserModeSpec
{
    int   maxStages;     // cap on the dialled cascade length
    float fbScale;       // multiplies the dialled feedback (result re-clamped)
    float wetLpHz;       // one-pole low-pass on the wet path; 0 = none
    float rCentreScale;  // multiplies the RIGHT channel's centre frequency
};

inline PhaserModeSpec phaserModeSpec (PhaserMode m) noexcept
{
    switch (m)
    {
        //                     stages  fb     wetLp    rCentre
        case PhaserMode::Vintage:
            return             { 4,    1.3f,  4500.0f, 1.0f };

        case PhaserMode::Stereo:
            return             { 12,   1.0f,  0.0f,    1.41421356f };   // +1/2 octave

        case PhaserMode::Modern:
        default:
            return             { 12,   1.0f,  0.0f,    1.0f };
    }
}

inline const char* phaserModeName (PhaserMode m) noexcept
{
    switch (m)
    {
        case PhaserMode::Vintage: return "vintage";
        case PhaserMode::Stereo:  return "stereo";
        case PhaserMode::Modern:
        default:                  return "modern";
    }
}

inline PhaserMode phaserModeFromIndex (int i) noexcept
{
    if (i <= 0) return PhaserMode::Modern;
    if (i >= kNumPhaserModes - 1) return PhaserMode::Stereo;
    return (PhaserMode) i;
}

class PhaserEngine
{
public:
    // Advertised ranges — the ParamSchema publishes these SAME numbers.
    static constexpr int   kMinStages   =    2;
    static constexpr int   kMaxStages   =   12;
    static constexpr float kMinCentreHz =  100.0f;
    static constexpr float kMaxCentreHz = 8000.0f;
    static constexpr float kMinFeedback =  -95.0f;   // % (negative = inverted)
    static constexpr float kMaxFeedback =   95.0f;
    static constexpr float kMinMix      =    0.0f;   // %
    static constexpr float kMaxMix      =  100.0f;

    // Constants of the algorithm, deliberately not knobs.
    static constexpr float kSweepOctaves   = 2.0f;
    static constexpr int   kCoeffInterval  = 16;      // samples between tan() calls

    // The depth pass turned the fixed 90-degree channel offset into the
    // stereo_spread param; this is its neutral default, kept as a named
    // constant so the schema and the engine agree by construction.
    static constexpr float kDefaultSpreadDeg = 90.0f;
    static constexpr float kMinSpreadDeg     = 0.0f;
    static constexpr float kMaxSpreadDeg     = 360.0f;

    PhaserEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        lfo_.prepare (sampleRate_);
        // The corner frequency is a filter coefficient; a stepped waveform in it
        // is a click, whatever a shape dial elsewhere might say.
        lfo_.setShape (LfoCore::kSine);

        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));
        reset();
    }

    void reset() noexcept
    {
        lfo_.reset();
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int s = 0; s < kMaxStages; ++s) z_[ch][s] = 0.0f;
            fbState_[ch] = 0.0f;
            wetLp_[ch]   = 0.0f;

            coeff_[ch]     = coeffFor (centreHzTarget_.load (std::memory_order_relaxed), sampleRate_);
            coeffStep_[ch] = 0.0f;
        }
        countdown_ = 0;

        centreHz_ = centreHzTarget_.load (std::memory_order_relaxed);
        feedback_ = feedbackTarget_.load (std::memory_order_relaxed) * 0.01f;
        mix_      = mixTarget_.load      (std::memory_order_relaxed) * 0.01f;
    }

    LfoCore&       lfo()       noexcept { return lfo_; }
    const LfoCore& lfo() const noexcept { return lfo_; }

    // ---- parameters (message thread) ---------------------------------------
    void setStages (int n) noexcept
    {
        stages_.store (std::clamp (n, kMinStages, kMaxStages), std::memory_order_relaxed);
    }
    void setCentreHz (float hz) noexcept
    {
        centreHzTarget_.store (std::clamp (hz, kMinCentreHz, kMaxCentreHz),
                               std::memory_order_relaxed);
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
    void setMode (PhaserMode m) noexcept
    {
        mode_.store ((int) m, std::memory_order_relaxed);
    }
    void setStereoSpreadDeg (float deg) noexcept
    {
        spreadDeg_.store (std::clamp (deg, kMinSpreadDeg, kMaxSpreadDeg),
                          std::memory_order_relaxed);
    }

    int   getStages()          const noexcept { return stages_.load          (std::memory_order_relaxed); }
    float getCentreHz()        const noexcept { return centreHzTarget_.load  (std::memory_order_relaxed); }
    float getFeedbackPercent() const noexcept { return feedbackTarget_.load  (std::memory_order_relaxed); }
    float getMixPercent()      const noexcept { return mixTarget_.load       (std::memory_order_relaxed); }
    float getStereoSpreadDeg() const noexcept { return spreadDeg_.load       (std::memory_order_relaxed); }
    PhaserMode getMode() const noexcept
    {
        return phaserModeFromIndex (mode_.load (std::memory_order_relaxed));
    }

    // The cascade length actually running: the mode may cap the dial. Public
    // because it is what the editor's hint and notch picture must report.
    int effectiveStages() const noexcept
    {
        return std::min (getStages(), phaserModeSpec (getMode()).maxStages);
    }

    // ---- audio thread ------------------------------------------------------
    void process (float* left, float* right, int numSamples) noexcept
    {
        const auto spec    = phaserModeSpec (getMode());
        const int  stages  = std::min (stages_.load (std::memory_order_relaxed),
                                       spec.maxStages);
        const float cTgt   = centreHzTarget_.load  (std::memory_order_relaxed);
        const float fTgt   = feedbackTarget_.load  (std::memory_order_relaxed) * 0.01f;
        const float mTgt   = mixTarget_.load       (std::memory_order_relaxed) * 0.01f;

        // The mode's feedback push is applied to the SMOOTHED value and then
        // re-clamped, so vintage can run hotter than the dial without ever
        // crossing the same 95% ceiling the dial itself enforces.
        const float fbScale = spec.fbScale;

        // The wet low-pass coefficient depends only on the mode, so one exp()
        // per block covers it; 0 Hz means the filter is SKIPPED, not run at a
        // neutral setting — modern must stay bit-identical to what shipped.
        wetLpCoeff_ = spec.wetLpHz <= 0.0f ? 0.0f
                    : (float) (1.0 - std::exp (-6.28318530717958647692 * spec.wetLpHz
                                               / sampleRate_));

        for (int i = 0; i < numSamples; ++i)
        {
            lfo_.advance();

            centreHz_ = cTgt + (centreHz_ - cTgt) * smoothCoeff_;
            feedback_ = fTgt + (feedback_ - fTgt) * smoothCoeff_;
            mix_      = mTgt + (mix_      - mTgt) * smoothCoeff_;

            if (countdown_ <= 0)
            {
                retargetCoeffs (right != nullptr, spec.rCentreScale);
                countdown_ = kCoeffInterval;
            }
            --countdown_;

            const float fb = std::clamp (feedback_ * fbScale, -0.95f, 0.95f);

            coeff_[0] += coeffStep_[0];
            left[i] = processChannel (0, left[i], stages, fb);

            if (right != nullptr)
            {
                coeff_[1] += coeffStep_[1];
                right[i] = processChannel (1, right[i], stages, fb);
            }
        }
    }

    // ---- pure helpers (what the g++ test pins) -----------------------------
    // First-order allpass coefficient for the difference equation used below:
    //   y[n] = -a*x[n] + x[n-1] + a*y[n-1]      =>  H(z) = (-a + z^-1)/(1 - a z^-1)
    // with a = (1 - tan(pi*fc/fs)) / (1 + tan(pi*fc/fs)), which puts the
    // half-way point of the phase rotation at fc and keeps |a| < 1 for any fc
    // strictly inside the band.
    static float coeffFor (float centreHz, double sampleRate) noexcept
    {
        const float nyquistLimit = (float) (0.45 * sampleRate);
        const float fc = std::clamp (centreHz, 20.0f, std::max (20.0f, nyquistLimit));
        const float t  = std::tan (3.14159265358979323846f * fc / (float) sampleRate);
        return (1.0f - t) / (1.0f + t);
    }

    // Where the corner sits for a given LFO value: two octaves either side of
    // centre at full depth, logarithmic so the sweep sounds the same width
    // wherever centre is parked.
    static float sweptHz (float centreHz, float lfoValue) noexcept
    {
        return centreHz * std::pow (2.0f, kSweepOctaves * lfoValue);
    }

private:
    void retargetCoeffs (bool stereo, float rCentreScale) noexcept
    {
        const float targetL = coeffFor (sweptHz (centreHz_, lfo_.valueAt (0.0f)), sampleRate_);
        coeffStep_[0] = (targetL - coeff_[0]) * (1.0f / (float) kCoeffInterval);

        if (stereo)
        {
            const float off = spreadDeg_.load (std::memory_order_relaxed) * (1.0f / 360.0f);
            const float targetR = coeffFor (sweptHz (centreHz_ * rCentreScale,
                                                     lfo_.valueAt (off)),
                                            sampleRate_);
            coeffStep_[1] = (targetR - coeff_[1]) * (1.0f / (float) kCoeffInterval);
        }
    }

    float processChannel (int ch, float in, int stages, float fb) noexcept
    {
        const float a = coeff_[ch];

        // Bound what re-enters the cascade: at 95% feedback the loop is close to
        // self-oscillating by design, and clamping the state is what makes the
        // top of the range safe to actually reach for.
        float y = std::clamp (in + fb * fbState_[ch], -4.0f, 4.0f);

        for (int s = 0; s < stages; ++s)
        {
            const float x  = y;
            const float out = -a * x + z_[ch][s];
            z_[ch][s] = x + a * out;
            y = out;
        }

        fbState_[ch] = y;

        // Vintage's warmth: the cascade's output rolled off before it meets
        // the dry path. Skipped entirely at 0 rather than run flat — an extra
        // one-pole in the path would not be bit-transparent, and modern has to
        // be the device exactly as it shipped.
        if (wetLpCoeff_ > 0.0f)
        {
            wetLp_[ch] += wetLpCoeff_ * (y - wetLp_[ch]);
            y = wetLp_[ch];
        }

        // Halved: two unity paths summing would be +6 dB where they agree, and a
        // MIX knob that changes level is a MIX knob nobody trusts.
        const float wet = 0.5f * (in + y);
        return in + mix_ * (wet - in);
    }

    LfoCore lfo_;

    std::atomic<int>   stages_         { 6 };
    std::atomic<float> centreHzTarget_ { 800.0f };
    std::atomic<float> feedbackTarget_ { 30.0f };
    std::atomic<float> mixTarget_      { 100.0f };
    std::atomic<int>   mode_           { (int) PhaserMode::Modern };
    std::atomic<float> spreadDeg_      { kDefaultSpreadDeg };

    // Audio-thread state only.
    float  z_[2][kMaxStages] {};
    float  coeff_[2]     { 0.0f, 0.0f };
    float  coeffStep_[2] { 0.0f, 0.0f };
    float  fbState_[2]   { 0.0f, 0.0f };
    float  wetLp_[2]     { 0.0f, 0.0f };
    float  wetLpCoeff_   = 0.0f;
    int    countdown_    = 0;

    float  centreHz_    = 800.0f;
    float  feedback_    = 0.3f;
    float  mix_         = 1.0f;
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
