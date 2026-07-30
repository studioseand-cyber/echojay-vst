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

// ---------------------------------------------------------------------------
// THE DELAY MODE (DEVICE_DEPTH_PLAN.md, Time).
//
// A delay's character is what happens to a repeat ON ITS WAY ROUND THE LOOP, so
// every one of these is a spec applied inside the feedback path and nowhere
// else. That placement is the whole design: the FIRST repeat comes out of the
// line untouched in every mode, and the colour accumulates from the second
// onward — which is exactly what a tape or a bucket-brigade chip does, and is
// why "each repeat is darker than the last" is a consequence here rather than a
// feature that had to be written.
//
//   digital   nothing added. The reference, and bit-identical to the device
//             before this existed.
//   tape      saturation, plus WOW (slow) and FLUTTER (fast) wobbling the read
//             position, plus extra darkening per pass. The wobble is not the
//             mod_depth knob: it is intrinsic to the medium, so it is on in tape
//             whether or not the user has dialled any modulation.
//   analog    a bucket-brigade line: band-limited hard at both ends (a BBD's
//             anti-alias and reconstruction filters are steep and low) with a
//             little grit. Darker than tape and cleaner in its timing — a BBD's
//             clock is stable, so there is only a trace of flutter.
//   pingpong  the clean digital bounce. Character-wise identical to `digital`;
//             what it changes is the routing.
// ---------------------------------------------------------------------------
enum class DelayMode
{
    Digital  = 0,
    Tape     = 1,
    Analog   = 2,
    PingPong = 3
};

constexpr int kNumDelayModes = 4;

struct DelayModeSpec
{
    float lpScale;      // multiplies the dialled low-pass corner, per pass
    float hpScale;      // multiplies the dialled high-pass corner, per pass
    float drive;        // saturation in the loop; 0 is exactly identity
    float wowMs;        // slow read-position wobble, milliseconds
    float flutterMs;    // fast read-position wobble, milliseconds
    bool  pingPong;     // hard L/R bounce
};

inline DelayModeSpec delayModeSpec (DelayMode m) noexcept
{
    switch (m)
    {
        //                    lp     hp     drive  wow    flutter  pp
        case DelayMode::Tape:
            return            { 0.55f, 1.15f, 0.42f, 0.90f, 0.22f, false };

        // Lower low-pass AND a raised high-pass: a BBD is band-limited at both
        // ends, and only doing the top would sound like a tape with the wobble
        // switched off rather than like an analogue delay.
        case DelayMode::Analog:
            return            { 0.34f, 1.85f, 0.26f, 0.04f, 0.09f, false };

        case DelayMode::PingPong:
            return            { 1.00f, 1.00f, 0.00f, 0.00f, 0.00f, true  };

        case DelayMode::Digital:
        default:
            return            { 1.00f, 1.00f, 0.00f, 0.00f, 0.00f, false };
    }
}

inline const char* delayModeName (DelayMode m) noexcept
{
    switch (m)
    {
        case DelayMode::Tape:     return "tape";
        case DelayMode::Analog:   return "analog";
        case DelayMode::PingPong: return "pingpong";
        case DelayMode::Digital:
        default:                  return "digital";
    }
}

inline DelayMode delayModeFromIndex (int i) noexcept
{
    if (i <= 0) return DelayMode::Digital;
    if (i >= kNumDelayModes - 1) return DelayMode::PingPong;
    return (DelayMode) i;
}

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
    static constexpr float kMinDuckPct     = 0.0f;
    static constexpr float kMaxDuckPct     = 100.0f;
    static constexpr float kMinDiffusionPct= 0.0f;
    static constexpr float kMaxDiffusionPct= 100.0f;

    // The diffusion allpasses in the feedback path, per channel. Four is enough
    // to turn a discrete repeat into a wash and few enough that the smear still
    // has a rhythm rather than becoming a reverb.
    static constexpr int kNumDiff = 4;

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
        duckSmooth_.prepare (sampleRate_, 0.005);

        lfo_.prepare (sampleRate_);
        wow_.prepare (sampleRate_);
        flutter_.prepare (sampleRate_);
        wow_.setRateHz (kWowRateHz);
        flutter_.setRateHz (kFlutterRateHz);

        // The diffusion allpasses, one chain per channel. Fixed, mutually
        // incommensurate lengths for the same reason a reverb's are: lengths
        // sharing a factor stack their smear into a pitch.
        for (int k = 0; k < kNumDiff; ++k)
        {
            diffL_[k].prepare (sampleRate_, 0.040);
            diffR_[k].prepare (sampleRate_, 0.040);
            diffL_[k].setDelaySamples ((int) (kDiffMsL[k] * sampleRate_ * 0.001));
            diffR_[k].setDelaySamples ((int) (kDiffMsR[k] * sampleRate_ * 0.001));
        }

        ducker_.prepare (sampleRate_);

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

        // The two tape wobbles start a quarter cycle apart from the main LFO and
        // from each other, so nothing in the device is ever phase-locked to
        // anything else — a wow and a flutter moving together are one wobble.
        wow_.reset (0.25);
        flutter_.reset (0.6);

        for (int k = 0; k < kNumDiff; ++k) { diffL_[k].reset(); diffR_[k].reset(); }

        ducker_.reset();
        duckSmooth_.snap (1.0f);

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

    // THE MODE. It scales both loop filter corners, so it dirties them the same
    // way the two frequency dials do.
    void setMode (DelayMode m) noexcept
    {
        mode_.store ((int) m, std::memory_order_relaxed);
        filtersDirty_.store (true, std::memory_order_relaxed);
    }

    DelayMode getMode() const noexcept
    {
        return delayModeFromIndex (mode_.load (std::memory_order_relaxed));
    }

    void setDuckPct (float p) noexcept
    {
        duckPct_.store (clampf (p, kMinDuckPct, kMaxDuckPct), std::memory_order_relaxed);
    }

    void setDiffusionPct (float p) noexcept
    {
        diffusionPct_.store (clampf (p, kMinDiffusionPct, kMaxDiffusionPct),
                             std::memory_order_relaxed);
    }

    float getDuckPct()      const noexcept { return duckPct_.load (std::memory_order_relaxed); }
    float getDiffusionPct() const noexcept { return diffusionPct_.load (std::memory_order_relaxed); }

    float duckWetGain() const noexcept { return ducker_.wetGain(); }

    // WHETHER THE ECHOES ACTUALLY BOUNCE, which is the mode OR the switch.
    //
    // Both are kept, and both are honoured, because they answer different
    // questions: `mode = pingpong` is "give me the clean bouncing delay", and
    // `ping_pong` is "whatever character I have chosen, bounce it". Keeping only
    // the mode would make a bouncing TAPE echo unexpressible, and keeping only the
    // switch would leave the mode list missing the entry the plan asks for. They
    // never disagree destructively: each round-trips independently, and the
    // routing is simply the OR of the two.
    bool effectivePingPong() const noexcept
    {
        return pingPong_.load (std::memory_order_relaxed)
            || delayModeSpec (getMode()).pingPong;
    }

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
        const bool  pingPong = stereo && effectivePingPong();
        const float modDepth = modDepthMs_.load (std::memory_order_relaxed)
                             * (float) sampleRate_ * 0.001f;

        // The mode, resolved once per block. Everything it contributes is a plain
        // scalar from here on, so the per-sample loop never touches the spec table.
        const auto  spec       = delayModeSpec (getMode());
        const float msToSample = (float) sampleRate_ * 0.001f;
        const float wowDepth   = spec.wowMs     * msToSample;
        const float flutDepth  = spec.flutterMs * msToSample;
        const float drive      = spec.drive;
        const bool  wobbling   = wowDepth > 0.0f || flutDepth > 0.0f;

        // DIFFUSION. Skipped entirely at zero rather than run at zero gain: a
        // Schroeder allpass with g = 0 is still a pure delay, so leaving the chain
        // in the loop would lengthen the echo and quietly detune the whole device.
        const float diff01 = diffusionPct_.load (std::memory_order_relaxed) * 0.01f;
        const bool  diffusing = diff01 > 0.0f;

        if (diffusing)
        {
            const float g = 0.15f + 0.62f * diff01;
            for (int k = 0; k < kNumDiff; ++k) { diffL_[k].setGain (g); diffR_[k].setGain (g); }
        }

        ducker_.setAmountPct (duckPct_.load (std::memory_order_relaxed));

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

            const float duck = duckSmooth_.next();
            duckSmooth_.setTarget (ducker_.process (inL, stereo ? inR : inL));

            lfo_.advance();

            // The MEDIUM's own instability, on top of whatever the mod knob asked
            // for. Wow and flutter are two different physical faults — a slow
            // speed drift and a fast one — so they are two oscillators, and the
            // right channel reads them in quadrature so the two sides drift
            // against each other rather than detuning in unison.
            float wobbleL = 0.0f, wobbleR = 0.0f;
            if (wobbling)
            {
                wow_.advance();
                flutter_.advance();
                wobbleL = wowDepth * wow_.sine()       + flutDepth * flutter_.sine();
                wobbleR = wowDepth * wow_.quadrature() + flutDepth * flutter_.quadrature();
            }

            const double dL = (double) timeL_.next()
                            + (double) (modDepth * lfo_.sine() + wobbleL);
            const double dR = (double) timeR_.next()
                            + (double) (modDepth * lfo_.quadrature() + wobbleR);

            // READ BEFORE WRITE — the loop's delay is exactly what these reads say.
            const float wetL = lineL_.readCubic (dL);
            const float wetR = stereo ? lineR_.readCubic (dR) : 0.0f;

            // Filter, colour and limit the FED-BACK copy only. The output tap
            // above is untouched, so everything the mode does shapes successive
            // repeats PROGRESSIVELY instead of colouring the first one — which is
            // both what a tape actually does and the reason `tape` sounds like a
            // medium rather than like a distortion pedal.
            //
            // Order inside the loop: tone, then diffusion, then saturation, then
            // the safety clip. Diffusion before saturation so the smear is what
            // gets driven (a driven cloud, not a cloud of driven clicks), and
            // softClip last so nothing downstream of it can push the loop gain
            // back over 1.
            float fedL = filtL_.process (wetL);
            float fedR = stereo ? filtR_.process (wetR) : 0.0f;

            if (diffusing)
            {
                for (int k = 0; k < kNumDiff; ++k) fedL = diffL_[k].process (fedL);
                if (stereo)
                    for (int k = 0; k < kNumDiff; ++k) fedR = diffR_[k].process (fedR);
            }

            fedL = softClip (tapeSaturate (fedL, drive) * fb);
            fedR = stereo ? softClip (tapeSaturate (fedR, drive) * fb) : 0.0f;

            if (! stereo)
            {
                lineL_.push (inL + fedL);
                left[i] = inL * (1.0f - mix) + wetL * duck * mix;
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

            // Ducking is applied to the WET ONLY, at the output. Inside the loop it
            // would shorten the tail instead of lowering it, and the swell in the
            // gaps — the entire point — would never happen.
            left[i]  = inL * (1.0f - mix) + wetL * duck * mix;
            right[i] = inR * (1.0f - mix) + wetR * duck * mix;
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
        const auto spec = delayModeSpec (getMode());

        // The mode SCALES the dialled corners rather than replacing them, so the
        // two dials keep meaning what they say: `analog` at LP 6 kHz is darker
        // than `digital` at LP 6 kHz, and turning the dial up still brightens it.
        // Clamped into the advertised window so a scale cannot walk a corner out
        // to somewhere the schema never promised.
        const double hp = std::clamp ((double) hpHz_.load (std::memory_order_relaxed)
                                          * (double) spec.hpScale,
                                      (double) kMinHpHz, (double) kMaxHpHz);
        const double lp = std::clamp ((double) lpHz_.load (std::memory_order_relaxed)
                                          * (double) spec.lpScale,
                                      (double) kMinLpHz, (double) kMaxLpHz);

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
    std::atomic<float> duckPct_     { 0.0f };
    std::atomic<float> diffusionPct_{ 0.0f };

    // An int, not the enum: std::atomic<enum class> is legal but carries no
    // lock-free guarantee, and an int is what every other selector here stores.
    std::atomic<int>   mode_        { (int) DelayMode::Digital };

    std::atomic<bool>  filtersDirty_ { true };

    // ---- the medium's own instability ---------------------------------------
    // Fixed rates, not dialable: wow and flutter are what a tape IS, and a
    // "flutter rate" control is a knob whose only correct setting is this one.
    // Incommensurate with each other and with nothing else in the device.
    static constexpr float kWowRateHz     = 0.61f;
    static constexpr float kFlutterRateHz = 7.3f;

    // The diffusion allpass lengths, per channel.
    static constexpr double kDiffMsL[kNumDiff] = { 7.13, 11.37, 17.91, 23.53 };
    static constexpr double kDiffMsR[kNumDiff] = { 8.29, 12.71, 19.43, 25.17 };

    // ---- audio-thread state -------------------------------------------------
    DelayLine        lineL_, lineR_;
    LoopFilter       filtL_, filtR_;
    SchroederAllpass diffL_[kNumDiff], diffR_[kNumDiff];
    Ducker           ducker_;
    Smoother         timeL_, timeR_, fbSmooth_, mixSmooth_, duckSmooth_;
    Lfo              lfo_, wow_, flutter_;

    double sampleRate_ = 44100.0;
};

} // namespace echojay
