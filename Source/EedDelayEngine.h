/*
    EedDelayEngine.h  —  the DSP behind "EchoJay Delay".

    A stereo delay assembled entirely out of EedDelayCore.h: two fractional
    delay lines, a LoopFilter in each feedback path, one LFO driving both, and a
    Smoother per side gliding the read position.

    JUCE-free, with a g++ test (test/delay_engine_test.cpp). Header-only, same as
    GainEngine — the whole thing is one per-sample loop and inlining it matters.

    The decisions that are not obvious:

    * TIME GLIDES, IT DOES NOT JUMP. The read position is smoothed with a ~60 ms
      one-pole, so changing the time slides the pitch of whatever is already in
      the line the way a tape delay does. Snapping it instead produces a click on
      every move — including every AI move, which is the common case here.

    * PING-PONG SUMS TO MONO. The classic bounce needs the first echo to land
      hard on ONE side; feeding both lines from their own channel gives two
      independent delays that merely happen to cross-feed, which does not bounce.
      So in ping-pong the input is summed and injected into the LEFT line only,
      and the feedback crosses. Off, each side is fed from its own channel and
      feeds back into itself.

    * STEREO OFFSET IS A PERCENTAGE, NOT MILLISECONDS. Offsetting the right side
      by a fixed number of ms is a huge musical difference at a 60 ms slap and
      nothing at a 2-second wash. As a percentage of the time it means the same
      thing at every setting, and it survives a tempo change.

    * FEEDBACK IS SOFT-CLIPPED, SO 100% IS SAFE. The loop gain is strictly under
      1 for any non-zero signal (see softClip in EedDelayCore.h), so the maximum
      setting is an extremely long tail rather than a divergence. That is what
      lets the schema advertise a full 0..100 range honestly: every value in it
      is a value the device actually survives.

    * SYNC READS A PUBLISHED TEMPO, AND ALWAYS HAS ONE. See EedTempoClock.h. A
      device that cannot find a host tempo runs at 120 rather than at zero.

    Real-time contract: process() never allocates, locks or blocks. Parameters
    are plain atomics, read once at the top of a block.
*/

#pragma once

#include "EedDelayCore.h"
#include "EedTempoClock.h"

#include <atomic>

namespace echojay
{

class DelayEngine
{
public:
    // ---- advertised ranges (the SAME numbers the ParamSchema publishes) -----
    static constexpr float kMinTimeMs      = 1.0f;
    static constexpr float kMaxTimeMs      = 4000.0f;
    static constexpr float kMinFeedbackPct = 0.0f;
    static constexpr float kMaxFeedbackPct = 100.0f;
    static constexpr float kMinMixPct      = 0.0f;
    static constexpr float kMaxMixPct      = 100.0f;
    static constexpr float kMinHpHz        = 20.0f;
    static constexpr float kMaxHpHz        = 2000.0f;
    static constexpr float kMinLpHz        = 200.0f;
    static constexpr float kMaxLpHz        = 20000.0f;
    static constexpr float kMinOffsetPct   = -50.0f;
    static constexpr float kMaxOffsetPct   = 50.0f;
    static constexpr float kMinModRateHz   = 0.01f;
    static constexpr float kMaxModRateHz   = 10.0f;
    static constexpr float kMinModDepthMs  = 0.0f;
    static constexpr float kMaxModDepthMs  = 20.0f;

    // ---- tempo divisions ---------------------------------------------------
    // Beats, where a quarter note is 1. Ordered SHORTEST FIRST so the index rises
    // with the delay time: turning the selector up makes the echo longer, which
    // is the only ordering a user (or a model reasoning about "a bit longer")
    // can predict.
    struct Division { const char* name; double beats; };

    static constexpr int kNumDivisions = 15;

    static const Division* divisions() noexcept
    {
        static const Division d[kNumDivisions] = {
            { "1/32",  0.125          },
            { "1/16T", 1.0 / 6.0      },
            { "1/32D", 0.1875         },
            { "1/16",  0.25           },
            { "1/8T",  1.0 / 3.0      },
            { "1/16D", 0.375          },
            { "1/8",   0.5            },
            { "1/4T",  2.0 / 3.0      },
            { "1/8D",  0.75           },
            { "1/4",   1.0            },
            { "1/2T",  4.0 / 3.0      },
            { "1/4D",  1.5            },
            { "1/2",   2.0            },
            { "1/2D",  3.0            },
            { "1/1",   4.0            },
        };
        return d;
    }

    static int clampDivision (int i) noexcept
    {
        return i < 0 ? 0 : (i >= kNumDivisions ? kNumDivisions - 1 : i);
    }

    static const char* divisionName (int i) noexcept { return divisions()[clampDivision (i)].name; }
    static double      divisionBeats (int i) noexcept { return divisions()[clampDivision (i)].beats; }

    // Note length -> milliseconds. Public and static because the EDITOR shows the
    // resulting time next to the division, and it must be the same arithmetic the
    // audio path uses or the readout lies.
    static double syncedMs (int divisionIndex, double bpm) noexcept
    {
        const double b = (bpm >= kMinTempoBpm && bpm <= kMaxTempoBpm) ? bpm : kDefaultTempoBpm;
        return divisionBeats (divisionIndex) * 60000.0 / b;
    }

    DelayEngine() = default;

    // ---- lifecycle ---------------------------------------------------------
    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

        // Headroom over the longest advertised time: the stereo offset can add
        // 50%, and the modulation another 20 ms on top of that.
        const double maxSeconds = (double) kMaxTimeMs * 1.5 / 1000.0
                                + (double) kMaxModDepthMs / 1000.0 + 0.05;

        lineL_.prepare (sampleRate_, maxSeconds);
        lineR_.prepare (sampleRate_, maxSeconds);

        timeL_.prepare (sampleRate_, 0.060);
        timeR_.prepare (sampleRate_, 0.060);
        fbSmooth_.prepare (sampleRate_, 0.020);
        mixSmooth_.prepare (sampleRate_, 0.020);

        lfo_.prepare (sampleRate_);

        refreshFilters();
        reset();
    }

    // Jump to the current targets and empty the lines. A restored session then
    // starts AT its settings rather than sweeping into them from wherever the
    // previous instance was.
    void reset() noexcept
    {
        lineL_.reset();
        lineR_.reset();
        filtL_.reset();
        filtR_.reset();
        lfo_.reset (0.0);

        double dl = 0.0, dr = 0.0;
        currentDelaySamples (dl, dr);
        timeL_.snap ((float) dl);
        timeR_.snap ((float) dr);
        fbSmooth_.snap (feedbackGain());
        mixSmooth_.snap (mixPct_.load (std::memory_order_relaxed) * 0.01f);
    }

    // ---- parameters (message thread) ---------------------------------------
    void setTimeMs (float ms) noexcept          { timeMs_.store (clampf (ms, kMinTimeMs, kMaxTimeMs), std::memory_order_relaxed); }
    void setSync (bool on) noexcept             { sync_.store (on, std::memory_order_relaxed); }
    void setDivision (int i) noexcept           { division_.store (clampDivision (i), std::memory_order_relaxed); }
    void setFeedbackPct (float p) noexcept      { feedbackPct_.store (clampf (p, kMinFeedbackPct, kMaxFeedbackPct), std::memory_order_relaxed); }
    void setMixPct (float p) noexcept           { mixPct_.store (clampf (p, kMinMixPct, kMaxMixPct), std::memory_order_relaxed); }
    void setPingPong (bool on) noexcept         { pingPong_.store (on, std::memory_order_relaxed); }
    void setStereoOffsetPct (float p) noexcept  { offsetPct_.store (clampf (p, kMinOffsetPct, kMaxOffsetPct), std::memory_order_relaxed); }
    void setModRateHz (float hz) noexcept       { modRateHz_.store (clampf (hz, kMinModRateHz, kMaxModRateHz), std::memory_order_relaxed); }
    void setModDepthMs (float ms) noexcept      { modDepthMs_.store (clampf (ms, kMinModDepthMs, kMaxModDepthMs), std::memory_order_relaxed); }

    // The filters are the one pair that has to be recomputed rather than just
    // stored, so they are not simple stores.
    void setHighpassHz (float hz) noexcept
    {
        hpHz_.store (clampf (hz, kMinHpHz, kMaxHpHz), std::memory_order_relaxed);
        filtersDirty_.store (true, std::memory_order_relaxed);
    }

    void setLowpassHz (float hz) noexcept
    {
        lpHz_.store (clampf (hz, kMinLpHz, kMaxLpHz), std::memory_order_relaxed);
        filtersDirty_.store (true, std::memory_order_relaxed);
    }

    float getTimeMs()          const noexcept { return timeMs_.load (std::memory_order_relaxed); }
    bool  getSync()            const noexcept { return sync_.load (std::memory_order_relaxed); }
    int   getDivision()        const noexcept { return division_.load (std::memory_order_relaxed); }
    float getFeedbackPct()     const noexcept { return feedbackPct_.load (std::memory_order_relaxed); }
    float getMixPct()          const noexcept { return mixPct_.load (std::memory_order_relaxed); }
    float getHighpassHz()      const noexcept { return hpHz_.load (std::memory_order_relaxed); }
    float getLowpassHz()       const noexcept { return lpHz_.load (std::memory_order_relaxed); }
    bool  getPingPong()        const noexcept { return pingPong_.load (std::memory_order_relaxed); }
    float getStereoOffsetPct() const noexcept { return offsetPct_.load (std::memory_order_relaxed); }
    float getModRateHz()       const noexcept { return modRateHz_.load (std::memory_order_relaxed); }
    float getModDepthMs()      const noexcept { return modDepthMs_.load (std::memory_order_relaxed); }

    // The time actually in force, in ms — free-running or synced. This is what
    // the editor puts under the TIME dial when SYNC is on, and it is derived
    // here so the readout can never disagree with the audio.
    double effectiveTimeMs() const noexcept
    {
        return getSync() ? syncedMs (getDivision(), hostTempoBpm())
                         : (double) getTimeMs();
    }

    // ---- audio thread ------------------------------------------------------
    // In-place stereo. `right` may be null for mono, in which case ping-pong and
    // stereo offset have nothing to act on and are ignored rather than folded
    // into the one channel — a mono delay that quietly halves its time because a
    // stereo control was set is worse than one that ignores it.
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (left == nullptr || numSamples <= 0) return;

        if (filtersDirty_.exchange (false, std::memory_order_relaxed))
            refreshFilters();

        // Read every parameter ONCE per block. A value changing mid-block is a
        // discontinuity in the middle of a loop that is already feeding itself.
        const bool  stereo   = (right != nullptr);
        const bool  pingPong = stereo && pingPong_.load (std::memory_order_relaxed);
        const float modDepth = modDepthMs_.load (std::memory_order_relaxed)
                             * (float) sampleRate_ * 0.001f;

        lfo_.setRateHz (modRateHz_.load (std::memory_order_relaxed));

        double targetL = 0.0, targetR = 0.0;
        currentDelaySamples (targetL, targetR);
        timeL_.setTarget ((float) targetL);
        timeR_.setTarget ((float) targetR);

        fbSmooth_.setTarget (feedbackGain());
        mixSmooth_.setTarget (mixPct_.load (std::memory_order_relaxed) * 0.01f);

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = left[i];
            const float inR = stereo ? right[i] : 0.0f;

            const float fb  = fbSmooth_.next();
            const float mix = mixSmooth_.next();

            lfo_.advance();
            const double dL = (double) timeL_.next() + (double) (modDepth * lfo_.sine());
            const double dR = (double) timeR_.next() + (double) (modDepth * lfo_.quadrature());

            // READ BEFORE WRITE — the loop's delay is exactly what these reads say.
            const float wetL = lineL_.readCubic (dL);
            const float wetR = stereo ? lineR_.readCubic (dR) : 0.0f;

            // Filter and limit the FED-BACK copy only. The dry output tap above
            // is untouched, so the tone control shapes successive repeats
            // progressively instead of colouring the first one.
            const float fedL = softClip (filtL_.process (wetL) * fb);
            const float fedR = stereo ? softClip (filtR_.process (wetR) * fb) : 0.0f;

            if (! stereo)
            {
                lineL_.push (inL + fedL);
                left[i] = inL * (1.0f - mix) + wetL * mix;
                continue;
            }

            if (pingPong)
            {
                // Summed input into the LEFT line only; the feedback crosses. The
                // first echo is hard left, the second hard right, and so on.
                const float mono = 0.5f * (inL + inR);
                lineL_.push (mono + fedR);
                lineR_.push (fedL);
            }
            else
            {
                lineL_.push (inL + fedL);
                lineR_.push (inR + fedR);
            }

            left[i]  = inL * (1.0f - mix) + wetL * mix;
            right[i] = inR * (1.0f - mix) + wetR * mix;
        }
    }

private:
    static float clampf (float v, float lo, float hi) noexcept
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    float feedbackGain() const noexcept
    {
        return feedbackPct_.load (std::memory_order_relaxed) * 0.01f;
    }

    void refreshFilters() noexcept
    {
        const double hp = (double) hpHz_.load (std::memory_order_relaxed);
        const double lp = (double) lpHz_.load (std::memory_order_relaxed);

        filtL_.setHighpass (hp, sampleRate_);
        filtL_.setLowpass  (lp, sampleRate_);
        filtR_.setHighpass (hp, sampleRate_);
        filtR_.setLowpass  (lp, sampleRate_);
    }

    // The two read positions in samples, before smoothing and modulation.
    void currentDelaySamples (double& l, double& r) const noexcept
    {
        const double ms = effectiveTimeMs();

        // The offset is applied to the RIGHT side only, then clamped into the
        // same advertised window as the left — so a 4-second delay with +50%
        // offset gives 4 s and 4 s rather than 4 s and a line-overrun.
        const double offset = (double) offsetPct_.load (std::memory_order_relaxed) * 0.01;
        double msR = ms * (1.0 + offset);
        if (msR < (double) kMinTimeMs) msR = (double) kMinTimeMs;
        if (msR > (double) kMaxTimeMs) msR = (double) kMaxTimeMs;

        l = ms   * sampleRate_ * 0.001;
        r = msR  * sampleRate_ * 0.001;
    }

    // ---- targets (message thread writes, audio thread reads) ---------------
    std::atomic<float> timeMs_      { 350.0f };
    std::atomic<bool>  sync_        { false };
    std::atomic<int>   division_    { 6 };        // 1/8
    std::atomic<float> feedbackPct_ { 35.0f };
    std::atomic<float> mixPct_      { 30.0f };
    std::atomic<float> hpHz_        { 120.0f };
    std::atomic<float> lpHz_        { 6000.0f };
    std::atomic<bool>  pingPong_    { false };
    std::atomic<float> offsetPct_   { 0.0f };
    std::atomic<float> modRateHz_   { 0.4f };
    std::atomic<float> modDepthMs_  { 0.0f };

    std::atomic<bool>  filtersDirty_ { true };

    // ---- audio-thread state -------------------------------------------------
    DelayLine  lineL_, lineR_;
    LoopFilter filtL_, filtR_;
    Smoother   timeL_, timeR_, fbSmooth_, mixSmooth_;
    Lfo        lfo_;

    double sampleRate_ = 44100.0;
};

} // namespace echojay
