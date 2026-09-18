#pragma once
// EJTruePeakInterp (18 Sep 2026): a 4x oversampling interpolator (4-phase windowed-sinc FIR, 17 taps per phase)
// that returns the largest |value| among the sample and its three inter-sample points - the true-peak detector the
// limiter uses when true_peak is on. Fixed arrays, no allocation, audio-thread safe.
#include <cmath>
#include <algorithm>
namespace echojay {
struct TruePeakInterp
{
    static constexpr int kPhases = 4, kTaps = 33;   // 33 taps, Blackman: the truncated sinc under-read the peak by ~0.3 dB at 17 taps / Hann
    float coef[kPhases][kTaps] {}; float hist[kTaps] {}; int pos = 0; bool ready = false;
    void prepare() noexcept
    {
        const int half = kTaps / 2; const double pi = 3.14159265358979323846;
        for (int ph = 0; ph < kPhases; ++ph)
        {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double x = (double) (k - half) - (double) ph / (double) kPhases;
                const double sinc = x == 0.0 ? 1.0 : std::sin (pi * x) / (pi * x);
                const double w = 0.42 - 0.5 * std::cos (2.0 * pi * ((double) k + 0.5) / (double) kTaps) + 0.08 * std::cos (4.0 * pi * ((double) k + 0.5) / (double) kTaps);   // Blackman
                coef[ph][k] = (float) (sinc * w); sum += sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) coef[ph][k] = (float) (coef[ph][k] / sum);
        }
        for (auto& h : hist) h = 0.0f; pos = 0; ready = true;
    }
    // Push one sample, return max |interpolated| over the 4 phases centred on the history (a kTaps/2-sample delay
    // relative to the input; the caller's lookahead window absorbs it).
    inline float maxAbs4 (float x) noexcept
    {
        hist[pos] = x; float m = 0.0f;
        for (int ph = 0; ph < kPhases; ++ph)
        {
            float a = 0.0f; int idx = pos;
            for (int k = 0; k < kTaps; ++k) { a += coef[ph][k] * hist[idx]; idx = idx == 0 ? kTaps - 1 : idx - 1; }
            m = std::max (m, std::abs (a));
        }
        pos = (pos + 1) % kTaps;
        return m;
    }
};
}
