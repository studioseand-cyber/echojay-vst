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

        sidechain -> Detector (peak | RMS, stereo-LINKED)
                  -> dB
                  -> GainComputer (threshold / ratio / knee / range)   = target GR
                  -> Ballistics (attack / release / hold)              = smoothed GR
                  -> applied to the signal, + makeup, + dry/wet mix

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

#include <algorithm>
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

    void setCutoff (double sampleRate, double freq) noexcept
    {
        lp1_ = lp2_ = Biquad::lowpass  (sampleRate, freq, kQ);
        hp1_ = hp2_ = Biquad::highpass (sampleRate, freq, kQ);
        // States were just overwritten with the fresh (zeroed) prototypes, so
        // this is also a reset. Coefficients only change at prepare/param time.
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
        ap_ = Biquad::allpass (sampleRate, freq, LinkwitzRiley4::kQ);
    }
    void  reset() noexcept        { ap_.reset(); }
    float process (float x) noexcept { return ap_.process (x); }

private:
    Biquad ap_;
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

    void reset() noexcept { ms_ = 0.0f; }

    void setMode (DetectorMode m) noexcept { mode_ = m; }
    DetectorMode getMode() const noexcept  { return mode_; }

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
            return std::max (std::fabs (left), std::fabs (right));

        const float sq = std::max (left * left, right * right);
        ms_ += (sq - ms_) * rmsCoeff_;
        return std::sqrt (std::max (ms_, 0.0f));
    }

private:
    void updateCoeff() noexcept
    {
        rmsCoeff_ = dyn::onePoleCoeff (rmsMs_, sampleRate_);
    }

    DetectorMode mode_       = DetectorMode::Peak;
    double       sampleRate_ = 44100.0;
    double       rmsMs_      = 10.0;
    float        rmsCoeff_   = 1.0f;
    float        ms_         = 0.0f;
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
    Gate     = 3    // downward below threshold, to a floor, with hysteresis
};

struct GainCurve
{
    float thresholdDb = -18.0f;
    float ratio       = 4.0f;    // >= 1. Ignored by Limit (infinite) and Gate.
    float kneeDb      = 6.0f;    // width of the soft transition, 0 = hard knee
    float rangeDb     = 0.0f;    // max reduction, 0 = unlimited (comp/limit)

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

            case DynamicsMode::Expand:
            case DynamicsMode::Gate:
            {
                // Below threshold, push DOWN by (ratio - 1) x the shortfall. A
                // gate is the same curve with a very steep ratio; what actually
                // makes it a gate is the hysteresis + hold in GateState.
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

        // Range caps how far the curve is allowed to pull. For an expander/gate
        // it is the floor the signal drops to, which is what makes a gate usable
        // as a "duck it 20 dB" rather than an on/off switch.
        if (rangeDb > 0.0f) g = std::max (g, -rangeDb);

        return g;
    }

    DynamicsMode mode = DynamicsMode::Compress;
};

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

    float currentGainDb() const noexcept { return gainDb_; }

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
    void updateCoeffs() noexcept
    {
        attackCoeff_  = dyn::onePoleCoeff (attackMs_,  sampleRate_);
        releaseCoeff_ = dyn::onePoleCoeff (releaseMs_, sampleRate_);
        holdSamples_  = (int) (holdMs_ * 0.001 * sampleRate_);
    }

    double sampleRate_ = 44100.0;
    double attackMs_ = 10.0, releaseMs_ = 100.0, holdMs_ = 0.0;
    float  attackCoeff_ = 1.0f, releaseCoeff_ = 1.0f;
    float  gainDb_ = 0.0f;
    int    holdSamples_ = 0, holdCounter_ = 0;
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
    void process (float* frame, int numChannels) noexcept
    {
        if (buf_.empty()) return;

        const int n  = std::min (numChannels, numCh_);
        const int rd = (writePos_ - delaySamples_ + maxSamples_) % maxSamples_;

        for (int ch = 0; ch < n; ++ch)
        {
            const std::size_t base = (std::size_t) ch * (std::size_t) maxSamples_;
            const float out = buf_[base + (std::size_t) rd];
            buf_[base + (std::size_t) writePos_] = frame[ch];
            frame[ch] = out;
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
        detector_.prepare (sampleRate_);
        ballistics_.prepare (sampleRate_);
        makeupSmoothCoeff_ = dyn::onePoleCoeff (20.0, sampleRate_);
        reset();
    }

    void reset() noexcept
    {
        detector_.reset();
        ballistics_.reset();
        gate_.reset();
        grDb_ = 0.0f;
        makeupGain_ = dyn::dbToGain (makeupDb_);
        mixSmoothed_ = mix_;
    }

    // ---- parameters (message thread) --------------------------------------
    void setMode (DynamicsMode m) noexcept       { curve_.mode = m; }
    DynamicsMode getMode() const noexcept        { return curve_.mode; }

    void setDetectorMode (DetectorMode m) noexcept { detector_.setMode (m); }
    void setRmsWindowMs (double ms) noexcept       { detector_.setRmsWindowMs (ms); }

    void setThresholdDb (float db) noexcept { curve_.thresholdDb = db; }
    void setRatio       (float r)  noexcept { curve_.ratio  = std::max (r, 1.0f); }
    void setKneeDb      (float db) noexcept { curve_.kneeDb = std::max (db, 0.0f); }
    void setRangeDb     (float db) noexcept { curve_.rangeDb = std::max (db, 0.0f); }
    void setHysteresisDb(float db) noexcept { hysteresisDb_ = std::max (db, 0.0f); }

    void setAttackMs  (double ms) noexcept { ballistics_.setAttackMs (ms); }
    void setReleaseMs (double ms) noexcept { ballistics_.setReleaseMs (ms); }
    void setHoldMs    (double ms) noexcept { ballistics_.setHoldMs (ms); }

    void setMakeupDb (float db) noexcept { makeupDb_ = db; }
    void setMix      (float m)  noexcept { mix_ = std::min (std::max (m, 0.0f), 1.0f); }

    float getThresholdDb() const noexcept { return curve_.thresholdDb; }
    float getRatio()       const noexcept { return curve_.ratio; }
    float getKneeDb()      const noexcept { return curve_.kneeDb; }
    float getRangeDb()     const noexcept { return curve_.rangeDb; }
    float getMakeupDb()    const noexcept { return makeupDb_; }
    float getMix()         const noexcept { return mix_; }

    // ---- metering ----------------------------------------------------------
    // The reduction currently applied, in dB, NEGATIVE (-6 means 6 dB down).
    // A benign racy read of a single float, exactly like EqEngine's
    // getBandDynamicGainDb: a meter that is one block stale is invisible, and a
    // lock here would be on the audio thread.
    float gainReductionDb() const noexcept { return grDb_; }

    // ---- audio thread ------------------------------------------------------
    // One sample of sidechain in, one LINEAR gain out (reduction only — makeup
    // and mix are applied by the caller, or by process()).
    float gainForSidechain (float scLeft, float scRight) noexcept
    {
        const float level   = detector_.process (scLeft, scRight);
        const float levelDb = dyn::gainToDb (level);

        float targetDb;

        if (curve_.mode == DynamicsMode::Gate)
        {
            // The gate's target is binary — open at unity, closed at the floor.
            // Ballistics turn that into attack/hold/release; the curve's ratio
            // and knee do not apply, which is why a gate publishes neither.
            const bool open = gate_.update (levelDb, curve_.thresholdDb, hysteresisDb_);
            const float floorDb = curve_.rangeDb > 0.0f ? -curve_.rangeDb : -80.0f;
            targetDb = open ? 0.0f : floorDb;
        }
        else
        {
            targetDb = curve_.reductionDb (levelDb);
        }

        grDb_ = ballistics_.process (targetDb);
        return dyn::dbToGain (grDb_);
    }

    // Full in-place stereo process using the signal as its own sidechain.
    // `right` may be null for mono.
    void process (float* left, float* right, int numSamples) noexcept
    {
        const float makeupTarget = dyn::dbToGain (makeupDb_);

        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = left[i];
            const float dryR = right != nullptr ? right[i] : 0.0f;

            const float g = gainForSidechain (dryL, right != nullptr ? dryR : dryL);

            // Makeup and mix ramp rather than jump: both are dialled live, and a
            // step in either is a click.
            makeupGain_  += (makeupTarget - makeupGain_) * makeupSmoothCoeff_;
            mixSmoothed_ += (mix_ - mixSmoothed_)        * makeupSmoothCoeff_;

            const float wetL = dryL * g * makeupGain_;
            left[i] = dryL + (wetL - dryL) * mixSmoothed_;

            if (right != nullptr)
            {
                const float wetR = dryR * g * makeupGain_;
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
    Detector    detector_;
    GainCurve   curve_;
    Ballistics  ballistics_;
    GateState   gate_;

    double sampleRate_   = 44100.0;
    float  hysteresisDb_ = 3.0f;
    float  makeupDb_     = 0.0f;
    float  mix_          = 1.0f;

    float  makeupGain_        = 1.0f;
    float  mixSmoothed_       = 1.0f;
    float  makeupSmoothCoeff_ = 1.0f;

    float  grDb_ = 0.0f;
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
