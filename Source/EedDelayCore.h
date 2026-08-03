/*
    EedDelayCore.h  —  the shared TIME core (BUILTIN_SUITE_PLAN.md §4, cluster F).

    One fractional delay line, one pair of one-pole filters, one Schroeder
    allpass, one LFO and one parameter smoother. Both Time devices are built out
    of exactly these pieces:

      * EchoJay Delay  — two lines + a filter in the feedback path.
      * EchoJay Reverb — a diffusion chain of allpasses feeding an 8-line FDN,
                         every one of which is a DelayLine with a filter in its
                         loop. A reverb IS a network of delays; writing the
                         primitive once is what makes the hard device tractable.

    JUCE-free, like EqEngine and GainEngine: plain C++ with a g++ unit test
    (test/delay_core_test.cpp) and no dependency on the plugin build. Header-only
    because every function is a handful of lines on a per-sample loop.

    Four things here are easy to get subtly wrong, and each is a documented
    decision rather than an accident:

    * INTERPOLATION. A delay whose time is modulated (chorus, tape flutter, an
      FDN's de-metallising wobble) reads at a FRACTIONAL position. Linear
      interpolation there is a comb filter that dulls the top end and whose gain
      wobbles with the fraction — audible as a lisp on every modulated echo.
      4-point Catmull-Rom costs a few more multiplies and is flat enough that the
      modulation stops colouring the sound.

    * READ/WRITE ORDER. In a feedback loop you must read BEFORE you write, or the
      loop has zero delay and the filter blows up. Every read here is expressed as
      "k samples before the most recent push", so a caller that reads then pushes
      gets the physically meaningful thing and cannot accidentally build a
      delay-free loop.

    * DENORMALS. A reverb tail decays smoothly toward zero and spends its last
      seconds in denormal territory, where some CPUs run 100x slower — the classic
      "CPU spikes AFTER the sound stops". Every recursive state here is flushed.

    * BOUNDED FEEDBACK. A delay at 100% feedback with wide-open loop filters has a
      round-trip gain of exactly 1 and grows without limit once you keep feeding it.
      softClip() makes the loop gain strictly less than 1 for any non-zero signal,
      so "infinite" repeats fade very slowly instead of clipping the master bus.

    Real-time contract, same as EqEngine: nothing here allocates, locks or blocks
    outside prepare().
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
// Kill a denormal before it reaches a recursive state variable.
//
// The threshold is well above the denormal range (~1e-38) and far below
// anything audible (-400 dB), so this only ever zeroes values that were already
// silence.
// ---------------------------------------------------------------------------
inline float flushDenorm (float x) noexcept
{
    return (std::fabs (x) < 1.0e-20f) ? 0.0f : x;
}

// ---------------------------------------------------------------------------
// Bounded soft clip for a feedback path.
//
//   softClip(x) = x / (1 + |x|/4)
//
// Unity slope at the origin (so it is transparent at any sane level), output
// bounded to +/-4, and — the property that matters — |softClip(x)| < |x| for
// every x != 0. A feedback loop containing it therefore has round-trip gain
// strictly below 1 even when the feedback knob is at maximum, which is what
// turns "100% feedback" from a divergence into an extremely long tail.
// ---------------------------------------------------------------------------
inline float softClip (float x) noexcept
{
    return x / (1.0f + std::fabs (x) * 0.25f);
}

// ---------------------------------------------------------------------------
// Tape / BBD saturation for a feedback loop.
//
// softClip() above is a SAFETY device — it exists so 100% feedback cannot
// diverge, and it is deliberately almost inaudible. This is the opposite: a
// colour you dial in, applied per pass through the loop so it ACCUMULATES the
// way a real tape echo does. Repeat one is barely touched, repeat six is
// visibly squashed, and that progression is most of what "tape" means.
//
// tanh(k x) / k, not tanh(k x): dividing by k leaves the small-signal gain at
// unity, so engaging a mode changes the tone and not the level. Inside a
// FEEDBACK loop that property matters twice over — a saturator with gain > 1 at
// low level raises the loop gain, and a loop gain over 1 is a device that howls
// at a setting that measured fine.
//
// `drive` 0 is exactly identity, so a clean mode costs nothing and is bit-exact.
// ---------------------------------------------------------------------------
inline float tapeSaturate (float x, float drive) noexcept
{
    if (drive <= 0.0f) return x;

    const float k = 1.0f + 6.0f * drive;
    return std::tanh (k * x) / k;
}

// ---------------------------------------------------------------------------
// One-pole low-pass. y += a * (x - y).
//
// Used as the damping filter in a feedback loop, where "one pole" is not a
// compromise: a steep filter in a loop rings, and its phase response smears the
// echo. A 6 dB/oct roll-off is what makes successive repeats get progressively
// darker the way a tape or a room does.
// ---------------------------------------------------------------------------
class OnePoleLowpass
{
public:
    void setCutoff (double hz, double sampleRate) noexcept
    {
        if (sampleRate <= 0.0) { a_ = 1.0f; return; }

        // Above Nyquist/2 the one-pole is effectively a wire; clamp there rather
        // than letting the exponential produce a > 1 coefficient (which is an
        // unstable pole, not a brighter filter).
        const double fc = std::clamp (hz, 1.0, sampleRate * 0.49);
        a_ = (float) (1.0 - std::exp (-2.0 * 3.14159265358979323846 * fc / sampleRate));
        a_ = std::clamp (a_, 0.0f, 1.0f);
    }

    void reset() noexcept { y_ = 0.0f; }

    float process (float x) noexcept
    {
        y_ = flushDenorm (y_ + a_ * (x - y_));
        return y_;
    }

    float coeff() const noexcept { return a_; }

private:
    float a_ = 1.0f;
    float y_ = 0.0f;
};

// ---------------------------------------------------------------------------
// One-pole high-pass, built as "input minus its low-passed part".
//
// In a feedback loop this is what stops DC and sub-bass energy from accumulating
// over hundreds of repeats into a slow, unrecoverable rumble.
// ---------------------------------------------------------------------------
class OnePoleHighpass
{
public:
    void setCutoff (double hz, double sampleRate) noexcept { lp_.setCutoff (hz, sampleRate); }
    void reset() noexcept                                  { lp_.reset(); }

    float process (float x) noexcept { return x - lp_.process (x); }

private:
    OnePoleLowpass lp_;
};

// ---------------------------------------------------------------------------
// The loop tone control: high-pass then low-pass, in series.
//
// Both devices want the same thing in their feedback path — trim the mud, roll
// off the top — so it is one object rather than two the caller has to remember
// to run in the right order.
// ---------------------------------------------------------------------------
class LoopFilter
{
public:
    void setHighpass (double hz, double sampleRate) noexcept { hp_.setCutoff (hz, sampleRate); }
    void setLowpass  (double hz, double sampleRate) noexcept { lp_.setCutoff (hz, sampleRate); }

    void reset() noexcept { hp_.reset(); lp_.reset(); }

    float process (float x) noexcept { return lp_.process (hp_.process (x)); }

private:
    OnePoleHighpass hp_;
    OnePoleLowpass  lp_;
};

// ---------------------------------------------------------------------------
// Fractional delay line.
//
// Power-of-two buffer with a mask instead of a modulo: the read position moves
// every sample when the delay is modulated, so the wrap is on the hot path.
//
// Indexing convention, used by every read: index k means "k samples before the
// most recent push". k = 0 is the sample just written. That phrasing is what
// makes read-before-write in a feedback loop the obvious thing to write.
// ---------------------------------------------------------------------------
class DelayLine
{
public:
    // maxDelaySeconds is a CEILING, not a hint: reads are clamped to it, so a
    // caller asking for more gets a shorter delay rather than reading garbage.
    void prepare (double sampleRate, double maxDelaySeconds)
    {
        const double sr   = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double want = std::max (8.0, sr * std::max (0.0, maxDelaySeconds) + 4.0);

        std::size_t n = 8;
        while ((double) n < want) n <<= 1;

        buf_.assign (n, 0.0f);
        mask_  = (int) n - 1;
        write_ = 0;
    }

    void reset() noexcept
    {
        std::fill (buf_.begin(), buf_.end(), 0.0f);
        write_ = 0;
    }

    bool isPrepared() const noexcept { return ! buf_.empty(); }

    // The largest delay a read will honour. Three samples of headroom: the
    // 4-point interpolator touches k-1 .. k+2, and k+2 must stay inside.
    double maxDelaySamples() const noexcept
    {
        return buf_.empty() ? 0.0 : (double) (buf_.size() - 3);
    }

    void push (float x) noexcept
    {
        if (buf_.empty()) return;
        buf_[(std::size_t) write_] = x;
        write_ = (write_ + 1) & mask_;
    }

    // k samples before the most recent push. k is clamped, never wrapped: a
    // caller that asks for more than the line holds gets the oldest sample it
    // has, which is a quiet mistake instead of a loud aliasing one.
    float readInt (int k) const noexcept
    {
        if (buf_.empty()) return 0.0f;
        k = std::clamp (k, 0, (int) buf_.size() - 1);
        return buf_[(std::size_t) ((write_ - 1 - k) & mask_)];
    }

    // Linear interpolation. Cheap and correct for a delay that does not move;
    // use readCubic when the time is modulated.
    float readLinear (double d) const noexcept
    {
        if (buf_.empty()) return 0.0f;

        d = std::clamp (d, 0.0, maxDelaySamples());
        const int   i = (int) d;
        const float f = (float) (d - (double) i);

        // at(i) is the NEWER of the two, at(i+1) the older; the fraction walks
        // from the newer toward the older as the delay grows.
        const float a = at (i);
        const float b = at (i + 1);
        return a + (b - a) * f;
    }

    // 4-point Catmull-Rom. Exact for any signal a cubic can fit (so a ramp comes
    // back out a ramp), and flat enough that a modulated read does not audibly
    // dull the signal.
    //
    // It needs one sample NEWER than the interpolation interval, so the delay is
    // clamped to >= 1: at a delay of 0 there is no newer sample to look at.
    float readCubic (double d) const noexcept
    {
        if (buf_.empty()) return 0.0f;

        d = std::clamp (d, 1.0, maxDelaySamples());
        const int    i = (int) d;
        const double f = d - (double) i;

        const float ym1 = at (i - 1);   // newer
        const float y0  = at (i);
        const float y1  = at (i + 1);
        const float y2  = at (i + 2);   // older

        const float c0 = y0;
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

        const float ff = (float) f;
        return ((c3 * ff + c2) * ff + c1) * ff + c0;
    }

private:
    float at (int k) const noexcept
    {
        return buf_[(std::size_t) ((write_ - 1 - k) & mask_)];
    }

    std::vector<float> buf_;
    int mask_  = 0;
    int write_ = 0;
};

// ---------------------------------------------------------------------------
// Schroeder allpass:  v[n] = x[n] + g*v[n-M],  y[n] = v[n-M] - g*v[n]
//
// Unity magnitude at every frequency, but a frequency-dependent delay. That is
// exactly what diffusion is: it smears a transient into a dense cloud WITHOUT
// changing its spectrum, which is why a reverb's input stage is a chain of these
// rather than a chain of filters.
// ---------------------------------------------------------------------------
class SchroederAllpass
{
public:
    void prepare (double sampleRate, double maxDelaySeconds)
    {
        line_.prepare (sampleRate, maxDelaySeconds);
    }

    void reset() noexcept { line_.reset(); }

    // Delay in samples, at least 1 — a zero-delay allpass is an algebraic loop.
    void setDelaySamples (int m) noexcept
    {
        m_ = std::max (1, m);
        if (line_.isPrepared())
            m_ = std::min (m_, (int) line_.maxDelaySamples());
    }

    // |g| < 1 for stability. ~0.5-0.7 is the usual diffusion range.
    void setGain (float g) noexcept { g_ = std::clamp (g, -0.95f, 0.95f); }

    int   delaySamples() const noexcept { return m_; }
    float gain()         const noexcept { return g_; }

    float process (float x) noexcept
    {
        // Read v[n-M] BEFORE pushing v[n]: index m_-1 is one sample older than
        // the sample about to be written, i.e. exactly M back from it.
        const float vM = line_.readInt (m_ - 1);
        const float v  = flushDenorm (x + g_ * vM);
        line_.push (v);
        return vM - g_ * v;
    }

private:
    DelayLine line_;
    int       m_ = 1;
    float     g_ = 0.5f;
};

// ---------------------------------------------------------------------------
// Sine LFO with a quadrature (90 degrees ahead) output.
//
// The quadrature tap is what gives a stereo delay its movement: modulating both
// channels with the SAME phase just detunes the whole image together, which is
// inaudible as width. A quarter-cycle apart, the two sides drift against each
// other and the image breathes.
// ---------------------------------------------------------------------------
class Lfo
{
public:
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        setRateHz (rateHz_);
    }

    void setRateHz (float hz) noexcept
    {
        rateHz_ = std::clamp (hz, 0.0f, 40.0f);
        inc_    = (double) rateHz_ / sampleRate_;
    }

    void reset (double phase01 = 0.0) noexcept
    {
        phase_ = phase01 - std::floor (phase01);
    }

    void advance() noexcept
    {
        phase_ += inc_;
        if (phase_ >= 1.0) phase_ -= std::floor (phase_);
    }

    float sine() const noexcept
    {
        return (float) std::sin (2.0 * 3.14159265358979323846 * phase_);
    }

    float quadrature() const noexcept
    {
        return (float) std::cos (2.0 * 3.14159265358979323846 * phase_);
    }

    float rateHz() const noexcept { return rateHz_; }

private:
    double sampleRate_ = 44100.0;
    double phase_      = 0.0;
    double inc_        = 0.0;
    float  rateHz_     = 1.0f;
};

// ---------------------------------------------------------------------------
// Ducker — a sidechain from the DRY signal that pulls the WET down.
//
// Both Time devices want the same thing and want it for the same reason: a
// delay or a reverb is competing with the source that fed it, and the moment
// the source is loudest is the moment its own tail is least wanted. Ducking
// makes the effect ebb under the dry and SWELL IN THE GAPS, which is how a wash
// this big stays out of the way of a vocal. It is the difference between a mix
// that is drowning and one that sounds huge.
//
// THE SIDECHAIN IS THE DRY INPUT, NOT THE WET. Detecting on the wet is a loop:
// the tail ducks itself, which reduces the tail, which reduces the ducking. The
// dry signal is the thing being made room for, so the dry signal is what the
// detector hears.
//
// FAST ATTACK, SLOW RELEASE, and neither is dialable. Ducking that takes 100 ms
// to engage has already let the collision happen; ducking that recovers in 50 ms
// pumps audibly on every syllable. 15 ms and 250 ms are the values that make it
// read as space rather than as a compressor, and publishing them would be two
// more knobs whose only interesting setting is this one.
//
// ABSOLUTE LEVELS, not a threshold. A threshold would be a second control that
// has to be re-dialled for every source. Instead the dry envelope is mapped
// across a fixed window — silence at the bottom, a normally-tracked signal at
// the top — so one percentage means the same thing on a quiet acoustic and a
// loud drum bus.
// ---------------------------------------------------------------------------
class Ducker
{
public:
    // The window the dry envelope is measured across. -45 dB is below anything
    // that should trigger it, and -8 dB is where a well-levelled source spends
    // its loud moments, so the full amount is reached by real material rather
    // than only by something clipping.
    static constexpr float kFloorDb = -45.0f;
    static constexpr float kRefDb   = -8.0f;

    // How far a duck of 100% can pull the wet down. 24 dB is "gone but not
    // silent": the tail is still there under the source, which is what stops the
    // effect switching on and off audibly.
    static constexpr float kMaxDepthDb = 24.0f;

    void prepare (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        attack_  = onePoleCoeff (15.0, sr);
        release_ = onePoleCoeff (250.0, sr);
        reset();
    }

    void reset() noexcept { env_ = 0.0f; gain_ = 1.0f; }

    // 0..100 from the schema. 0 is off, and off is exactly unity — a device with
    // no ducking dialled pays nothing and is bit-transparent.
    void setAmountPct (float pct) noexcept
    {
        amount_ = (pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct)) * 0.01f;
    }

    float amountPct() const noexcept { return amount_ * 100.0f; }
    bool  isActive()  const noexcept { return amount_ > 0.0f; }

    // The gain currently applied to the wet path, linear. Published for a meter
    // or a readout on the same benign racy single-float contract the rest of the
    // suite uses.
    float wetGain() const noexcept { return gain_; }

    // One frame of DRY in, the wet gain out. Called per sample, before the wet is
    // mixed, so the two are never a block out of step.
    float process (float dryL, float dryR) noexcept
    {
        const float peak = std::max (std::fabs (dryL), std::fabs (dryR));

        // Peak, not RMS, and asymmetric: the front of a word has to duck before
        // it collides, so the envelope rises immediately and falls slowly.
        env_ += (peak - env_) * (peak > env_ ? attack_ : release_);
        env_  = flushDenorm (env_);

        if (amount_ <= 0.0f) { gain_ = 1.0f; return 1.0f; }

        const float envDb = env_ > 1.0e-6f ? 20.0f * std::log10 (env_) : -120.0f;
        const float t      = (envDb - kFloorDb) / (kRefDb - kFloorDb);
        const float amount = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

        gain_ = dbToLinear (-kMaxDepthDb * amount_ * amount);
        return gain_;
    }

private:
    static float dbToLinear (float db) noexcept
    {
        return db <= -120.0f ? 0.0f : std::pow (10.0f, db * 0.05f);
    }

    // Shared with the smoother below in spirit but not in code: this one is a
    // coefficient for a per-sample follower rather than for a parameter ramp.
    static float onePoleCoeff (double ms, double sampleRate) noexcept
    {
        if (ms <= 0.0 || sampleRate <= 0.0) return 1.0f;
        return (float) (1.0 - std::exp (-1.0 / (ms * 0.001 * sampleRate)));
    }

    float amount_  = 0.0f;
    float env_     = 0.0f;
    float gain_    = 1.0f;
    float attack_  = 1.0f;
    float release_ = 1.0f;
};

// ---------------------------------------------------------------------------
// One-pole parameter smoother, in the units of the parameter itself.
//
// Delay TIME is the interesting user of this. Jumping the read position by a
// thousand samples is a click; gliding it is the tape-style pitch bend everyone
// expects when they turn a delay-time knob. So the smoothing is not just
// anti-zipper here — it is the effect.
// ---------------------------------------------------------------------------
class Smoother
{
public:
    void prepare (double sampleRate, double tauSeconds) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        coeff_ = (float) std::exp (-1.0 / (std::max (1.0e-4, tauSeconds) * sr));
        coeff_ = std::clamp (coeff_, 0.0f, 0.999999f);
    }

    void setTarget (float t) noexcept { target_ = t; }

    // Jump straight there — for a fresh state load, where ramping in from the
    // previous instance's value would be an audible artefact of nothing.
    void snap (float v) noexcept { target_ = v; current_ = v; }
    void snap() noexcept         { current_ = target_; }

    float next() noexcept
    {
        current_ = target_ + (current_ - target_) * coeff_;
        return current_;
    }

    float current() const noexcept { return current_; }
    float target()  const noexcept { return target_; }

private:
    float coeff_   = 0.0f;
    float current_ = 0.0f;
    float target_  = 0.0f;
};

} // namespace echojay
