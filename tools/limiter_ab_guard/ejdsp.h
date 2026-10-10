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
// `overs` counts inter-sample overs inside the audio; `edgeOvers` those within one kernel span (kTaps) of either
// end of the file or in the final flush - a print that starts or stops mid-waveform has a step there, and every
// interpolator reconstructs that step with an overshoot that is the cut, not the limiter. `edgePeakLin` is the
// largest value seen in those spans, reported beside the in-audio peak so nothing is hidden.
struct TruePeakResult { double peakLin = 0; size_t peakIndex = 0; size_t overs = 0; double samplePeakLin = 0; size_t edgeOvers = 0; double edgePeakLin = 0; };
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
    const size_t N = x.size(), span = (size_t) TruePeakMeter::kTaps;
    auto feed = [&] (double v, size_t idx)
    {
        const double m = tp.maxAbs (v);
        const bool edge = idx < span || idx + span >= N;   // the reading at `idx` describes the sample kDelay earlier; both ends covered by the full span
        if (edge) { r.edgePeakLin = std::max (r.edgePeakLin, m); if (m > limit) ++r.edgeOvers; return; }
        if (m > r.peakLin) { r.peakLin = m; r.peakIndex = idx; } if (m > limit) ++r.overs;
    };
    for (size_t n = 0; n < N; ++n) { feed (x[n], n); r.samplePeakLin = std::max (r.samplePeakLin, std::abs (x[n])); }
    for (int i = 0; i < TruePeakMeter::kTaps; ++i) feed (0.0, N);   // flush the group delay: always an edge reading
    return r;
}

// THE ARBITER (8 Oct 2026, after the gate's finding): the EXACT band-limited true peak. The signal is reconstructed
// by FFT zero-padding (16x) in 65536-sample chunks with 50 % overlap (only each chunk's central half is read, so a
// chunk's edge affects nothing), and every local maximum of |x| on the 16x grid is refined with a parabola through
// its two neighbours - the continuous peak, not the nearest grid point (at Nyquist a 16x grid alone can miss 0.17
// dB). Shares no code with any windowed-sinc meter here or in the limiter. The same edge rule as the meters: a
// reading within kEdgeSpan input samples of a file end, or in the final flush, is an EDGE reading (a cut print).
struct ExactPeakResult { double peakLin = 0; size_t peakIndex = 0; size_t overs = 0; size_t edgeOvers = 0; double edgePeakLin = 0; };
constexpr size_t kEdgeSpan = 96;
inline ExactPeakResult truePeakExact (const std::vector<double>& x, double ceilingLin, double marginDb = 0.02)
{
    ExactPeakResult r; const double limit = ceilingLin * lin (marginDb);
    const size_t N = x.size(); if (N == 0) return r;
    const size_t C = 65536, H = C / 2, OS = 16;
    std::vector<std::complex<double>> A (C), B (C * OS);
    std::vector<double> y (C * OS);
    size_t lastOverSample = (size_t) -1;
    for (size_t p = 0; p + H <= N + H; p += H)   // chunks start every H; the last ones run past N with zeros
    {
        for (size_t i = 0; i < C; ++i) A[i] = (p + i < N) ? x[p + i] : 0.0;
        fft (A, false);
        std::fill (B.begin(), B.end(), std::complex<double> (0.0, 0.0));
        for (size_t k = 0; k < C / 2; ++k) B[k] = A[k];
        B[C / 2] = A[C / 2] * 0.5; B[C * OS - C / 2] = A[C / 2] * 0.5;
        for (size_t k = C / 2 + 1; k < C; ++k) B[C * OS - C + k] = A[k];
        fft (B, true);
        for (size_t i = 0; i < C * OS; ++i) y[i] = B[i].real() * (double) OS;
        // read the central half of the chunk (the first chunk also reads its first quarter, the last its final quarter)
        const size_t i0 = (p == 0) ? 0 : C / 4, i1 = (p + C >= N + H) ? C : 3 * C / 4;
        for (size_t i = i0 * OS + 1; i + 1 < i1 * OS; ++i)
        {
            const double a = std::abs (y[i - 1]), b = std::abs (y[i]), c = std::abs (y[i + 1]);
            if (b < a || b < c) continue;
            const double den = a - 2.0 * b + c; double pk = b;
            if (den < 0.0) { const double d = 0.5 * (a - c) / den; if (std::abs (d) <= 1.0) pk = b - 0.25 * (a - c) * d; }
            const size_t n = p + i / OS; if (n >= N + kEdgeSpan) break;
            const bool edge = n < kEdgeSpan || n + kEdgeSpan >= N;
            if (edge) { r.edgePeakLin = std::max (r.edgePeakLin, pk); if (pk > limit) ++r.edgeOvers; continue; }
            if (pk > r.peakLin) { r.peakLin = pk; r.peakIndex = n; }
            if (pk > limit && n != lastOverSample) { ++r.overs; lastOverSample = n; }   // one over per input sample
        }
        if (p + C >= N + H) break;
    }
    return r;
}

// Power spectrum of x[start .. start+n) with a 4-term Blackman-Harris window (sidelobes -92 dB; Hann's leakage
// from a line that is not bin-centred put a -44 dB floor under THD+N), n a power of two. Returns |X|^2 per bin,
// scaled so a full-scale sine reads 1.0 at its bin (coherent gain removed); the caller sums ±5 bins for a line.
inline std::vector<double> powerSpectrum (const std::vector<double>& x, size_t start, size_t n)
{
    std::vector<std::complex<double>> a (n);
    double cg = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * kPi * (double) i / (double) n;
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
        cg += w; a[i] = (start + i < x.size() ? x[start + i] : 0.0) * w;
    }
    fft (a, false);
    std::vector<double> p (n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) { const double m = std::abs (a[k]) * 2.0 / cg; p[k] = m * m; }   // amplitude of a sine = 2|X|/cg
    return p;
}

} // namespace ejdsp
