#pragma once
// ejdsp.h (limiter_ab_guard, 7 Oct 2026): the arithmetic the metrics are built from. FFT, K-weighting
// (the plugin's own coefficients, so the harness's LUFS and the plugin's mean the same thing), a BS.1770 4x
// true-peak interpolator (the plugin's own TruePeakInterp, so a reading here and a reading in the product agree).
#include "../../Source/EchoJayKWeighting.h"
#include "../../Source/EJTruePeakInterp.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace ejdsp {

constexpr double kPi = 3.14159265358979323846;
inline double dB (double lin)  { return lin > 1e-30 ? 20.0 * std::log10 (lin) : -600.0; }
inline double dBp (double pow) { return pow > 1e-60 ? 10.0 * std::log10 (pow) : -600.0; }
inline double lin (double db)  { return std::pow (10.0, db / 20.0); }

// In-place iterative radix-2 complex FFT. n must be a power of two.
inline void fft (std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap (a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / (double) len * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w (1.0, 0.0); for (size_t k = 0; k < len / 2; ++k) { const auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } }
    }
    if (inverse) for (auto& x : a) x /= (double) n;
}
inline size_t nextPow2 (size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

struct Biquad
{
    BiquadCoeffs c {}; double z1 = 0, z2 = 0;
    inline double run (double x) { const double y = c.b0 * x + z1; z1 = c.b1 * x - c.a1 * y + z2; z2 = c.b2 * x - c.a2 * y; return y; }
};

// K-weight a channel (BS.1770-4 pre-filter + RLB), returning a new vector.
inline std::vector<double> kWeight (const std::vector<double>& x, double sr)
{
    Biquad s1, s2; echojay::computeKWeightingCoeffs (sr, s1.c, s2.c);
    std::vector<double> y (x.size());
    for (size_t n = 0; n < x.size(); ++n) y[n] = s2.run (s1.run (x[n]));
    return y;
}

// True peak of one channel, by the HARNESS'S OWN interpolator: 8x oversampling, a 96-tap Kaiser-windowed sinc per
// phase. The product's 24-tap Blackman interpolator over-reads a 12 kHz sine by 0.11 dB (its passband ripple), which
// is more than the over margin; an independent meter has to be better than the thing it judges. Counts the samples
// whose interpolated peak exceeds `ceilingLin` by more than `marginDb`, and returns the index of the worst one.
struct TruePeakResult { double peakLin = 0; size_t peakIndex = 0; size_t overs = 0; double samplePeakLin = 0; };
struct TruePeakMeter
{
    static constexpr int kPhases = 8, kTaps = 96;
    double coef[kPhases][kTaps]; double hist[kTaps]; int pos = 0;
    static double besselI0 (double x) { double s = 1, t = 1; for (int k = 1; k < 50; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-12 * s) break; } return s; }
    TruePeakMeter()
    {
        const double beta = 9.0, i0b = besselI0 (beta);
        for (int ph = 0; ph < kPhases; ++ph)
        {
            double sum = 0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double x = ((double) k - (kTaps / 2 - 0.5)) - (double) ph / kPhases + 0.5;   // the fractional point between hist samples
                const double sinc = x == 0.0 ? 1.0 : std::sin (kPi * x) / (kPi * x);
                const double r = 2.0 * x / (double) kTaps; const double w = std::abs (r) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - r * r)) / i0b;
                coef[ph][k] = sinc * w; sum += sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) coef[ph][k] /= sum;
        }
        for (auto& h : hist) h = 0; pos = 0;
    }
    inline double maxAbs (double x)
    {
        hist[pos] = x; double m = 0;
        for (int ph = 0; ph < kPhases; ++ph) { double a = 0; int idx = pos; for (int k = 0; k < kTaps; ++k) { a += coef[ph][k] * hist[idx]; idx = idx == 0 ? kTaps - 1 : idx - 1; } m = std::max (m, std::abs (a)); }
        pos = (pos + 1) % kTaps; return m;
    }
};
inline TruePeakResult truePeak (const std::vector<double>& x, double ceilingLin, double marginDb = 0.01)
{
    TruePeakMeter tp; TruePeakResult r; const double limit = ceilingLin * lin (marginDb);
    auto feed = [&] (double v, size_t idx) { const double m = tp.maxAbs (v); if (m > r.peakLin) { r.peakLin = m; r.peakIndex = idx; } if (m > limit) ++r.overs; };
    for (size_t n = 0; n < x.size(); ++n) { feed (x[n], n); r.samplePeakLin = std::max (r.samplePeakLin, std::abs (x[n])); }
    for (int i = 0; i < TruePeakMeter::kTaps; ++i) feed (0.0, x.size());   // flush the group delay
    return r;
}

// Power spectrum of x[start .. start+n) with a Hann window, n a power of two. Returns |X|^2 per bin, scaled so
// a full-scale sine reads 1.0 at its bin (coherent gain removed); the caller sums ±bins for a line.
inline std::vector<double> powerSpectrum (const std::vector<double>& x, size_t start, size_t n)
{
    std::vector<std::complex<double>> a (n);
    double cg = 0;
    for (size_t i = 0; i < n; ++i) { const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n); cg += w; a[i] = (start + i < x.size() ? x[start + i] : 0.0) * w; }
    fft (a, false);
    std::vector<double> p (n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) { const double m = std::abs (a[k]) * 2.0 / cg; p[k] = m * m; }   // amplitude of a sine = 2|X|/cg
    return p;
}

} // namespace ejdsp
