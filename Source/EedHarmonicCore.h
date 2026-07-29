/*
    EedHarmonicCore.h  —  the shared DSP core behind EchoJay's Harmonic cluster
    (BUILTIN_SUITE_PLAN.md §4: Saturation, Tape and Exciter are three faces on
    this one core).

    JUCE-free, exactly like EqEngine and EedGainEngine: a plain C++ core with a
    g++ unit test (test/harmonic_core_test.cpp) that builds with no dependency on
    the plugin. That discipline is what lets the interesting part — whether the
    anti-aliasing actually works — be MEASURED rather than asserted.

    What is here, and why each piece is not optional:

    * Curves. Four static shapers, all with UNITY SLOPE AT ZERO and a bounded
      output. Unity slope matters because it makes "drive at its minimum" mean
      "near enough to a wire", so inserting the device is not a level change.
      Two of them are deliberately asymmetric: asymmetry is where even harmonics
      come from, and even harmonics are the whole reason a tube sounds unlike a
      transistor.

    * Oversampling. A waveshaper is a frequency multiplier: shaping an 11 kHz
      tone at 48 kHz puts its 5th harmonic at 55 kHz, which folds back to 7 kHz
      as an inharmonic whistle that no EQ can remove. So the shaper runs at 2x or
      4x behind a steep half-band FIR, and the test measures the folded product
      with and without it (the difference is ~40 dB).

    * Latency, and a delay-matched dry path. Anti-aliasing filters cost latency,
      so the core reports it (the plugin publishes it, the host compensates) AND
      delays its own dry signal by the same amount. Without that second half, a
      50% mix would comb-filter itself — the classic "why does 50% wet sound
      thinner than either extreme".

    * DC blocking. An asymmetric curve emits DC by construction. Left in, it eats
      headroom, thumps on every bypass toggle and slowly poisons anything with
      feedback downstream.

    Real-time contract, same as EqEngine: process() never allocates, never locks,
    never blocks. Parameters are plain atomics read at the top of a block and
    ramped per sample, so an AI move lands as a fade, not as a click.
*/

#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace echojay::harmonic
{

// ---------------------------------------------------------------------------
// The curves.
//
// Index order is FROZEN: it is the value a `type`/`mode` param carries, and it
// is written into saved state.
// ---------------------------------------------------------------------------
enum class Curve { Tube = 0, Tape = 1, Diode = 2, Soft = 3 };

inline constexpr int kCurveCount = 4;

const char* curveName (Curve c) noexcept;          // "tube" | "tape" | ...
Curve       curveFromIndex (int index) noexcept;   // clamped, never out of range

// Soft: tanh. Odd-symmetric, so odd harmonics only — the clean, neutral option.
inline float shapeSoft (float x) noexcept
{
    return std::tanh (x);
}

// Tape: x / cbrt(1 + |x|^3). Flatter than tanh near the origin and with a firmer
// knee past it, which is the shape of tape's own transfer curve: nearly linear
// until it isn't.
inline float shapeTape (float x) noexcept
{
    const float a = std::fabs (x);
    return x / std::cbrt (1.0f + a * a * a);
}

// Diode: an exponential clipper with a different ceiling per polarity, so it
// makes both a sharp odd-harmonic series (the knee is tighter than tanh) and a
// strong 2nd (the asymmetry). Slope at 0 is 1 on both sides, so it is still
// continuous and transparent at low level.
inline float shapeDiode (float x) noexcept
{
    constexpr float kNeg = 1.6f;
    if (x >= 0.0f) return 1.0f - std::exp (-x);
    return -(1.0f - std::exp (kNeg * x)) / kNeg;
}

// Tube: soft clip with MORE headroom on the negative half, the way a triode
// stage clips one side long before the other. Value and slope are continuous at
// zero, so the asymmetry shows up as harmonic content rather than as a kink.
inline float shapeTube (float x) noexcept
{
    constexpr float kNeg = 0.6f;
    if (x >= 0.0f) return std::tanh (x);
    return std::tanh (x * kNeg) / kNeg;
}

inline float shape (Curve c, float x) noexcept
{
    switch (c)
    {
        case Curve::Tube:  return shapeTube  (x);
        case Curve::Tape:  return shapeTape  (x);
        case Curve::Diode: return shapeDiode (x);
        case Curve::Soft:
        default:           return shapeSoft  (x);
    }
}

// Shape around a bias point. Subtracting the curve's value AT the bias removes
// the DC the offset would otherwise inject, so bias changes the HARMONIC balance
// (it pushes the signal onto a more asymmetric part of the curve) without also
// changing the operating level. That separation is what makes bias usable.
inline float shapeBiased (Curve c, float x, float bias) noexcept
{
    return shape (c, x + bias) - shape (c, bias);
}

// Output compensation for a given drive gain.
//
// The two obvious choices are both wrong. Dividing by the drive gain keeps quiet
// material at unity but drops a hot mix by the full drive amount (-18 dB at 18
// dB of drive). Dividing by the curve's ceiling response keeps peaks at 0 dBFS
// but hands quiet material the entire drive as a level boost, turning DRIVE into
// a volume knob. The geometric mean of the two splits the difference: at 12 dB
// of drive, quiet parts come up ~5 dB and peaks come down ~7 dB, so the knob
// changes character at roughly constant loudness — which is what "drive" is
// supposed to mean.
//
// Referenced to UNITY drive, so the whole expression is exactly 1.0 at the
// bottom of the range. Without that reference it lands on 1/sqrt(f(1)) = +1.2 dB
// with the drive knob at its minimum, which makes inserting the device at its
// defaults a level change — small, but the kind that makes an A/B lie.
inline float driveCompensation (Curve c, float driveGain) noexcept
{
    const float g   = driveGain > 1.0e-6f ? driveGain : 1.0e-6f;
    const float f   = std::fmax (1.0e-6f, shape (c, g));
    const float ref = std::fmax (1.0e-6f, shape (c, 1.0f));
    return std::sqrt (ref / (g * f));
}

// ---------------------------------------------------------------------------
// A one-pole DC blocker (~10 Hz). Asymmetric curves emit DC by construction.
// ---------------------------------------------------------------------------
class DCBlock
{
public:
    void prepare (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        constexpr double kCornerHz = 10.0;
        r_ = (float) (1.0 - (2.0 * 3.14159265358979323846 * kCornerHz / sr));
        reset();
    }

    void reset() noexcept { x1_ = 0.0f; y1_ = 0.0f; }

    inline float process (float x) noexcept
    {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float r_ = 0.999f, x1_ = 0.0f, y1_ = 0.0f;
};

// ---------------------------------------------------------------------------
// A one-pole low-pass. Used both as a tone control's band splitter and, in the
// tape face, as the head/tape HF loss.
// ---------------------------------------------------------------------------
class OnePoleLP
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
    }

    void reset() noexcept { z_ = 0.0f; }

    void setCutoff (float hz) noexcept
    {
        // Nyquist-safe: a cutoff above ~0.49 fs makes the coefficient meaningless
        // and the filter unstable-looking rather than merely open.
        const double f = std::fmin ((double) hz, sr_ * 0.49);
        a_ = (float) (1.0 - std::exp (-2.0 * 3.14159265358979323846 * std::fmax (1.0, f) / sr_));
    }

    inline float process (float x) noexcept
    {
        z_ += a_ * (x - z_);
        return z_;
    }

    inline float lastLow() const noexcept { return z_; }

private:
    double sr_ = 44100.0;
    float  a_  = 1.0f, z_ = 0.0f;
};

// ---------------------------------------------------------------------------
// A tilt tone control: one split, two complementary gains.
//
// Positive dB brightens, negative darkens, and 0 dB is EXACTLY unity (the two
// gains are reciprocal and the split is complementary), so a tone control at
// centre is not a filter in the path.
// ---------------------------------------------------------------------------
class TiltFilter
{
public:
    void prepare (double sampleRate, float pivotHz) noexcept
    {
        lp_.prepare (sampleRate);
        lp_.setCutoff (pivotHz);
        setTiltDb (tiltDb_);
    }

    void reset() noexcept { lp_.reset(); }

    // `db` is the TOTAL low-to-high tilt: +12 means the highs end up 6 dB up and
    // the lows 6 dB down.
    void setTiltDb (float db) noexcept
    {
        tiltDb_ = db;
        gLow_  = std::pow (10.0f, -db * 0.025f);   // -db/2 dB, i.e. /40
        gHigh_ = std::pow (10.0f,  db * 0.025f);
    }

    inline float process (float x) noexcept
    {
        const float low  = lp_.process (x);
        const float high = x - low;
        return low * gLow_ + high * gHigh_;
    }

private:
    OnePoleLP lp_;
    float     tiltDb_ = 0.0f, gLow_ = 1.0f, gHigh_ = 1.0f;
};

// ---------------------------------------------------------------------------
// RBJ biquad, peaking form only (the tape head bump).
// ---------------------------------------------------------------------------
class PeakBiquad
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
        setPeak (freq_, q_, gainDb_);
    }

    void reset() noexcept { x1_ = x2_ = y1_ = y2_ = 0.0f; }

    void setPeak (float freqHz, float q, float gainDb) noexcept
    {
        freq_ = freqHz; q_ = q; gainDb_ = gainDb;

        const double f = std::fmin (std::fmax (10.0, (double) freqHz), sr_ * 0.45);
        const double A = std::pow (10.0, (double) gainDb / 40.0);
        const double w = 2.0 * 3.14159265358979323846 * f / sr_;
        const double alpha = std::sin (w) / (2.0 * std::fmax (0.05, (double) q));
        const double cosw = std::cos (w);

        const double b0 =  1.0 + alpha * A;
        const double b1 = -2.0 * cosw;
        const double b2 =  1.0 - alpha * A;
        const double a0 =  1.0 + alpha / A;
        const double a1 = -2.0 * cosw;
        const double a2 =  1.0 - alpha / A;

        b0_ = (float) (b0 / a0); b1_ = (float) (b1 / a0); b2_ = (float) (b2 / a0);
        a1_ = (float) (a1 / a0); a2_ = (float) (a2 / a0);
    }

    inline float process (float x) noexcept
    {
        const float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = x;
        y2_ = y1_; y1_ = y;
        return y;
    }

private:
    double sr_ = 44100.0;
    float  freq_ = 1000.0f, q_ = 0.7f, gainDb_ = 0.0f;
    float  b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float  x1_ = 0.0f, x2_ = 0.0f, y1_ = 0.0f, y2_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Half-band FIR, 61 taps, Kaiser windowed (beta 9 -> about -90 dB stopband).
//
// Half-band means every EVEN-offset tap from the centre is exactly zero, so both
// the interpolator and the decimator cost 30 multiplies instead of 61 — and the
// centre tap is a pure delay, not a filter. 61 taps puts the passband edge near
// 0.40 fs and the stopband edge near 0.60 fs, so what folds is confined to the
// top of the audio band where nothing is left to mask.
//
// N is chosen so the half-length M = 30 is EVEN. That is not cosmetic: it puts
// the centre tap in the even polyphase phase, which makes the round-trip latency
// a whole number of base samples (30 for 2x, 45 for 4x) instead of a half sample
// the host could never compensate exactly.
// ---------------------------------------------------------------------------
namespace halfband
{
    inline constexpr int kTaps   = 61;
    inline constexpr int kM      = 30;   // (kTaps - 1) / 2, even by construction
    inline constexpr int kBranch = 30;   // the non-zero odd-offset taps
    inline constexpr int kRing   = 32;   // power of two >= kBranch + 2
    inline constexpr int kMask   = kRing - 1;

    // kBranch coefficients, summing to EXACTLY 0.5 so the two polyphase phases
    // have identical DC gain. Unequal phases would put a mirror image at Nyquist
    // that no amount of filtering downstream can undo.
    const float* coeffs() noexcept;
}

class HalfBandUp2x
{
public:
    void reset() noexcept;

    // One base sample in, two oversampled samples out (in time order).
    inline void process (float x, float& even, float& odd) noexcept
    {
        w_ = (w_ + 1) & halfband::kMask;
        hist_[w_] = x;

        // The centre tap is 0.5 and the zero-stuffing costs 6 dB, so this phase
        // is EXACTLY the input, delayed. Not approximately: exactly.
        even = hist_[(w_ - halfband::kM / 2) & halfband::kMask];

        const float* c = halfband::coeffs();
        float acc = 0.0f;
        for (int t = 0; t < halfband::kBranch; ++t)
            acc += c[t] * hist_[(w_ - t) & halfband::kMask];
        odd = 2.0f * acc;
    }

private:
    float hist_[halfband::kRing] {};
    int   w_ = 0;
};

class HalfBandDown2x
{
public:
    void reset() noexcept;

    // Two oversampled samples in (in time order), one base sample out.
    inline float process (float even, float odd) noexcept
    {
        w_ = (w_ + 1) & halfband::kMask;
        e_[w_] = even;
        o_[w_] = odd;

        float acc = 0.5f * e_[(w_ - halfband::kM / 2) & halfband::kMask];

        const float* c = halfband::coeffs();
        for (int t = 0; t < halfband::kBranch; ++t)
            acc += c[t] * o_[(w_ - 1 - t) & halfband::kMask];
        return acc;
    }

private:
    float e_[halfband::kRing] {};
    float o_[halfband::kRing] {};
    int   w_ = 0;
};

// ---------------------------------------------------------------------------
// 1x / 2x / 4x oversampling around a block of per-sample nonlinear work.
//
//     float* os = ovs.upsample (in, n);
//     for (int i = 0; i < n * ovs.factor(); ++i) os[i] = shape (curve, os[i]);
//     ovs.downsample (out, n);
//
// factor 1 is a genuine bypass (no filters, no latency), which is what makes the
// alias-suppression test a fair A/B rather than a comparison of two filters.
// ---------------------------------------------------------------------------
class Oversampler
{
public:
    void prepare (int factor, int maxBlockSize);
    void reset() noexcept;

    int factor() const noexcept { return factor_; }

    // In BASE samples: 0 at 1x, 30 at 2x, 45 at 4x.
    int latencySamples() const noexcept;
    static int latencyForFactor (int factor) noexcept;

    // Never more than the prepared maximum; the caller chunks to this.
    int maxBlockSize() const noexcept { return maxBlock_; }

    float* upsample (const float* in, int n) noexcept;
    void   downsample (float* out, int n) noexcept;

private:
    HalfBandUp2x   up1_, up2_;
    HalfBandDown2x dn1_, dn2_;

    std::vector<float> mid_;    // 2x domain
    std::vector<float> top_;    // the domain the caller shapes in

    int factor_   = 1;
    int maxBlock_ = 0;
};

// ---------------------------------------------------------------------------
// A delay line with fractional (linear-interpolated) read. Used for the
// latency-matched dry path and for tape wow/flutter.
// ---------------------------------------------------------------------------
class DelayLine
{
public:
    void prepare (int maxDelaySamples);
    void reset() noexcept;

    inline void write (float x) noexcept
    {
        if (buf_.empty()) return;
        w_ = w_ + 1 >= (int) buf_.size() ? 0 : w_ + 1;
        buf_[(std::size_t) w_] = x;
    }

    inline float readInt (int delaySamples) const noexcept
    {
        if (buf_.empty()) return 0.0f;
        int i = w_ - delaySamples;
        while (i < 0) i += (int) buf_.size();
        return buf_[(std::size_t) i];
    }

    inline float read (float delaySamples) const noexcept
    {
        if (buf_.empty()) return 0.0f;
        const float d  = delaySamples < 0.0f ? 0.0f : delaySamples;
        const int   i0 = (int) d;
        const float f  = d - (float) i0;
        const float a  = readInt (i0);
        const float b  = readInt (i0 + 1);
        return a + (b - a) * f;
    }

private:
    std::vector<float> buf_;
    int w_ = 0;
};

// ---------------------------------------------------------------------------
// Block-rate target, per-sample ramp.
//
// The parameter is chased at BLOCK rate (one exponential step per block, the
// house discipline) but delivered per SAMPLE as a straight line across the
// block. Chasing alone would still leave a step at every block boundary, which
// on a gain coefficient is exactly what a click is.
// ---------------------------------------------------------------------------
class SmoothedValue
{
public:
    void snap (float v) noexcept { cur_ = v; inc_ = 0.0f; }

    void startBlock (float target, int numSamples, float blockCoeff) noexcept
    {
        const float end = target + (cur_ - target) * blockCoeff;
        inc_ = numSamples > 0 ? (end - cur_) / (float) numSamples : 0.0f;
    }

    inline float next() noexcept { const float v = cur_; cur_ += inc_; return v; }
    inline float current() const noexcept { return cur_; }

private:
    float cur_ = 0.0f, inc_ = 0.0f;
};

// The per-block chase coefficient for a given time constant.
float blockCoefficient (double sampleRate, int numSamples, double tauSeconds) noexcept;

// ---------------------------------------------------------------------------
// HarmonicCore — the full saturation voice: drive -> curve -> DC block -> tilt
// -> latency-matched dry/wet -> output.
//
// "EchoJay Saturation" IS this class with knobs on. Tape and Exciter reuse its
// parts (the oversampler, the curves, the compensation) around their own
// topology rather than wrapping the whole thing, because their dry paths need
// delaying by a different amount.
// ---------------------------------------------------------------------------
class HarmonicCore
{
public:
    // The advertised ranges. The SAME numbers the ParamSchema publishes.
    static constexpr float kMinDriveDb =   0.0f, kMaxDriveDb = 36.0f;
    static constexpr float kMinToneDb  = -12.0f, kMaxToneDb  = 12.0f;
    static constexpr float kMinOutDb   = -24.0f, kMaxOutDb   = 24.0f;
    static constexpr float kTonePivotHz = 700.0f;

    HarmonicCore() = default;

    // The oversampling factor is fixed BEFORE prepare, because changing it
    // changes the reported latency and a host cannot be told mid-stream.
    void setOversampling (int factor) noexcept;

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    int latencySamples() const noexcept { return latency_; }

    // ---- parameters (message thread) --------------------------------------
    void setDriveDb   (float db) noexcept { driveDb_.store (db); }
    void setCurve     (Curve c)  noexcept { curve_.store ((int) c); }
    void setToneDb    (float db) noexcept { toneDb_.store (db); }
    void setMixPercent(float pc) noexcept { mix_.store (pc); }
    void setOutputDb  (float db) noexcept { outDb_.store (db); }

    float getDriveDb()    const noexcept { return driveDb_.load(); }
    Curve getCurve()      const noexcept { return curveFromIndex (curve_.load()); }
    float getToneDb()     const noexcept { return toneDb_.load(); }
    float getMixPercent() const noexcept { return mix_.load(); }
    float getOutputDb()   const noexcept { return outDb_.load(); }

    // ---- audio thread ------------------------------------------------------
    // In-place. `right` may be null for mono.
    void process (float* left, float* right, int numSamples) noexcept;

private:
    struct Channel
    {
        Oversampler ovs;
        DCBlock     dc;
        TiltFilter  tilt;
        DelayLine   dry;

        // The block's input, kept because the oversampler writes its result back
        // over `data` — without a copy there is nothing left to feed the dry
        // path, and the mix control would have only one signal to mix.
        std::vector<float> scratch;

        SmoothedValue drive, comp, wet, dryG, out;
    };

    void processChunk (Channel& ch, float* data, int n, float blockCoeff) noexcept;

    Channel channels_[2];

    std::atomic<float> driveDb_ { 0.0f };
    std::atomic<int>   curve_   { (int) Curve::Tube };
    std::atomic<float> toneDb_  { 0.0f };
    std::atomic<float> mix_     { 100.0f };
    std::atomic<float> outDb_   { 0.0f };

    double sampleRate_ = 44100.0;
    int    osFactor_   = 4;
    int    latency_    = 0;
    int    maxBlock_   = 0;

    // The tilt is the one parameter that is not per-sample ramped: it is a
    // filter coefficient, not a gain, so it is recomputed only when it moves.
    float lastToneDb_ = 0.0f;
};

} // namespace echojay::harmonic
