/*
    EedDynamicsCore.h  —  the shared dynamics DSP core for the Dynamics cluster
    (BUILTIN_SUITE_PLAN.md §4).

    Compressor, Gate, Expander, Limiter, De-Esser and every band of the 4-Band
    Compressor are FACES on this one core. That is the whole reason six devices
    are tractable: the detector, the envelope follower, the gain computer and the
    ballistics are written and tested once, and a device only decides which mode
    to run it in and which knobs to publish.

    JUCE-free, like EqEngine and GainEngine: plain C++ with a g++ unit test
    (test/dynamics_core_test.cpp) and no dependency on the plugin build. Header-
    only because the hot path is per-sample and inlining it matters.

    THE SIGNAL FLOW, and why it is in this order:

        sidechain -> SidechainFilter (HPF / LPF)  DETECTOR ONLY, never the audio
                  -> Detector (peak | true peak | RMS, per channel)
                  -> stereo LINK (blend toward the louder channel)
                  -> dB
                  -> GainComputer (threshold / ratio / knee / range)   = target GR
                  -> Ballistics (attack / release / hold, character-scaled,
                                 optionally programme-dependent)       = smoothed GR
                  -> applied to the signal (which has been through the LOOKAHEAD
                     delay, if any), + character drive, + makeup, + dry/wet mix

    THE CHARACTER MODE (DEVICE_DEPTH_PLAN.md, Dynamics) is not a stage of its own.
    It reaches into three of the stages above — it scales the dialled attack and
    release, reshapes the knee, and adds a gentle drive on the way out — which is
    exactly what distinguishes a VCA from an opto from a FET in hardware. Keeping
    it as a modifier rather than as a box means every face gets all four
    archetypes for one param, and `clean` is bit-for-bit the device that shipped
    before it existed.

    Attack and release act on the GAIN REDUCTION, not on the detected level. Both
    placements are defensible, but smoothing the gain is what makes "attack" mean
    what a user expects — the time the compressor takes to reach its reduction —
    independently of how far over the threshold the signal went. It is also what
    makes a gate's hold expressible at all, since hold is a property of the gate's
    state, not of the input level.

    STEREO LINKING is not optional. The detector takes both channels and derives
    ONE level, so both channels get the identical gain. A per-channel detector
    makes the image wander every time one side is louder than the other, which on
    a stereo bus is the single most audible way to get dynamics wrong. This is the
    same approach the EQ's dynamic band already uses (EqEngine.cpp: "stereo-linked
    detector: peak of the band-passed signal across ch").

    REAL-TIME CONTRACT, same as EqEngine: process() never allocates, never locks,
    never blocks. Every buffer is sized in prepare(). Parameters are plain
    scalars written from the message thread and read on the audio thread; they
    are smoothed in the DSP where a jump would be audible, and the ones that are
    not (mode switches) are cheap enough that a torn read is inaudible.
*/

#pragma once

#include "viz/VizTap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
// Shared constants + small helpers.
// ---------------------------------------------------------------------------
namespace dyn
{
    constexpr double kPi = 3.14159265358979323846;

    // The level a detector reports for digital silence. -120 dB rather than
    // -infinity: an infinite level makes every downstream subtraction produce
    // NaN, and NaN in a gain coefficient is silent death for the whole chain.
    constexpr float kSilenceDb = -120.0f;

    inline float gainToDb (float g) noexcept
    {
        return g > 1.0e-6f ? 20.0f * std::log10 (g) : kSilenceDb;
    }

    inline float dbToGain (float db) noexcept
    {
        return db <= kSilenceDb ? 0.0f : std::pow (10.0f, db * 0.05f);
    }

    // One-pole coefficient for a time constant: the fraction of the remaining
    // distance covered per sample. `ms` <= 0 means "instant", which is a real
    // setting (a limiter's attack) rather than an error.
    inline float onePoleCoeff (double ms, double sampleRate) noexcept
    {
        if (ms <= 0.0 || sampleRate <= 0.0) return 1.0f;
        const double sec = ms * 0.001;
        return (float) (1.0 - std::exp (-1.0 / (sec * sampleRate)));
    }

    // ---- the DWELL HISTOGRAM's axis ---------------------------------------
    // How long the detector spends at each input level, in bins of dB. This is
    // what the transfer curve glows along: a compressor whose signal lives at
    // -18 dB has a bright band at -18 dB, and a threshold set 10 dB above where
    // the music actually sits is a curve that glows nowhere near its knee.
    //
    // ONE fixed axis rather than a per-device one, because it is written on the
    // audio thread and read by a view that may be resized, rescaled or told to
    // change its floor at any moment. A histogram whose bins mean different dB
    // on the two sides of the tap is a picture that is wrong in a way nothing
    // can detect. The view interpolates its own axis out of these bins instead.
    //
    // -96 .. +12 dB in 128 bins is 0.84 dB per bin — finer than any plot this
    // draws into, and wide enough for a limiter, whose input is routinely over
    // 0 dBFS because that is what a limiter is for.
    constexpr int   kDwellBins    = 128;
    constexpr float kDwellFloorDb = -96.0f;
    constexpr float kDwellTopDb   =  12.0f;

    using DwellTap = echojay::viz::HistogramTap<kDwellBins>;

    // The bin a level belongs in, or -1 for "below the axis".
    //
    // -1 rather than clamping to bin 0 matters: digital silence reports
    // kSilenceDb, and folding that into the bottom bin would leave every plot
    // in the rack with a permanent bright spot at its floor that has nothing to
    // do with the music. Over the TOP the clamp is correct — material above
    // +12 dBFS is real, and it belongs at the top of the picture.
    inline int dwellBinFor (float db) noexcept
    {
        if (db < kDwellFloorDb) return -1;

        const float t = (db - kDwellFloorDb) / (kDwellTopDb - kDwellFloorDb);
        const int   b = (int) (t * (float) kDwellBins);
        return b < kDwellBins ? b : kDwellBins - 1;
    }

    inline float dwellBinCentreDb (int bin) noexcept
    {
        return kDwellFloorDb
             + ((float) bin + 0.5f) * (kDwellTopDb - kDwellFloorDb) / (float) kDwellBins;
    }
}

// ---------------------------------------------------------------------------
// Biquad — transposed direct form II.
//
// Needed here rather than borrowed from EqEngine because two of the six faces
// need filtering that is NOT an EQ band: the de-esser's sidechain bandpass and
// the 4-band's Linkwitz-Riley crossover. TDF-II is chosen over DF-I for its
// better numerical behaviour at the low cutoffs a 120 Hz crossover reaches.
// ---------------------------------------------------------------------------
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    // ---- coefficient factories (RBJ cookbook, normalised by a0) -----------
    // Frequency is clamped below Nyquist: a cutoff at or above it produces a
    // degenerate tan() and an unstable filter, and a crossover dialled to 20 kHz
    // at 44.1 kHz is a thing a user can actually do.
    static double clampFreq (double f, double sampleRate) noexcept
    {
        return std::min (std::max (f, 10.0), sampleRate * 0.45);
    }

    static Biquad lowpass (double sampleRate, double freq, double q) noexcept
    {
        const double w0 = 2.0 * dyn::kPi * clampFreq (freq, sampleRate) / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::max (q, 0.05));
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = (float) (((1.0 - cw) * 0.5) / a0);
        f.b1 = (float) ((1.0 - cw) / a0);
        f.b2 = f.b0;
        f.a1 = (float) ((-2.0 * cw) / a0);
        f.a2 = (float) ((1.0 - alpha) / a0);
        return f;
    }

    static Biquad highpass (double sampleRate, double freq, double q) noexcept
    {
        const double w0 = 2.0 * dyn::kPi * clampFreq (freq, sampleRate) / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::max (q, 0.05));
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = (float) (((1.0 + cw) * 0.5) / a0);
        f.b1 = (float) (-(1.0 + cw) / a0);
        f.b2 = f.b0;
        f.a1 = (float) ((-2.0 * cw) / a0);
        f.a2 = (float) ((1.0 - alpha) / a0);
        return f;
    }

    // Constant-skirt-gain bandpass with UNITY peak — the de-esser's sidechain
    // wants "how loud is it around 7 kHz", so the filter must not itself change
    // the level it is measuring.
    static Biquad bandpass (double sampleRate, double freq, double q) noexcept
    {
        const double w0 = 2.0 * dyn::kPi * clampFreq (freq, sampleRate) / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::max (q, 0.05));
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = (float) (alpha / a0);
        f.b1 = 0.0f;
        f.b2 = (float) (-alpha / a0);
        f.a1 = (float) ((-2.0 * cw) / a0);
        f.a2 = (float) ((1.0 - alpha) / a0);
        return f;
    }

    // Adopt another biquad's coefficients without disturbing this one's state.
    // Coefficient changes are frequent (any dial move); state resets are not,
    // because a state reset is audible.
    void setCoeffs (const Biquad& src) noexcept
    {
        b0 = src.b0; b1 = src.b1; b2 = src.b2; a1 = src.a1; a2 = src.a2;
    }

    static Biquad allpass (double sampleRate, double freq, double q) noexcept
    {
        const double w0 = 2.0 * dyn::kPi * clampFreq (freq, sampleRate) / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::max (q, 0.05));
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = (float) ((1.0 - alpha) / a0);
        f.b1 = (float) ((-2.0 * cw) / a0);
        f.b2 = 1.0f;
        f.a1 = f.b1;
        f.a2 = f.b0;
        return f;
    }
};

// ---------------------------------------------------------------------------
// Linkwitz-Riley 4th-order crossover: two cascaded Butterworth 2nd-orders per
// leg. LOW + HIGH sums to a pure 2nd-order ALLPASS — magnitude-flat, no notch at
// the crossover — which is the property that makes a multiband compressor
// transparent when every band is set to unity. A Butterworth crossover instead
// sums +3 dB at the crossover point, which is exactly the kind of error that
// sounds like "the multiband is doing something" when it is doing nothing.
// ---------------------------------------------------------------------------
class LinkwitzRiley4
{
public:
    static constexpr double kQ = 0.70710678118654752440;   // Butterworth

    // Replaces the COEFFICIENTS and keeps the state. Zeroing the state instead
    // would empty the filter's memory mid-signal, which is a click on every
    // crossover movement — and a crossover is a dial a user drags.
    void setCutoff (double sampleRate, double freq) noexcept
    {
        const auto lp = Biquad::lowpass  (sampleRate, freq, kQ);
        const auto hp = Biquad::highpass (sampleRate, freq, kQ);
        lp1_.setCoeffs (lp); lp2_.setCoeffs (lp);
        hp1_.setCoeffs (hp); hp2_.setCoeffs (hp);
    }

    void reset() noexcept
    {
        lp1_.reset(); lp2_.reset(); hp1_.reset(); hp2_.reset();
    }

    void process (float x, float& lowOut, float& highOut) noexcept
    {
        lowOut  = lp2_.process (lp1_.process (x));
        highOut = hp2_.process (hp1_.process (x));
    }

private:
    Biquad lp1_, lp2_, hp1_, hp2_;
};

// The phase response an LR4 split imposes on its own input, as a single biquad.
// Used to keep the OTHER branches of a multiband tree phase-aligned with a
// branch that has been split — see FourBandSplitter for why that is required and
// not merely nice.
class LR4Allpass
{
public:
    void setCutoff (double sampleRate, double freq) noexcept
    {
        ap_.setCoeffs (Biquad::allpass (sampleRate, freq, LinkwitzRiley4::kQ));
    }
    void  reset() noexcept        { ap_.reset(); }
    float process (float x) noexcept { return ap_.process (x); }

private:
    Biquad ap_;
};

// ---------------------------------------------------------------------------
// SIDECHAIN FILTER — the DETECTOR's own high-pass / low-pass.
//
// This filters what the device LISTENS to and never what it outputs, which is
// the whole point: a bass drum 20 dB louder than everything else makes a
// full-band detector pump the entire mix in time with the kick. High-passing the
// sidechain at 100 Hz makes the compressor deaf to the kick while still
// compressing it, and that one control is the difference between a bus
// compressor that breathes musically and one that gasps.
//
// The low-pass is the other half of the same idea, for triggering rather than
// for taming: a gate with the sidechain band limited to 60-200 Hz opens on a
// kick and ignores the snare bleeding into the same microphone.
//
// It lives on the detector path inside DynamicsCore, so a device cannot wire it
// into the audio by accident — there is no code path from here to the output.
// ---------------------------------------------------------------------------
class SidechainFilter
{
public:
    // Below this a high-pass is treated as OFF rather than as a filter at 3 Hz.
    // The schema advertises 0 as off, and every value under this is inaudible on
    // a detector anyway, so one threshold serves both.
    static constexpr double kHpfOffBelowHz = 20.0;

    // And at or above this a low-pass is off: it is above the top of the
    // spectrum, so the honest thing is to remove the filter rather than run one
    // whose corner has been clamped under Nyquist and is quietly not where the
    // dial says.
    static constexpr double kLpfOffAboveHz = 20000.0;

    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        update();
        reset();
    }

    void reset() noexcept
    {
        for (auto& f : hpf_) f.reset();
        for (auto& f : lpf_) f.reset();
    }

    void setHighpassHz (double hz) noexcept { hpfHz_ = std::max (0.0, hz); update(); }
    void setLowpassHz  (double hz) noexcept { lpfHz_ = std::max (0.0, hz); update(); }

    double highpassHz() const noexcept { return hpfHz_; }
    double lowpassHz()  const noexcept { return lpfHz_; }

    bool highpassActive() const noexcept { return hpfOn_; }
    bool lowpassActive()  const noexcept { return lpfOn_; }

    // In place on one stereo frame of DETECTOR signal.
    void process (float& l, float& r) noexcept
    {
        if (hpfOn_) { l = hpf_[0].process (l); r = hpf_[1].process (r); }
        if (lpfOn_) { l = lpf_[0].process (l); r = lpf_[1].process (r); }
    }

private:
    void update() noexcept
    {
        // Q of 0.707 — Butterworth, the flattest response that does not ring.
        // A resonant sidechain filter would make the detector hear a bump that
        // is not in the music.
        hpfOn_ = hpfHz_ >= kHpfOffBelowHz;
        lpfOn_ = lpfHz_ >  0.0 && lpfHz_ < kLpfOffAboveHz;

        if (hpfOn_)
        {
            const auto f = Biquad::highpass (sampleRate_, hpfHz_, 0.70710678);
            for (auto& h : hpf_) h.setCoeffs (f);
        }
        if (lpfOn_)
        {
            const auto f = Biquad::lowpass (sampleRate_, lpfHz_, 0.70710678);
            for (auto& l : lpf_) l.setCoeffs (f);
        }
    }

    Biquad hpf_[2], lpf_[2];
    double sampleRate_ = 44100.0;
    double hpfHz_ = 0.0;
    double lpfHz_ = kLpfOffAboveHz;
    bool   hpfOn_ = false, lpfOn_ = false;
};

// ---------------------------------------------------------------------------
// Detector — peak or RMS, stereo-linked into ONE level.
// ---------------------------------------------------------------------------
enum class DetectorMode
{
    Peak = 0,   // instantaneous |x|: catches every transient, what a limiter needs
    Rms  = 1    // averaged over a window: tracks loudness, what a bus comp wants
};

class Detector
{
public:
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        updateCoeff();
        reset();
    }

    void reset() noexcept
    {
        ms_ = 0.0f;
        for (auto& z : z_) z = 0.0f;
    }

    void setMode (DetectorMode m) noexcept { mode_ = m; }
    DetectorMode getMode() const noexcept  { return mode_; }

    // TRUE PEAK — estimate the peak BETWEEN samples, not just at them.
    //
    // A sample-peak reading is blind to what a D/A converter (or a lossy codec's
    // decoder) reconstructs between the samples it was given: a sine near
    // Nyquist sampled either side of its crest reads several dB low, so a limiter
    // that holds sample peaks at -0.1 dBFS can still hand the outside world a
    // signal that clips. This is what the -0.3 dB default ceiling exists to
    // paper over, and what true-peak detection removes the need to guess at.
    //
    // Estimated with a 4-point Lagrange interpolation at the half-sample point,
    // which is the cheapest form that is actually correct in the band that
    // matters: the worst inter-sample overshoot happens at high frequencies,
    // where consecutive samples straddle the crest and the midpoint IS very
    // nearly the peak. It costs the detector about one sample of lag, which is
    // immaterial against a lookahead measured in milliseconds.
    void setTruePeak (bool on) noexcept { truePeak_ = on; }
    bool isTruePeak() const noexcept    { return truePeak_; }

    // The RMS averaging window. Short enough and RMS becomes peak; long enough
    // and the compressor stops hearing syllables. 10 ms is the usual bus value.
    void setRmsWindowMs (double ms) noexcept
    {
        rmsMs_ = std::max (0.1, ms);
        updateCoeff();
    }

    // Stereo-linked: ONE level from both channels, so both get the same gain and
    // the image cannot wander. `right` may equal `left` for mono.
    //
    // Linking by MAX rather than by sum/average: the loudest channel governs, so
    // a peak that exists on one side only is still caught. Averaging would let a
    // hard-panned transient through at half its true level.
    float process (float left, float right) noexcept
    {
        if (mode_ == DetectorMode::Peak)
            return std::max (peakOf (left, 0), peakOf (right, 1));

        // The interpolation history is kept up to date even in RMS, so switching
        // to true peak mid-signal starts from the real waveform rather than from
        // four samples of whatever was there when the mode last changed.
        push (left, 0);
        push (right, 1);

        const float sq = std::max (left * left, right * right);
        ms_ += (sq - ms_) * rmsCoeff_;
        return std::sqrt (std::max (ms_, 0.0f));
    }

private:
    void updateCoeff() noexcept
    {
        rmsCoeff_ = dyn::onePoleCoeff (rmsMs_, sampleRate_);
    }

    void push (float x, int ch) noexcept
    {
        float* z = z_ + ch * 4;
        z[0] = z[1]; z[1] = z[2]; z[2] = z[3]; z[3] = x;
    }

    float peakOf (float x, int ch) noexcept
    {
        push (x, ch);

        if (! truePeak_) return std::fabs (x);

        const float* z = z_ + ch * 4;

        // Lagrange at t = 0.5 between z[1] and z[2]: (-z0 + 9 z1 + 9 z2 - z3)/16.
        // The peak of the reconstructed waveform is either at one of the two
        // samples or between them, so the max of the three is the estimate.
        const float mid = (-z[0] + 9.0f * z[1] + 9.0f * z[2] - z[3]) * 0.0625f;

        return std::max (std::max (std::fabs (z[1]), std::fabs (z[2])),
                         std::fabs (mid));
    }

    DetectorMode mode_       = DetectorMode::Peak;
    double       sampleRate_ = 44100.0;
    double       rmsMs_      = 10.0;
    float        rmsCoeff_   = 1.0f;
    float        ms_         = 0.0f;
    bool         truePeak_   = false;

    // Four samples per channel, oldest first. Flat rather than [2][4] so the
    // two channels' histories are one contiguous block the compiler keeps in
    // registers across the frame.
    float z_[8] {};
};

// ---------------------------------------------------------------------------
// Gain computer — the static curve. Input level in dB, out a gain in dB that is
// always <= 0 (every mode here is a REDUCTION; makeup is a separate stage).
// ---------------------------------------------------------------------------
enum class DynamicsMode
{
    Compress = 0,   // downward above threshold, ratio:1
    Limit    = 1,   // downward above threshold, infinity:1 — a brick wall
    Expand   = 2,   // downward below threshold, ratio:1
    Gate     = 3,   // downward below threshold, to a floor, with hysteresis
    Duck     = 4    // the gate INVERTED: down by the range ABOVE the threshold
};

struct GainCurve
{
    float thresholdDb = -18.0f;
    float ratio       = 4.0f;    // >= 1. Ignored by Limit (infinite) and Gate.
    float kneeDb      = 6.0f;    // width of the soft transition, 0 = hard knee
    float rangeDb     = 0.0f;    // max reduction, 0 = unlimited (comp/limit)

    // Where a CLOSED gate sits. A range of 0 means "no explicit floor", which is
    // -80 dB rather than silence so that a closed gate still has a finite,
    // drawable, subtractable level.
    //
    // Spelled once, here, because two things need the same answer: the audio
    // path (DynamicsCore's gate branch) and the picture (reductionDb below,
    // which is what TransferCurveView plots). Two copies of it is how a gate
    // that draws one floor and applies another gets shipped.
    float gateFloorDb() const noexcept
    {
        return rangeDb > 0.0f ? -rangeDb : -80.0f;
    }

    // The reduction, in dB, this curve asks for at `inDb`. Never positive.
    float reductionDb (float inDb) const noexcept
    {
        float g = 0.0f;

        switch (mode)
        {
            case DynamicsMode::Compress:
            case DynamicsMode::Limit:
            {
                // Ratio of 1 is a genuine bypass of the curve, and 1/inf = 0 for
                // the limiter, which is the whole point of the brick wall.
                const float slope = (mode == DynamicsMode::Limit)
                                  ? 1.0f
                                  : 1.0f - 1.0f / std::max (ratio, 1.0f);
                const float over = inDb - thresholdDb;

                if (kneeDb > 0.0f && 2.0f * over > -kneeDb && 2.0f * over < kneeDb)
                {
                    // Quadratic soft knee: the curve and its slope are both
                    // continuous at each end of the knee, so a signal riding the
                    // threshold does not audibly chatter between two slopes.
                    const float t = over + kneeDb * 0.5f;
                    g = -slope * (t * t) / (2.0f * kneeDb);
                }
                else if (over > 0.0f)
                {
                    g = -slope * over;
                }
                break;
            }

            case DynamicsMode::Gate:
            {
                // A GATE IS NOT A STEEP EXPANDER, and this branch used to say it
                // was — it ran the expander slope below, on a `ratio` the Gate
                // device never sets. That was harmless while nothing called it
                // (DynamicsCore's gate path goes through GateState, not through
                // here) and became wrong the moment TransferCurveView started
                // plotting this function: the picture would have shown a 4:1
                // expander for a device that applies a step.
                //
                // What the gate actually does, ignoring the hysteresis and the
                // ballistics that sit on top of it: unity at or above the
                // threshold, the floor below it. Hysteresis is a property of the
                // gate's STATE, not of this curve, so it is drawn as a band by
                // the view rather than smuggled in here.
                return inDb >= thresholdDb ? 0.0f : gateFloorDb();
            }

            case DynamicsMode::Duck:
            {
                // The same step, the other way up: unity below the threshold and
                // down by the range above it. A ducker, not a compressor — the
                // reduction is a fixed amount rather than proportional to how far
                // over the signal went, which is what "pull this down whenever
                // that is playing" actually means.
                //
                // Hysteresis works identically to the gate's (GateState is shared
                // between them): engage at the threshold, disengage only once the
                // signal has fallen threshold-minus-hysteresis below it. So the
                // band the view draws is right for both without a special case.
                return inDb >= thresholdDb ? gateFloorDb() : 0.0f;
            }

            case DynamicsMode::Expand:
            {
                // Below threshold, push DOWN by (ratio - 1) x the shortfall.
                const float slope = std::max (ratio, 1.0f) - 1.0f;
                const float under = thresholdDb - inDb;

                if (kneeDb > 0.0f && 2.0f * under > -kneeDb && 2.0f * under < kneeDb)
                {
                    const float t = under + kneeDb * 0.5f;
                    g = -slope * (t * t) / (2.0f * kneeDb);
                }
                else if (under > 0.0f)
                {
                    g = -slope * under;
                }
                break;
            }
        }

        // Range caps how far the curve is allowed to pull. For an expander it is
        // the floor the signal drops to, which is what makes it usable as "duck
        // it 20 dB" rather than a fade to nothing. (Gate returned above, having
        // already applied its own floor.)
        if (rangeDb > 0.0f) g = std::max (g, -rangeDb);

        return g;
    }

    DynamicsMode mode = DynamicsMode::Compress;
};

// ---------------------------------------------------------------------------
// CHARACTER — the one selector that changes how a dynamics device FEELS.
//
// Every hardware compressor people name is really a set of four decisions:
// how fast the detector's gain moves, whether that speed depends on the
// programme, how sharp the corner at the threshold is, and how much the gain
// element colours what passes through it. This enum is those four decisions,
// bundled into the four archetypes worth having, so that "make it punchier" is
// one param rather than a paragraph of knob moves the model has to derive.
//
//   clean  — VCA. Nothing is reshaped: the dialled times are the times, the
//            dialled knee is the knee, and the signal path is arithmetic. The
//            reference, and the only mode that is bit-transparent when idle.
//   glue   — the bus compressor. Slower than you dialled in both directions and
//            softer at the corner, with a programme-dependent release, so a mix
//            settles rather than being ridden. This is the mode that makes two
//            things sound like one thing.
//   punch  — FET, the 1176 lineage. A very fast attack that grabs the transient,
//            a quick recovery, a hard corner, and audible drive when it works.
//            Aggressive on purpose: it is what a snare or a bass DI wants.
//   smooth — opto, the LA-2A lineage. A relaxed attack and a release whose speed
//            depends on how deep the reduction is, which is what a photocell
//            physically does and why nobody can dial it by hand. Forgiving, and
//            almost impossible to make sound like it is working.
//
// The scales MULTIPLY the dialled times rather than replacing them. That is the
// property that keeps the mode from stealing the knobs: attack_ms still reads
// what the user set and still means what it says relative to the other modes, so
// a mode change is a change of feel and not a silent overwrite of six values.
// ---------------------------------------------------------------------------
enum class CharacterMode
{
    Clean  = 0,
    Glue   = 1,
    Punch  = 2,
    Smooth = 3
};

constexpr int kCharacterCount = 4;

struct CharacterSpec
{
    float attackScale;      // multiplies the dialled attack
    float releaseScale;     // multiplies the dialled release
    float kneeScale;        // multiplies the dialled knee
    float kneeAddDb;        // ...and then widens it by this much
    float driveAmount;      // harmonic character at full reduction, 0 = none
    bool  programRelease;   // release slows down as the reduction deepens
};

inline CharacterSpec characterSpec (CharacterMode m) noexcept
{
    switch (m)
    {
        // A knee ADD rather than a scale alone, deliberately: a bus compressor
        // dialled to a hard 0 dB knee should still be gentle at the corner,
        // which a pure multiplication of zero cannot express.
        case CharacterMode::Glue:   return { 1.6f, 2.2f, 1.0f, 6.0f, 0.06f, true  };

        // The knee is SCALED down and not zeroed, so a user who deliberately
        // dialled a wide knee still gets a wider one here than at 3 dB.
        case CharacterMode::Punch:  return { 0.25f, 0.7f, 0.35f, 0.0f, 0.22f, false };

        case CharacterMode::Smooth: return { 2.5f, 1.6f, 1.0f, 9.0f, 0.10f, true  };

        case CharacterMode::Clean:
        default:                    return { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f,  false };
    }
}

inline const char* characterName (CharacterMode m) noexcept
{
    switch (m)
    {
        case CharacterMode::Glue:   return "glue";
        case CharacterMode::Punch:  return "punch";
        case CharacterMode::Smooth: return "smooth";
        case CharacterMode::Clean:
        default:                    return "clean";
    }
}

inline CharacterMode characterModeFromIndex (int i) noexcept
{
    if (i <= 0) return CharacterMode::Clean;
    if (i >= kCharacterCount - 1) return CharacterMode::Smooth;
    return (CharacterMode) i;
}

// The gain element's colour, as one sample in and one out.
//
// tanh(k x) / k, NOT tanh(k x) / tanh(k): dividing by k leaves the SMALL-SIGNAL
// gain at exactly unity, so engaging a character mode changes the tone and not
// the level. Normalising by tanh(k) instead would multiply quiet material by up
// to 1.5x, which reads as "punch is louder" rather than as "punch is dirtier" —
// the classic way a drive control gets mistaken for a volume control.
//
// `blend` is how much of the shaped signal is mixed in, and callers scale it by
// how hard the device is working, so a mode is inaudible until it is doing
// something and clean is identity at every level.
inline float characterShape (float x, float blend) noexcept
{
    if (blend <= 0.0f) return x;

    const float k      = 1.0f + 3.0f * blend;
    const float shaped = std::tanh (k * x) / k;
    return x + blend * (shaped - x);
}

// ---------------------------------------------------------------------------
// Ballistics — attack / release on the gain reduction, plus hold.
//
// Attack applies when the reduction is DEEPENING and release when it is
// recovering. Hold freezes the recovery for a fixed time after the curve stops
// asking for reduction, which is what stops a gate chattering on a decaying
// note and what stops a de-esser modulating inside a single sibilant.
// ---------------------------------------------------------------------------
class Ballistics
{
public:
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        updateCoeffs();
        reset();
    }

    void reset() noexcept { gainDb_ = 0.0f; holdCounter_ = 0; }

    void setAttackMs  (double ms) noexcept { attackMs_  = std::max (0.0, ms); updateCoeffs(); }
    void setReleaseMs (double ms) noexcept { releaseMs_ = std::max (0.0, ms); updateCoeffs(); }
    void setHoldMs    (double ms) noexcept
    {
        holdMs_ = std::max (0.0, ms);
        holdSamples_ = (int) (holdMs_ * 0.001 * sampleRate_);
    }

    // PROGRAMME-DEPENDENT RELEASE — the release gets slower the deeper the
    // reduction currently is.
    //
    // This is what an opto cell physically does (the photoresistor's recovery is
    // nonlinear in how much light hit it) and it is why an LA-2A is forgiving:
    // a brief transient recovers fast enough not to be heard, while a sustained
    // passage that pulled 10 dB lets go slowly enough that the recovery does not
    // pump. It is also the honest implementation of "auto release" — a single
    // time constant cannot be both, and asking a user to pick one is asking them
    // to pick which half of the material to get wrong.
    //
    // The dialled release stays the FAST end, so the knob still means something
    // and the mode only ever adds time.
    void setProgramRelease (bool on) noexcept { programRelease_ = on; }
    bool isProgramRelease() const noexcept    { return programRelease_; }

    float currentGainDb() const noexcept { return gainDb_; }

    double getAttackMs()  const noexcept { return attackMs_; }
    double getReleaseMs() const noexcept { return releaseMs_; }
    double getHoldMs()    const noexcept { return holdMs_; }

    float process (float targetDb) noexcept
    {
        if (targetDb < gainDb_)
        {
            // Deepening: attack, and arm the hold for when it recovers.
            gainDb_ += (targetDb - gainDb_) * attackCoeff_;
            holdCounter_ = holdSamples_;
        }
        else if (holdCounter_ > 0)
        {
            --holdCounter_;     // frozen at the current reduction
        }
        else if (programRelease_)
        {
            // Blended rather than switched between two constants: a hard
            // threshold at some depth would make the recovery audibly change
            // gear halfway through, which is worse than either speed.
            const float t = std::min (std::fabs (gainDb_) / kProgramDepthDb, 1.0f);
            const float c = releaseCoeff_ + (releaseSlowCoeff_ - releaseCoeff_) * t;
            gainDb_ += (targetDb - gainDb_) * c;
        }
        else
        {
            gainDb_ += (targetDb - gainDb_) * releaseCoeff_;
        }

        // A one-pole never quite arrives; snapping the last hundredth of a dB
        // keeps a released compressor at exactly unity rather than at -0.003 dB
        // forever, which matters when the device is supposed to be transparent.
        if (std::fabs (gainDb_) < 0.001f) gainDb_ = 0.0f;

        return gainDb_;
    }

private:
    // How deep a reduction counts as "fully sustained", and how much slower the
    // recovery is there. 12 dB and 4x are the numbers that make a programme-
    // dependent release read as smooth rather than as stuck: at 3 dB of
    // reduction the release is barely changed, and at 12 it is four times as
    // long, which on a 100 ms setting is 400 ms — an opto's ballpark.
    static constexpr float  kProgramDepthDb   = 12.0f;
    static constexpr double kProgramSlowScale = 4.0;

    void updateCoeffs() noexcept
    {
        attackCoeff_      = dyn::onePoleCoeff (attackMs_,  sampleRate_);
        releaseCoeff_     = dyn::onePoleCoeff (releaseMs_, sampleRate_);
        releaseSlowCoeff_ = dyn::onePoleCoeff (releaseMs_ * kProgramSlowScale, sampleRate_);
        holdSamples_      = (int) (holdMs_ * 0.001 * sampleRate_);
    }

    double sampleRate_ = 44100.0;
    double attackMs_ = 10.0, releaseMs_ = 100.0, holdMs_ = 0.0;
    float  attackCoeff_ = 1.0f, releaseCoeff_ = 1.0f, releaseSlowCoeff_ = 1.0f;
    float  gainDb_ = 0.0f;
    int    holdSamples_ = 0, holdCounter_ = 0;
    bool   programRelease_ = false;
};

// ---------------------------------------------------------------------------
// Gate state — open/closed with HYSTERESIS.
//
// A gate with one threshold chatters: a signal sitting exactly on it opens and
// closes every few samples, which is audible as a buzz and is the reason cheap
// gates are unusable on room-y sources. Hysteresis makes the CLOSE threshold sit
// below the open threshold, so a signal has to genuinely fall away before the
// gate shuts.
// ---------------------------------------------------------------------------
class GateState
{
public:
    void reset() noexcept { open_ = false; }
    bool isOpen() const noexcept { return open_; }

    // Returns true while the gate should pass audio.
    bool update (float levelDb, float openDb, float hysteresisDb) noexcept
    {
        const float closeDb = openDb - std::max (hysteresisDb, 0.0f);
        if (! open_ && levelDb >= openDb)  open_ = true;
        else if (open_ && levelDb < closeDb) open_ = false;
        return open_;
    }

private:
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// Lookahead delay — a fixed-length ring the SIGNAL passes through while the
// detector reads the undelayed input, so the gain is already at its final value
// by the time the peak arrives. This is what lets a limiter catch a transient
// without an attack fast enough to distort.
//
// It costs latency, which the device MUST report to the host
// (EedLimiterProcessor::setLatencySamples) or every other track in the session
// drifts against it.
// ---------------------------------------------------------------------------
class LookaheadDelay
{
public:
    // maxMs is the ceiling the buffer is sized for, ONCE, in prepare(). Asking
    // for more later is clamped rather than reallocated: growing a buffer on the
    // audio thread is exactly the allocation the real-time contract forbids.
    void prepare (double sampleRate, double maxMs, int numChannels) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        numCh_      = std::max (1, numChannels);
        maxSamples_ = std::max (1, (int) std::ceil (maxMs * 0.001 * sampleRate_) + 1);

        buf_.assign ((std::size_t) (maxSamples_ * numCh_), 0.0f);
        writePos_ = 0;
        setDelayMs (delayMs_);
    }

    void reset() noexcept
    {
        std::fill (buf_.begin(), buf_.end(), 0.0f);
        writePos_ = 0;
    }

    void setDelayMs (double ms) noexcept
    {
        delayMs_ = std::max (0.0, ms);
        const int want = (int) std::lround (delayMs_ * 0.001 * sampleRate_);
        delaySamples_ = std::min (std::max (want, 0), maxSamples_ - 1);
    }

    int delaySamples() const noexcept { return delaySamples_; }

    // Push one frame in, get the frame from `delaySamples` ago out. In-place on
    // the caller's scalars so a device does not need a scratch buffer.
    //
    // WRITE THEN READ, and the order is the whole correctness of a zero delay.
    // Reading first makes rd == writePos_ pick up whatever was in that slot from
    // the last time round the ring, so a delay of 0 came out as a delay of the
    // WHOLE BUFFER — 481 samples at the limiter's 10 ms maximum — while the
    // device honestly reported zero latency to the host. Silent, and exactly the
    // kind of error that reads as a mix problem: one track a few milliseconds
    // late, only when its lookahead happens to be dialled to nothing.
    //
    // Written first, the d == 0 read finds the sample just stored and the frame
    // passes through untouched; every d > 0 reads a different slot and is
    // unaffected by the reorder.
    void process (float* frame, int numChannels) noexcept
    {
        if (buf_.empty()) return;

        const int n  = std::min (numChannels, numCh_);
        const int rd = (writePos_ - delaySamples_ + maxSamples_) % maxSamples_;

        for (int ch = 0; ch < n; ++ch)
        {
            const std::size_t base = (std::size_t) ch * (std::size_t) maxSamples_;
            buf_[base + (std::size_t) writePos_] = frame[ch];
            frame[ch] = buf_[base + (std::size_t) rd];
        }

        writePos_ = (writePos_ + 1) % maxSamples_;
    }

private:
    std::vector<float> buf_;
    double sampleRate_   = 44100.0;
    double delayMs_      = 0.0;
    int    maxSamples_   = 1;
    int    delaySamples_ = 0;
    int    numCh_        = 2;
    int    writePos_     = 0;
};

// ---------------------------------------------------------------------------
// DynamicsCore — detector + gain computer + ballistics, wired together.
//
// This is what a device holds. It computes a GAIN, and deliberately does NOT
// insist on owning the signal: `gainForSidechain` lets the de-esser detect on a
// filtered band while processing the full signal, and lets the 4-band run one
// core per band. `process` is the convenience path for the common case where the
// sidechain IS the signal.
// ---------------------------------------------------------------------------
class DynamicsCore
{
public:
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        for (auto& c : ch_)
        {
            c.detector.prepare (sampleRate_);
            c.ballistics.prepare (sampleRate_);
        }
        scFilter_.prepare (sampleRate_);

        // Sized ONCE, for the maximum the device published. Every later
        // lookahead change is a read-pointer move inside this buffer, never a
        // reallocation on the audio thread. A device that publishes no lookahead
        // leaves the maximum at 0 and pays for a two-float buffer it never reads.
        lookahead_.prepare (sampleRate_, maxLookaheadMs_, 2);

        applyCharacter();
        makeupSmoothCoeff_ = dyn::onePoleCoeff (20.0, sampleRate_);

        // The decay is per FLUSH, not per block: a histogram that faded by a
        // fixed fraction per callback would fade at a rate the user's buffer
        // size chooses, so the same music would glow differently at 64 and 1024
        // samples. Tied to the sample rate, it fades in seconds.
        const double flushSec = (double) kDwellFlushSamples / sampleRate_;
        dwellDecay_ = (float) std::exp (-flushSec / kDwellTauSec);

        reset();
    }

    void reset() noexcept
    {
        for (auto& c : ch_)
        {
            c.detector.reset();
            c.ballistics.reset();
            c.gate.reset();
        }
        scFilter_.reset();
        lookahead_.reset();
        grDb_ = 0.0f;
        inDb_ = dyn::kSilenceDb;
        makeupGain_ = dyn::dbToGain (makeupDb_);
        mixSmoothed_ = mix_;

        dwell_.fill (0.0f);
        dwellCount_ = 0;
        dwellFlush_ = 0;
        dwellTap_.clear();
    }

    // ---- parameters (message thread) --------------------------------------
    void setMode (DynamicsMode m) noexcept       { curve_.mode = m; }
    DynamicsMode getMode() const noexcept        { return curve_.mode; }

    void setDetectorMode (DetectorMode m) noexcept
    {
        for (auto& c : ch_) c.detector.setMode (m);
    }
    DetectorMode getDetectorMode() const noexcept { return ch_[0].detector.getMode(); }

    void setRmsWindowMs (double ms) noexcept
    {
        for (auto& c : ch_) c.detector.setRmsWindowMs (ms);
    }

    void setTruePeak (bool on) noexcept
    {
        for (auto& c : ch_) c.detector.setTruePeak (on);
    }
    bool isTruePeak() const noexcept { return ch_[0].detector.isTruePeak(); }

    // ---- the character mode ------------------------------------------------
    // Everything it touches — the two time constants and the knee — is DERIVED
    // here rather than written into the knobs, so the dialled values still read
    // back exactly as they were set (setAttackMs/getAttackMs round-trip) and a
    // mode change is reversible. effectiveAttackMs() is what the DSP actually
    // runs, and is what a picture of the device should be drawn from.
    void setCharacter (CharacterMode m) noexcept { character_ = m; applyCharacter(); }
    CharacterMode getCharacter() const noexcept  { return character_; }

    // AUTO RELEASE. Independent of the character mode, and ORed with it: `smooth`
    // is programme-dependent by definition, and switching auto_release off must
    // not turn an opto into a VCA behind the label.
    void setAutoRelease (bool on) noexcept { autoRelease_ = on; applyCharacter(); }
    bool isAutoRelease() const noexcept    { return autoRelease_; }

    // ---- lookahead ---------------------------------------------------------
    // The detector reads the input UNDELAYED while the signal passes through a
    // delay, so the gain is already where it needs to be by the time the peak
    // arrives and the attack can be gentle instead of distorting. It costs
    // latency, and the device MUST report lookaheadSamples() to the host or this
    // track drifts against every other one in the session.
    //
    // Called BEFORE prepare(), from a device's constructor: the buffer is sized
    // once for this maximum and never grows. A device that does not publish
    // lookahead leaves it at 0, and process() then skips the ring entirely.
    void setMaxLookaheadMs (double ms) noexcept { maxLookaheadMs_ = std::max (0.0, ms); }

    void setLookaheadMs (double ms) noexcept { lookahead_.setDelayMs (ms); }
    double getLookaheadMs() const noexcept
    {
        return sampleRate_ > 0.0 ? 1000.0 * (double) lookahead_.delaySamples() / sampleRate_
                                 : 0.0;
    }
    int lookaheadSamples() const noexcept { return lookahead_.delaySamples(); }

    // The delay ON ITS OWN, for a bypassed device. BYPASS STILL DELAYS: the host
    // is compensating for the reported latency whether or not the device is
    // bypassed, so returning the signal early would shift this track in time
    // every time bypass was toggled.
    void processDelayOnly (float* left, float* right, int numSamples) noexcept
    {
        if (lookahead_.delaySamples() <= 0) return;

        for (int i = 0; i < numSamples; ++i)
        {
            float frame[2] = { left[i], right != nullptr ? right[i] : 0.0f };
            lookahead_.process (frame, 2);
            left[i] = frame[0];
            if (right != nullptr) right[i] = frame[1];
        }
    }

    // ---- the sidechain (DETECTOR path only) --------------------------------
    void setSidechainHpfHz (double hz) noexcept { scFilter_.setHighpassHz (hz); }
    void setSidechainLpfHz (double hz) noexcept { scFilter_.setLowpassHz (hz); }
    double getSidechainHpfHz() const noexcept   { return scFilter_.highpassHz(); }
    double getSidechainLpfHz() const noexcept   { return scFilter_.lowpassHz(); }

    // ---- stereo link -------------------------------------------------------
    // 1 is fully linked — ONE level from both channels, one gain, an image that
    // cannot wander, and what every face defaults to. 0 is two independent
    // compressors sharing a panel, which is what a pair of unrelated mono
    // sources on one stereo track actually wants. Between them the detectors
    // blend, which is the classic partial link: the louder side still governs
    // most of the gain, but a hard-panned hit is not forced on the other channel.
    void setStereoLink (float linked01) noexcept
    {
        link_ = std::min (std::max (linked01, 0.0f), 1.0f);
    }
    float getStereoLink() const noexcept { return link_; }

    void setThresholdDb (float db) noexcept { curve_.thresholdDb = db; }
    void setRatio       (float r)  noexcept { curve_.ratio  = std::max (r, 1.0f); }
    void setKneeDb      (float db) noexcept { dialKneeDb_ = std::max (db, 0.0f); applyCharacter(); }
    void setRangeDb     (float db) noexcept { curve_.rangeDb = std::max (db, 0.0f); }
    void setHysteresisDb(float db) noexcept { hysteresisDb_ = std::max (db, 0.0f); }

    void setAttackMs  (double ms) noexcept { dialAttackMs_  = std::max (0.0, ms); applyCharacter(); }
    void setReleaseMs (double ms) noexcept { dialReleaseMs_ = std::max (0.0, ms); applyCharacter(); }
    void setHoldMs    (double ms) noexcept
    {
        holdMs_ = std::max (0.0, ms);
        for (auto& c : ch_) c.ballistics.setHoldMs (holdMs_);
    }

    void setMakeupDb (float db) noexcept { makeupDb_ = db; }
    void setMix      (float m)  noexcept { mix_ = std::min (std::max (m, 0.0f), 1.0f); }

    float getThresholdDb() const noexcept { return curve_.thresholdDb; }
    float getRatio()       const noexcept { return curve_.ratio; }
    float getKneeDb()      const noexcept { return dialKneeDb_; }
    float getRangeDb()     const noexcept { return curve_.rangeDb; }
    float getMakeupDb()    const noexcept { return makeupDb_; }
    float getMix()         const noexcept { return mix_; }
    float getHysteresisDb()const noexcept { return hysteresisDb_; }

    double getAttackMs()  const noexcept { return dialAttackMs_; }
    double getReleaseMs() const noexcept { return dialReleaseMs_; }
    double getHoldMs()    const noexcept { return holdMs_; }

    // What the character mode turned those into — the numbers the DSP runs, and
    // the ones a readout or a drawn curve has to use if it is to be honest.
    double effectiveAttackMs()  const noexcept { return ch_[0].ballistics.getAttackMs(); }
    double effectiveReleaseMs() const noexcept { return ch_[0].ballistics.getReleaseMs(); }
    float  effectiveKneeDb()    const noexcept { return curve_.kneeDb; }
    bool   isProgramRelease()   const noexcept { return ch_[0].ballistics.isProgramRelease(); }

    // ---- metering ----------------------------------------------------------
    // The reduction currently applied, in dB, NEGATIVE (-6 means 6 dB down).
    // A benign racy read of a single float, exactly like EqEngine's
    // getBandDynamicGainDb: a meter that is one block stale is invisible, and a
    // lock here would be on the audio thread.
    float gainReductionDb() const noexcept { return grDb_; }

    // The level the DETECTOR is currently seeing, in dBFS — the same number the
    // gain computer was just asked about. One more benign racy float on exactly
    // the contract above, and the whole data path behind the live dot riding
    // the transfer curve (VISUALS_PLAN.md, "One float tap"): the curve is
    // analytic, but WHERE ON IT the signal is sitting is not, and without this
    // a threshold set 10 dB too low still looks identical on the knobs.
    //
    // dyn::kSilenceDb for digital silence, never -infinity, for the same reason
    // the detector reports it that way: an infinite level poisons every
    // subtraction downstream of it, including the editor's coordinate maths.
    float detectorLevelDb() const noexcept { return inDb_; }

    // WHERE THE SIGNAL LIVES, not where it is this instant: a decaying
    // histogram of how much time the detector has spent at each input level.
    //
    // This is the data behind the transfer curve's DWELL GLOW, and the reason
    // it is accumulated down here instead of sampled by the editor is the whole
    // point of the visual. A single float polled at 20 Hz is 20 opinions a
    // second about a signal that had 2400 levels in that time; it can only ever
    // be drawn as a point that jumps. Every sample (well, every fourth)
    // contributing to a bin makes the density CONTINUOUS — the picture is an
    // integral rather than a sample, so it flows instead of stepping, and it is
    // right about transients that live entirely between two UI frames.
    //
    // Published as one snapshot (viz::HistogramTap) rather than as bins the
    // reader might catch from two different moments, on the same never-block
    // contract as gainReductionDb.
    const dyn::DwellTap& dwellHistogram() const noexcept { return dwellTap_; }

    // ---- audio thread ------------------------------------------------------
    // One frame of sidechain in, TWO linear gains out (reduction only — makeup
    // and mix are applied by the caller, or by process()). The two are identical
    // whenever the stereo link is at 1, which is every device's default.
    void gainsForSidechain (float scLeft, float scRight,
                            float& gainLeft, float& gainRight) noexcept
    {
        // THE SIDECHAIN FILTER GOES HERE AND NOWHERE ELSE. These are local
        // copies: nothing downstream of this function touches the audio, so
        // there is no path by which a detector filter can reach the output.
        float fL = scLeft, fR = scRight;
        scFilter_.process (fL, fR);

        const float levL   = ch_[0].detector.process (fL, fL);
        const float levR   = ch_[1].detector.process (fR, fR);
        const float linked = std::max (levL, levR);

        // Linked by MAX, then blended back toward each channel's own level. At
        // link 1 both channels see `linked`, which is exactly the single-detector
        // behaviour every face shipped with.
        const float aL = linked * link_ + levL * (1.0f - link_);
        const float aR = linked * link_ + levR * (1.0f - link_);

        // The PUBLISHED level and the dwell histogram follow the linked level,
        // not one channel's: the picture is of the device, and a plot that
        // glowed at whatever the left channel happened to be doing would be a
        // different reading every time the material moved in the image.
        inDb_ = dyn::gainToDb (linked);
        accumulateDwell (inDb_);

        const float gDbL = ch_[0].ballistics.process (targetFor (0, dyn::gainToDb (aL)));
        const float gDbR = ch_[1].ballistics.process (targetFor (1, dyn::gainToDb (aR)));

        // The meter reports the DEEPER of the two, which is the most this device
        // is doing to the signal. Averaging them would under-report a device
        // pulling 10 dB off one channel and nothing off the other.
        grDb_ = std::min (gDbL, gDbR);

        gainLeft  = dyn::dbToGain (gDbL);
        gainRight = dyn::dbToGain (gDbR);
    }

    // The linked gain, for a face that applies ONE gain to both channels — the
    // de-esser, the limiter and every band of the 4-band, none of which publish
    // a stereo link, because splitting the image is not something any of them
    // could want.
    float gainForSidechain (float scLeft, float scRight) noexcept
    {
        float gL = 1.0f, gR = 1.0f;
        gainsForSidechain (scLeft, scRight, gL, gR);
        return gL;
    }

    // How much of the character mode's colour applies right now: its full amount
    // scaled by how hard the device is working. Zero when idle, so clean is
    // identity and every other mode is inaudible until it reduces — a device
    // that colours a signal it is not compressing is a distortion box.
    float characterBlend() const noexcept
    {
        const float amount = characterSpec (character_).driveAmount;
        if (amount <= 0.0f) return 0.0f;

        const float depth = std::min (std::fabs (grDb_) / kCharacterDepthDb, 1.0f);
        return amount * depth;
    }

    // One WET sample through the gain element's colour. Public because the faces
    // that drive gainForSidechain themselves (limiter, de-esser, 4-band) have to
    // apply it where they apply the gain.
    float shapeCharacter (float x) const noexcept
    {
        return characterShape (x, characterBlend());
    }

    // Full in-place stereo process using the signal as its own sidechain.
    // `right` may be null for mono.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float makeupTarget = dyn::dbToGain (makeupDb_);
        const bool  look         = lookahead_.delaySamples() > 0;

        for (int i = 0; i < numSamples; ++i)
        {
            // The DETECTOR reads the input before the delay — that is the whole
            // trick — so the sidechain is taken here, and the dry signal the mix
            // blends against is the DELAYED one, or a partial mix would comb.
            float gL = 1.0f, gR = 1.0f;
            gainsForSidechain (left[i], right != nullptr ? right[i] : left[i], gL, gR);

            if (look)
            {
                float frame[2] = { left[i], right != nullptr ? right[i] : 0.0f };
                lookahead_.process (frame, 2);
                left[i] = frame[0];
                if (right != nullptr) right[i] = frame[1];
            }

            const float dryL = left[i];
            const float dryR = right != nullptr ? right[i] : 0.0f;

            // Makeup and mix ramp rather than jump: both are dialled live, and a
            // step in either is a click.
            makeupGain_  += (makeupTarget - makeupGain_) * makeupSmoothCoeff_;
            mixSmoothed_ += (mix_ - mixSmoothed_)        * makeupSmoothCoeff_;

            // The colour goes on the WET path, after the gain and before the
            // mix, which is where a gain element physically sits. On the dry side
            // of the mix it would colour a parallel-compression blend's clean
            // half too, and at mix 0 the device would not be bit-transparent.
            const float blend = characterBlend();

            const float wetL = characterShape (dryL * gL, blend) * makeupGain_;
            left[i] = dryL + (wetL - dryL) * mixSmoothed_;

            if (right != nullptr)
            {
                const float wetR = characterShape (dryR * gR, blend) * makeupGain_;
                right[i] = dryR + (wetR - dryR) * mixSmoothed_;
            }
        }
    }

    // Exposed so a device that drives gainForSidechain itself can still ramp its
    // makeup/mix the same way process() does.
    float nextMakeupGain() noexcept
    {
        const float target = dyn::dbToGain (makeupDb_);
        makeupGain_ += (target - makeupGain_) * makeupSmoothCoeff_;
        return makeupGain_;
    }

    float nextMix() noexcept
    {
        mixSmoothed_ += (mix_ - mixSmoothed_) * makeupSmoothCoeff_;
        return mixSmoothed_;
    }

private:
    // The reduction depth at which a character mode's colour is fully in. Same
    // 12 dB the programme-dependent release uses, and for the same reason: it is
    // roughly where a compressor stops "controlling" and starts "sounding like a
    // compressor", so it is the depth both of the mode's behaviours should key on.
    static constexpr float kCharacterDepthDb = 12.0f;

    // Push the dialled values through the character mode into the ballistics and
    // the curve. Called from every setter that feeds it, so there is no order in
    // which the two can be out of step.
    void applyCharacter() noexcept
    {
        const auto s = characterSpec (character_);

        for (auto& c : ch_)
        {
            c.ballistics.setAttackMs  (dialAttackMs_  * (double) s.attackScale);
            c.ballistics.setReleaseMs (dialReleaseMs_ * (double) s.releaseScale);
            c.ballistics.setProgramRelease (autoRelease_ || s.programRelease);
        }

        curve_.kneeDb = std::max (0.0f, dialKneeDb_ * s.kneeScale + s.kneeAddDb);
    }

    // One channel's target reduction for a detected level.
    float targetFor (int chIndex, float levelDb) noexcept
    {
        if (curve_.mode == DynamicsMode::Gate || curve_.mode == DynamicsMode::Duck)
        {
            // The gate's target is binary — at unity on one side of the threshold
            // and at the floor on the other. Ballistics turn that into
            // attack/hold/release; the curve's ratio and knee do not apply, which
            // is why a gate publishes neither.
            //
            // GateState is shared by both modes and means the same thing in each:
            // "the signal is over the line, allowing for hysteresis". Which side
            // of that gets attenuated is the only difference between gating and
            // ducking.
            const bool over = ch_[chIndex].gate.update (levelDb, curve_.thresholdDb,
                                                        hysteresisDb_);
            const bool reduce = (curve_.mode == DynamicsMode::Gate) ? ! over : over;
            return reduce ? curve_.gateFloorDb() : 0.0f;
        }

        return curve_.reductionDb (levelDb);
    }

    // One in four samples, not all four. A detector's output is already an
    // envelope — consecutive samples land in the same bin nearly always — so
    // three quarters of the work buys no extra shape. At 48 kHz this is still
    // 12,000 contributions a second, six hundred times what a 20 Hz poll gives.
    static constexpr int kDwellDecimation = 4;

    // Decay + publish every this many samples. Fixed rather than per-block so
    // the fade and the publish rate are properties of the DSP, not of whatever
    // buffer size the host happens to have chosen: 5.3 ms at 48 kHz, so the
    // editor's 60 Hz timer always has something newer than its last frame.
    static constexpr int kDwellFlushSamples = 256;

    // How long energy takes to fade out of a bin, seconds. THE constant that
    // decides whether the glow shows where the signal is hitting NOW or a smear
    // of everywhere it has been: every level the music has touched in the last
    // few time constants is still lit, so a long one integrates a whole phrase
    // into an even wash across the working range.
    //
    // 180 ms is roughly a musical beat's worth of history. Long enough that a
    // transient stays visible for a frame or two after it lands, short enough
    // that the bright core tracks the part of the range the signal is actually
    // living in. The editor's 120 ms ease is what keeps the motion smooth at
    // this speed — the two are a pair, and shortening this without that ease
    // would be where the glow started to flicker.
    static constexpr double kDwellTauSec = 0.18;

    void accumulateDwell (float levelDb) noexcept
    {
        if (++dwellCount_ >= kDwellDecimation)
        {
            dwellCount_ = 0;

            const int bin = dyn::dwellBinFor (levelDb);
            if (bin >= 0) dwell_[(std::size_t) bin] += 1.0f;
        }

        if (++dwellFlush_ >= kDwellFlushSamples)
        {
            dwellFlush_ = 0;

            for (auto& v : dwell_) v *= dwellDecay_;

            dwellTap_.publish (dwell_.data());
        }
    }

    // ONE CHAIN PER CHANNEL. Two detectors, two envelope followers and two gate
    // states, because a stereo link below 1 means the two channels genuinely are
    // being measured separately — and because a gate's OPEN/CLOSED is state, so
    // an unlinked pair needs one each or the two channels fight over it.
    //
    // Both run always, even fully linked (where they compute the same number from
    // the same input). The alternative — a fast path that skips channel 1 — would
    // leave its detector and gate holding stale state at the moment the link is
    // dialled down, which is a click at best and a stuck gate at worst.
    struct Chain
    {
        Detector   detector;
        Ballistics ballistics;
        GateState  gate;
    };

    Chain           ch_[2];
    GainCurve       curve_;
    SidechainFilter scFilter_;
    LookaheadDelay  lookahead_;
    double          maxLookaheadMs_ = 0.0;

    double sampleRate_   = 44100.0;
    float  hysteresisDb_ = 3.0f;
    float  makeupDb_     = 0.0f;
    float  mix_          = 1.0f;
    float  link_         = 1.0f;

    // The DIALLED values, kept apart from what the character mode makes of them
    // so that a mode change is reversible and getAttackMs() still returns what
    // was set rather than what is being run.
    double        dialAttackMs_  = 10.0;
    double        dialReleaseMs_ = 100.0;
    double        holdMs_        = 0.0;
    float         dialKneeDb_    = 6.0f;
    CharacterMode character_     = CharacterMode::Clean;
    bool          autoRelease_   = false;

    float  makeupGain_        = 1.0f;
    float  mixSmoothed_       = 1.0f;
    float  makeupSmoothCoeff_ = 1.0f;

    float  grDb_ = 0.0f;
    float  inDb_ = dyn::kSilenceDb;

    // The accumulator is audio-thread-private and plain; only the published
    // copy in the tap is ever seen by anyone else.
    std::array<float, (std::size_t) dyn::kDwellBins> dwell_ {};
    dyn::DwellTap dwellTap_;
    int   dwellCount_ = 0;
    int   dwellFlush_ = 0;
    float dwellDecay_ = 0.99f;
};

// ---------------------------------------------------------------------------
// FourBandSplitter — one input into four LR4 bands that SUM BACK TO THE INPUT.
//
// The tree is: split at f2 into halves, then split the low half at f1 and the
// high half at f3. That alone is NOT flat, and the reason is the trap in every
// naive multiband: the low half has been through an allpass at f1 that the high
// half has not, so around f2 the two halves no longer add up. The fix is to pass
// each half through the OTHER half's allpass, after which every band has seen
// exactly the same allpass chain (AP_f1 . AP_f2 . AP_f3) and the sum is a pure
// allpass of the input — magnitude-flat at every frequency.
//
// test/dynamics_core_test.cpp measures that flatness across the spectrum; it is
// the test that would have caught the naive version, which is audibly wrong by
// several dB near the crossovers while looking completely correct in code.
// ---------------------------------------------------------------------------
class FourBandSplitter
{
public:
    static constexpr int kNumBands = 4;

    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        setCrossovers (f1_, f2_, f3_);
    }

    // Frequencies are forced into ascending order rather than rejected: the AI
    // can send them in any order, and a swapped pair that silently produced a
    // broken filter tree would be far worse than one that is quietly sorted.
    void setCrossovers (double f1, double f2, double f3) noexcept
    {
        double a = f1, b = f2, c = f3;
        if (a > b) std::swap (a, b);
        if (b > c) std::swap (b, c);
        if (a > b) std::swap (a, b);

        // Keep a minimum spacing so two crossovers cannot land on top of each
        // other and produce a band of zero width.
        const double minRatio = 1.05;
        b = std::max (b, a * minRatio);
        c = std::max (c, b * minRatio);

        f1_ = a; f2_ = b; f3_ = c;

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            ch_[ch].splitMid.setCutoff  (sampleRate_, f2_);
            ch_[ch].splitLow.setCutoff  (sampleRate_, f1_);
            ch_[ch].splitHigh.setCutoff (sampleRate_, f3_);

            // ONE allpass instance PER BAND, not one shared by the pair. A
            // biquad carries state; running two different band signals through
            // the same instance interleaves their histories and corrupts both,
            // which shows up as a sum that is flat on paper and wrong in audio.
            ch_[ch].apHigh[0].setCutoff (sampleRate_, f3_);   // compensates band 0
            ch_[ch].apHigh[1].setCutoff (sampleRate_, f3_);   //             band 1
            ch_[ch].apLow[0].setCutoff  (sampleRate_, f1_);   //             band 2
            ch_[ch].apLow[1].setCutoff  (sampleRate_, f1_);   //             band 3
        }
    }

    double crossover1() const noexcept { return f1_; }
    double crossover2() const noexcept { return f2_; }
    double crossover3() const noexcept { return f3_; }

    void reset() noexcept
    {
        for (auto& c : ch_)
        {
            c.splitMid.reset(); c.splitLow.reset(); c.splitHigh.reset();
            for (auto& a : c.apHigh) a.reset();
            for (auto& a : c.apLow)  a.reset();
        }
    }

    // One sample of one channel into four band outputs.
    void process (int channel, float x, float* bandsOut) noexcept
    {
        if (channel < 0 || channel >= kMaxChannels)
        {
            for (int b = 0; b < kNumBands; ++b) bandsOut[b] = 0.0f;
            return;
        }

        Channel& c = ch_[channel];

        float lowHalf = 0.0f, highHalf = 0.0f;
        c.splitMid.process (x, lowHalf, highHalf);

        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, b3 = 0.0f;
        c.splitLow.process  (lowHalf,  b0, b1);
        c.splitHigh.process (highHalf, b2, b3);

        // The compensation that makes the four bands sum flat.
        bandsOut[0] = c.apHigh[0].process (b0);
        bandsOut[1] = c.apHigh[1].process (b1);
        bandsOut[2] = c.apLow[0].process  (b2);
        bandsOut[3] = c.apLow[1].process  (b3);
    }

private:
    static constexpr int kMaxChannels = 2;

    struct Channel
    {
        LinkwitzRiley4 splitMid, splitLow, splitHigh;
        LR4Allpass     apHigh[2], apLow[2];
    };

    Channel ch_[kMaxChannels];
    double  sampleRate_ = 44100.0;
    double  f1_ = 120.0, f2_ = 800.0, f3_ = 5000.0;
};

} // namespace echojay
