// probe_noise.h - THE BAND-LIMITED NOISE GENERATOR for EchoJayProbe's --sweep signal=noise (7 Oct 2026, DEESSER_PROFILE_SPEC v0.1
// section 3). Pure and standalone (std only) so the suite pins it (RoundTripTest testNoise) by relative include.
//
//   Seeded Gaussian white noise (xorshift64*, Box-Muller) through a 4th-order Butterworth band-pass - two high-pass biquads at
//   lo and two low-pass at hi with the Butterworth Q pair 0.5412 / 1.3066 (-24 dB/oct outside the band) - run for kWarmS before
//   the first kept sample, then the whole block normalised to the asked RMS. Deterministic: the same (sr, n, lo, hi, seed) gives
//   the same samples in every process and every hold, so a re-run is a re-measurement, not a new sample.
#pragma once

#include <cmath>
#include <vector>
#include <algorithm>

namespace ejprobe::noise
{

inline constexpr double kWarmS = 0.5;
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kButterQ1 = 0.5412, kButterQ2 = 1.3066;

struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
inline Biquad highpass (double sr, double hz, double q) { Biquad f; const double w = 2.0 * kPi * hz / sr, c = std::cos (w), a = std::sin (w) / (2.0 * q), n = 1.0 + a; f.b0 = (1 + c) / 2 / n; f.b1 = -(1 + c) / n; f.b2 = (1 + c) / 2 / n; f.a1 = -2 * c / n; f.a2 = (1 - a) / n; return f; }
inline Biquad lowpass  (double sr, double hz, double q) { Biquad f; const double w = 2.0 * kPi * hz / sr, c = std::cos (w), a = std::sin (w) / (2.0 * q), n = 1.0 + a; f.b0 = (1 - c) / 2 / n; f.b1 = (1 - c) / n; f.b2 = (1 - c) / 2 / n; f.a1 = -2 * c / n; f.a2 = (1 - a) / n; return f; }
struct Gauss
{
    unsigned long long s; explicit Gauss (unsigned long long seed) : s (seed ? seed : 1ULL) {}
    double uniform() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return (double) ((s * 2685821657736338717ULL) >> 11) / 9007199254740992.0; }
    double next() { const double u1 = std::max (1e-300, uniform()), u2 = uniform(); return std::sqrt (-2.0 * std::log (u1)) * std::cos (2.0 * kPi * u2); }
};
inline std::vector<double> bandLimited (double sr, long long n, double loHz, double hiHz, unsigned long long seed, double rmsTarget)
{
    Gauss g (seed); Biquad h1 = highpass (sr, loHz, kButterQ1), h2 = highpass (sr, loHz, kButterQ2), l1 = lowpass (sr, hiHz, kButterQ1), l2 = lowpass (sr, hiHz, kButterQ2);
    auto one = [&] { return l2.run (l1.run (h2.run (h1.run (g.next())))); };
    for (long long i = 0; i < (long long) std::llround (kWarmS * sr); ++i) one();
    std::vector<double> v ((size_t) std::max (0LL, n)); double ss = 0.0;
    for (auto& x : v) { x = one(); ss += x * x; }
    const double rms = v.empty() ? 1.0 : std::sqrt (ss / (double) v.size()); const double k = rms > 0.0 ? rmsTarget / rms : 0.0;
    for (auto& x : v) x *= k;
    return v;
}
// PINK NOISE (7 Oct, REVERB_DELAY_PROFILE_SPEC v0.1 section 4: the decay's broadband burst): seeded Gaussian white noise through Paul
// Kellet's three-pole pink filter (-3 dB/oct within about 0.5 dB from 10 Hz to 20 kHz at 48 kHz), the filter warmed for kWarmS, the block
// normalised to the asked RMS. Deterministic in the seed, like bandLimited.
inline std::vector<double> pink (double sr, long long n, unsigned long long seed, double rmsTarget)
{
    (void) sr;
    Gauss g (seed); double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    auto one = [&] { const double w = g.next(); b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913; return b0 + b1 + b2 + w * 0.1848; };
    for (long long i = 0; i < (long long) std::llround (kWarmS * 48000.0); ++i) one();
    std::vector<double> v ((size_t) std::max (0LL, n)); double ss = 0.0;
    for (auto& x : v) { x = one(); ss += x * x; }
    const double rms = v.empty() ? 1.0 : std::sqrt (ss / (double) v.size()); const double k = rms > 0.0 ? rmsTarget / rms : 0.0;
    for (auto& x : v) x *= k;
    return v;
}
// the power at one frequency (Goertzel), in dB, for the suite's band check
inline double powerDbAt (const std::vector<double>& v, double sr, double hz)
{
    const double coef = 2.0 * std::cos (2.0 * kPi * hz / sr); double s1 = 0.0, s2 = 0.0;
    for (double x : v) { const double s0 = x + coef * s1 - s2; s2 = s1; s1 = s0; }
    const double m2 = s1 * s1 + s2 * s2 - coef * s1 * s2; return 10.0 * std::log10 (std::max (1e-30, m2 / ((double) v.size() * (double) v.size())));
}

} // namespace ejprobe::noise
