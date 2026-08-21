#pragma once
// ===========================================================================
// BS.1770-4 K-weighting: the ONE copy of the numbers.
//
// MeterEngine (the post-chain display / capture meters) and LevelTally (the
// running level instrument at every point of the chain) both weight audio
// with this filter before they measure loudness. The coefficients used to
// live inside MeterEngine; they are here so the two instruments cannot
// drift apart on the one thing that makes their LUFS mean the same thing.
// Header-only, no JUCE dependency beyond <cmath>.
// ===========================================================================
#include <cmath>

// Biquad coefficients (direct form), shared shape for every filter stage.
struct BiquadCoeffs {
    double b0, b1, b2, a1, a2;
};

namespace echojay {

// Stage 1: the high-frequency shelf. Stage 2: the RLB high-pass.
// Constants are the ITU-R BS.1770-4 pre-filter definitions at 48 kHz,
// re-derived for the given sample rate.
inline void computeKWeightingCoeffs (double sr, BiquadCoeffs& stage1, BiquadCoeffs& stage2)
{
    const double pi = 3.14159265358979323846;
    {
        const double f0 = 1681.974450955533;
        const double G  = 3.999843853973347;
        const double Q  = 0.7071752369554196;
        const double K  = std::tan (pi * f0 / sr);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        stage1.b0 = (Vh + Vb * K / Q + K * K) / a0;
        stage1.b1 = 2.0 * (K * K - Vh) / a0;
        stage1.b2 = (Vh - Vb * K / Q + K * K) / a0;
        stage1.a1 = 2.0 * (K * K - 1.0) / a0;
        stage1.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f1 = 38.13547087602444;
        const double Q1 = 0.5003270373238773;
        const double K1 = std::tan (pi * f1 / sr);
        const double a0 = 1.0 + K1 / Q1 + K1 * K1;
        stage2.b0 = 1.0 / a0;
        stage2.b1 = -2.0 / a0;
        stage2.b2 = 1.0 / a0;
        stage2.a1 = 2.0 * (K1 * K1 - 1.0) / a0;
        stage2.a2 = (1.0 - K1 / Q1 + K1 * K1) / a0;
    }
}

} // namespace echojay
