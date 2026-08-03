/*
    EedAutoPanEngine.h  —  LFO-steered stereo placement for "EchoJay Auto Pan".

    JUCE-free, g++-tested (test/mod_engines_test.cpp).

    The default pan law is GainEngine's, included rather than re-derived:
    constant power, normalised so CENTRE IS UNITY. That is what makes depth 0 a
    bit-identical pass-through, and it is the whole reason to share the function
    instead of writing a second one that is 3 dB down in the middle.

    WHAT STEREO PHASE DOES HERE, because it is the one non-obvious choice. Each
    input channel is steered by the LFO at its own phase: the left channel by the
    left tap, the right by the right tap, keeping the gain that channel would get
    at that pan position. So

      * 0 degrees  — both taps agree and this is an ordinary auto-pan: the whole
                     image swings left and right together.
      * 90 degrees — the channels sweep out of step and the image rotates rather
                     than sliding, which is the wide "rotary" version.
      * 180 degrees— the taps mirror, so both channels dip and swell together and
                     it becomes a tremolo. Musically real, and reached by turning
                     one knob rather than by a hidden mode switch.

    The alternative — cross-feeding both inputs through a single pan position —
    sums to mono at depth 0, which fails the "a device at its defaults is
    pass-through" rule that every EchoJay utility keeps.

    THE DEPTH PASS added a pan-law MODE and a WIDTH scale:

      * constant_power — the law above, the device as it shipped, and the
        neutral default.
      * linear — a straight crossfade with centre still at unity: the far
        channel just fades out. Dips very slightly in the middle of a sweep,
        which is the older, plainer auto-pan sound some material wants.
      * binaural — constant-power gains PLUS a small inter-aural delay on the
        ear the image is moving away from, up to kMaxItdMs at the extremes.
        Level panning tells the ear "quieter on one side"; the delay is the
        other half of how humans localise, so the image reads as PLACED rather
        than as faded. The delay follows the (already smoothed) LFO, so its
        motion doubles as a whisper of doppler — that is the "more convincing"
        part, not an artefact.
      * width scales the pan position itself, so it bounds how far the FIELD
        extends; depth is how much of that field each sweep uses. 100 is the
        full field and the neutral default.
*/

#pragma once

#include "EedGainEngine.h"    // GainEngine::panGains — the shared pan law
#include "EedLfoCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
// The pan law (DEVICE_DEPTH_PLAN.md, Modulation depth pass).
// ---------------------------------------------------------------------------
enum class AutoPanMode
{
    Linear        = 0,
    ConstantPower = 1,
    Binaural      = 2
};

constexpr int kNumAutoPanModes = 3;

inline const char* autoPanModeName (AutoPanMode m) noexcept
{
    switch (m)
    {
        case AutoPanMode::Linear:   return "linear";
        case AutoPanMode::Binaural: return "binaural";
        case AutoPanMode::ConstantPower:
        default:                    return "constant_power";
    }
}

inline AutoPanMode autoPanModeFromIndex (int i) noexcept
{
    if (i <= 0) return AutoPanMode::Linear;
    if (i >= kNumAutoPanModes - 1) return AutoPanMode::Binaural;
    return (AutoPanMode) i;
}

class AutoPanEngine
{
public:
    // Advertised ranges — the ParamSchema publishes these SAME numbers.
    static constexpr float kMinWidthPct = 0.0f;
    static constexpr float kMaxWidthPct = 100.0f;

    // The largest inter-aural delay a head produces is ~0.65 ms; binaural mode
    // reaches just under that at a hard extreme. A constant of the algorithm,
    // deliberately not a knob.
    static constexpr float kMaxItdMs = 0.6f;

    AutoPanEngine() = default;

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        lfo_.prepare (sampleRate_);

        constexpr double tau = 0.020;
        smoothCoeff_ = (float) std::exp (-1.0 / (tau * sampleRate_));

        // The binaural delay lines: a millisecond and change per channel.
        // Allocated here (never on the audio thread) whatever the mode is, so
        // switching TO binaural mid-flight touches no memory.
        const int n = (int) std::ceil (kMaxItdMs * 0.001 * sampleRate_) + 8;
        for (auto& d : itd_) { d.buf.assign ((size_t) n, 0.0f); d.size = n; d.write = 0; }

        reset();
    }

    void reset() noexcept
    {
        lfo_.reset();
        width_ = widthTarget_.load (std::memory_order_relaxed) * 0.01f;
        for (auto& d : itd_)
        {
            std::fill (d.buf.begin(), d.buf.end(), 0.0f);
            d.write = 0;
        }
    }

    LfoCore&       lfo()       noexcept { return lfo_; }
    const LfoCore& lfo() const noexcept { return lfo_; }

    // ---- parameters (message thread) ---------------------------------------
    void setMode (AutoPanMode m) noexcept { mode_.store ((int) m, std::memory_order_relaxed); }
    AutoPanMode getMode() const noexcept
    {
        return autoPanModeFromIndex (mode_.load (std::memory_order_relaxed));
    }

    void setWidthPercent (float pct) noexcept
    {
        widthTarget_.store (std::clamp (pct, kMinWidthPct, kMaxWidthPct),
                            std::memory_order_relaxed);
    }
    float getWidthPercent() const noexcept
    {
        return widthTarget_.load (std::memory_order_relaxed);
    }

    // ---- audio thread ------------------------------------------------------
    // In-place. `right` may be null: a mono signal has no stereo field to be
    // placed in, so it passes through untouched rather than being amplitude-
    // modulated by half a pan law — the same call GainEngine makes for its pan.
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (right == nullptr) return;

        const auto  mode = getMode();
        const float wTgt = widthTarget_.load (std::memory_order_relaxed) * 0.01f;

        for (int i = 0; i < numSamples; ++i)
        {
            float posL = 0.0f, posR = 0.0f;
            lfo_.nextStereo (posL, posR);        // already -depth..+depth, i.e. a
                                                 // pan position in -1..+1

            width_ = wTgt + (width_ - wTgt) * smoothCoeff_;
            posL *= width_;
            posR *= width_;

            float glL = 0.0f, grL = 0.0f;
            float glR = 0.0f, grR = 0.0f;

            if (mode == AutoPanMode::Linear)
            {
                panGainsLinear (posL, glL, grL);
                panGainsLinear (posR, glR, grR);
            }
            else
            {
                GainEngine::panGains (posL, glL, grL);
                GainEngine::panGains (posR, glR, grR);
            }

            if (mode == AutoPanMode::Binaural && itd_[0].size > 0)
            {
                // The ear the image moves AWAY from hears it later: panned
                // right, the LEFT channel is delayed, and vice versa. The
                // delay rides the smoothed LFO, so it never steps.
                itd_[0].push (left[i]);
                itd_[1].push (right[i]);

                const float toSamples = (float) (0.001 * sampleRate_) * kMaxItdMs;
                const float dL = std::max (0.0f,  posL) * toSamples;
                const float dR = std::max (0.0f, -posR) * toSamples;

                left[i]  = itd_[0].read (dL) * glL;
                right[i] = itd_[1].read (dR) * grR;
            }
            else
            {
                left[i]  *= glL;     // left channel's own gain at its own position
                right[i] *= grR;     // right channel's, at its own
            }
        }
    }

    // ---- pure helper (what the g++ test pins) ------------------------------
    // Straight crossfade, centre at UNITY like the constant-power law, so the
    // two modes agree everywhere the "pass-through at rest" contract bites:
    // centre is untouched in both, the extremes silence the far channel in
    // both, and only the middle of the sweep differs (linear dips, constant
    // power holds).
    static void panGainsLinear (float pan, float& gl, float& gr) noexcept
    {
        const float p = std::clamp (pan, -1.0f, 1.0f);
        gl = p > 0.0f ? 1.0f - p : 1.0f;
        gr = p < 0.0f ? 1.0f + p : 1.0f;
    }

private:
    // A minimal fractional delay for the ITD: sub-millisecond, linear
    // interpolation, and — unlike the chorus line — a read of 0 means "the
    // sample just pushed", so an undelayed ear is genuinely undelayed.
    struct ItdLine
    {
        std::vector<float> buf;
        int size  = 0;
        int write = 0;

        void push (float x) noexcept
        {
            buf[(size_t) write] = x;
            if (++write >= size) write = 0;
        }
        float read (float delaySamples) const noexcept
        {
            const float d = std::clamp (delaySamples, 0.0f, (float) (size - 2));
            float rp = (float) write - 1.0f - d;
            while (rp < 0.0f) rp += (float) size;

            const int i0 = (int) rp;
            const float frac = rp - (float) i0;
            int i1 = i0 + 1;
            if (i1 >= size) i1 -= size;

            return buf[(size_t) i0] + frac * (buf[(size_t) i1] - buf[(size_t) i0]);
        }
    };

    LfoCore lfo_;
    ItdLine itd_[2];

    std::atomic<int>   mode_        { (int) AutoPanMode::ConstantPower };
    std::atomic<float> widthTarget_ { 100.0f };

    float  width_       = 1.0f;
    float  smoothCoeff_ = 0.0f;
    double sampleRate_  = 44100.0;
};

} // namespace echojay
