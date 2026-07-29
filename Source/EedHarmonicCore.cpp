/*
    EedHarmonicCore.cpp  —  see EedHarmonicCore.h.

    JUCE-free on purpose: this file compiles under plain g++ for
    test/harmonic_core_test.cpp, which is where the anti-aliasing claim is
    measured rather than asserted.
*/

#include "EedHarmonicCore.h"

#include <algorithm>
#include <array>

namespace echojay::harmonic
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // I0, the zeroth-order modified Bessel function, by its defining series
    // sum_k ((x/2)^k / k!)^2. Converges fast for the arguments a Kaiser window
    // uses (beta <= 9), so 30 terms is far past machine precision.
    double besselI0 (double x) noexcept
    {
        double term = 1.0, sum = 1.0;
        for (int k = 1; k <= 30; ++k)
        {
            term *= (x * 0.5) / (double) k;
            sum  += term * term;
        }
        return sum;
    }

    double kaiser (int n, int length, double beta) noexcept
    {
        const double r = 2.0 * (double) n / (double) (length - 1) - 1.0;
        const double s = 1.0 - r * r;
        return besselI0 (beta * std::sqrt (s > 0.0 ? s : 0.0)) / besselI0 (beta);
    }
}

const float* halfband::coeffs() noexcept
{
    // Function-local static: built once, on first use, thread-safely.
    static const std::array<float, (std::size_t) halfband::kBranch> table = []
    {
        std::array<double, (std::size_t) halfband::kBranch> raw {};
        double sum = 0.0;

        for (int i = 0; i < halfband::kBranch; ++i)
        {
            const int n = 2 * i + 1;               // odd tap index, 1 .. 59
            const int m = n - halfband::kM;        // odd offset from the centre

            // Ideal half-band low-pass: h[m] = 0.5 * sinc(m/2). For odd m that is
            // 0.5 * sin(pi*m/2) / (pi*m/2); for even m it is zero, which is the
            // property the whole polyphase saving rests on.
            const double arg  = kPi * 0.5 * (double) m;
            const double sinc = std::sin (arg) / arg;
            const double v    = 0.5 * sinc * kaiser (n, halfband::kTaps, 9.0);

            raw[(std::size_t) i] = v;
            sum += v;
        }

        // Force the branch to sum to EXACTLY 0.5. The centre tap is already
        // exactly 0.5, so this makes both polyphase phases unity at DC; a
        // mismatch between them is a mirror image at Nyquist, and no filter
        // downstream can take it back out.
        const double norm = 0.5 / sum;

        std::array<float, (std::size_t) halfband::kBranch> out {};
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = (float) (raw[i] * norm);
        return out;
    }();

    return table.data();
}

void HalfBandUp2x::reset() noexcept
{
    std::fill (std::begin (hist_), std::end (hist_), 0.0f);
    w_ = 0;
}

void HalfBandDown2x::reset() noexcept
{
    std::fill (std::begin (e_), std::end (e_), 0.0f);
    std::fill (std::begin (o_), std::end (o_), 0.0f);
    w_ = 0;
}

// ---------------------------------------------------------------------------
// Oversampler
// ---------------------------------------------------------------------------
int Oversampler::latencyForFactor (int factor) noexcept
{
    // Each half-band filter delays by kM samples AT THE RATE IT RUNS, and a
    // round trip uses two of them.
    //   2x: up and down both run at 2x -> 2 * kM at 2x = kM base samples.
    //   4x: the 2x stage adds kM base, the 4x stage adds kM at 2x = kM/2 base.
    if (factor >= 4) return halfband::kM + halfband::kM / 2;   // 45
    if (factor >= 2) return halfband::kM;                      // 30
    return 0;
}

void Oversampler::prepare (int factor, int maxBlockSize)
{
    factor_   = factor >= 4 ? 4 : (factor >= 2 ? 2 : 1);
    maxBlock_ = std::max (1, maxBlockSize);

    mid_.assign ((std::size_t) (maxBlock_ * 2), 0.0f);
    top_.assign ((std::size_t) (maxBlock_ * factor_), 0.0f);

    reset();
}

void Oversampler::reset() noexcept
{
    up1_.reset(); up2_.reset();
    dn1_.reset(); dn2_.reset();
    std::fill (mid_.begin(), mid_.end(), 0.0f);
    std::fill (top_.begin(), top_.end(), 0.0f);
}

int Oversampler::latencySamples() const noexcept
{
    return latencyForFactor (factor_);
}

float* Oversampler::upsample (const float* in, int n) noexcept
{
    if (factor_ == 1)
    {
        for (int i = 0; i < n; ++i) top_[(std::size_t) i] = in[i];
        return top_.data();
    }

    if (factor_ == 2)
    {
        for (int i = 0; i < n; ++i)
            up1_.process (in[i], top_[(std::size_t) (2 * i)],
                                 top_[(std::size_t) (2 * i + 1)]);
        return top_.data();
    }

    for (int i = 0; i < n; ++i)
        up1_.process (in[i], mid_[(std::size_t) (2 * i)],
                             mid_[(std::size_t) (2 * i + 1)]);

    for (int i = 0; i < 2 * n; ++i)
        up2_.process (mid_[(std::size_t) i], top_[(std::size_t) (2 * i)],
                                             top_[(std::size_t) (2 * i + 1)]);
    return top_.data();
}

void Oversampler::downsample (float* out, int n) noexcept
{
    if (factor_ == 1)
    {
        for (int i = 0; i < n; ++i) out[i] = top_[(std::size_t) i];
        return;
    }

    if (factor_ == 2)
    {
        for (int i = 0; i < n; ++i)
            out[i] = dn1_.process (top_[(std::size_t) (2 * i)],
                                   top_[(std::size_t) (2 * i + 1)]);
        return;
    }

    for (int i = 0; i < 2 * n; ++i)
        mid_[(std::size_t) i] = dn2_.process (top_[(std::size_t) (2 * i)],
                                              top_[(std::size_t) (2 * i + 1)]);

    for (int i = 0; i < n; ++i)
        out[i] = dn1_.process (mid_[(std::size_t) (2 * i)],
                               mid_[(std::size_t) (2 * i + 1)]);
}

// ---------------------------------------------------------------------------
// DelayLine
// ---------------------------------------------------------------------------
void DelayLine::prepare (int maxDelaySamples)
{
    // +2 for the fractional read's second tap, +1 so a read at the maximum delay
    // cannot land on the sample being written this instant.
    buf_.assign ((std::size_t) std::max (4, maxDelaySamples + 4), 0.0f);
    reset();
}

void DelayLine::reset() noexcept
{
    std::fill (buf_.begin(), buf_.end(), 0.0f);
    w_ = 0;
}

// ---------------------------------------------------------------------------
float blockCoefficient (double sampleRate, int numSamples, double tauSeconds) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const double t  = tauSeconds > 0.0 ? tauSeconds : 0.001;
    return (float) std::exp (-(double) std::max (1, numSamples) / (t * sr));
}

const char* curveName (Curve c) noexcept
{
    switch (c)
    {
        case Curve::Tube:  return "tube";
        case Curve::Tape:  return "tape";
        case Curve::Diode: return "diode";
        case Curve::Soft:
        default:           return "soft";
    }
}

Curve curveFromIndex (int index) noexcept
{
    if (index <= 0) return Curve::Tube;
    if (index >= kCurveCount - 1) return Curve::Soft;
    return (Curve) index;
}

// ---------------------------------------------------------------------------
// HarmonicCore
// ---------------------------------------------------------------------------
void HarmonicCore::setOversampling (int factor) noexcept
{
    osFactor_ = factor >= 4 ? 4 : (factor >= 2 ? 2 : 1);
}

void HarmonicCore::prepare (double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlock_   = std::max (1, maxBlockSize);
    latency_    = Oversampler::latencyForFactor (osFactor_);

    lastToneDb_ = toneDb_.load();
    inputLevel_.prepare (sampleRate_);

    for (auto& ch : channels_)
    {
        ch.ovs.prepare (osFactor_, maxBlock_);
        ch.dc.prepare (sampleRate_);
        ch.tilt.prepare (sampleRate_, kTonePivotHz);
        // AFTER prepare: the filter rebuilds from its own stored tilt, so a tone
        // restored from state before prepare would otherwise sit unapplied until
        // the user happened to touch the knob.
        ch.tilt.setTiltDb (lastToneDb_);
        // The dry path is delayed by exactly the oversampler's latency, so a
        // partial mix sums two time-aligned signals instead of comb-filtering.
        ch.dry.prepare (latency_ + 4);
        ch.scratch.assign ((std::size_t) maxBlock_, 0.0f);
    }

    reset();
}

void HarmonicCore::reset() noexcept
{
    const Curve c = getCurve();
    const float g = std::pow (10.0f, driveDb_.load() * 0.05f);

    inputLevel_.reset();
    inputLevelTap_.set (0.0f);

    for (auto& ch : channels_)
    {
        ch.ovs.reset();
        ch.dc.reset();
        ch.tilt.reset();
        ch.dry.reset();

        // Snap, not ramp: a restored session must start AT its values rather
        // than sliding into them over the first blocks of playback.
        ch.drive.snap (g);
        ch.comp .snap (driveCompensation (c, g));
        ch.wet  .snap (mix_.load() * 0.01f);
        ch.dryG .snap (1.0f - mix_.load() * 0.01f);
        ch.out  .snap (std::pow (10.0f, outDb_.load() * 0.05f));
    }
}

void HarmonicCore::process (float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr) return;

    const float toneDb = toneDb_.load();
    if (toneDb != lastToneDb_)
    {
        lastToneDb_ = toneDb;
        for (auto& ch : channels_) ch.tilt.setTiltDb (toneDb);
    }

    // The level dot: measured on the INPUT, across both channels, before
    // anything touches it. Read once for the whole call rather than per chunk,
    // so the follower's release is timed off real elapsed samples.
    inputLevelTap_.set (inputLevel_.push (std::max (blockPeak (left, numSamples),
                                                    blockPeak (right, numSamples)),
                                          numSamples));

    // The oversampler's scratch is sized to the prepared block, so a host that
    // hands us a longer buffer than it promised is chunked rather than trusted.
    const int maxChunk = std::max (1, channels_[0].ovs.maxBlockSize());

    for (int offset = 0; offset < numSamples; offset += maxChunk)
    {
        const int n = std::min (maxChunk, numSamples - offset);
        const float coeff = blockCoefficient (sampleRate_, n, 0.020);

        processChunk (channels_[0], left + offset, n, coeff);
        if (right != nullptr)
            processChunk (channels_[1], right + offset, n, coeff);
    }
}

void HarmonicCore::processChunk (Channel& ch, float* data, int n, float blockCoeff) noexcept
{
    const Curve c       = getCurve();
    const float driveG  = std::pow (10.0f, driveDb_.load() * 0.05f);
    const float wet     = std::min (1.0f, std::max (0.0f, mix_.load() * 0.01f));
    const float outG    = std::pow (10.0f, outDb_.load() * 0.05f);

    ch.drive.startBlock (driveG, n, blockCoeff);
    ch.comp .startBlock (driveCompensation (c, driveG), n, blockCoeff);
    ch.wet  .startBlock (wet, n, blockCoeff);
    ch.dryG .startBlock (1.0f - wet, n, blockCoeff);
    ch.out  .startBlock (outG, n, blockCoeff);

    // The dry path is captured BEFORE anything touches the signal; the shaping
    // below writes its result back over `data`.
    for (int i = 0; i < n; ++i) ch.scratch[(std::size_t) i] = data[i];

    const int factor = ch.ovs.factor();
    float* os = ch.ovs.upsample (data, n);

    // Drive is held across the sub-samples of one base sample: the ramp is a
    // base-rate object, and interpolating it again inside the oversampled loop
    // would buy nothing audible for four times the arithmetic.
    for (int i = 0; i < n; ++i)
    {
        const float g = ch.drive.next();
        const float k = ch.comp.next();

        for (int s = 0; s < factor; ++s)
        {
            const std::size_t idx = (std::size_t) (i * factor + s);
            os[idx] = shape (c, os[idx] * g) * k;
        }
    }

    ch.ovs.downsample (data, n);

    for (int i = 0; i < n; ++i)
    {
        float y = ch.dc.process (data[i]);
        y = ch.tilt.process (y);

        // Written then read `latency_` back: the dry sample that entered the
        // device at the same instant as the wet one now leaving it.
        ch.dry.write (ch.scratch[(std::size_t) i]);
        const float dry = ch.dry.readInt (latency_);
        data[i] = (y * ch.wet.next() + dry * ch.dryG.next()) * ch.out.next();
    }
}

} // namespace echojay::harmonic
