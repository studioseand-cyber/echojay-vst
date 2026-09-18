#pragma once
// EJTruePeakInterp (18 Sep 2026): a 4x oversampling interpolator - 4 phases of a 24-tap Blackman-windowed sinc
// (the same formulation the guards' independent meters use, so a product reading and an independent reading of the
// same buffer agree within a tenth of a dB) - returning the largest |value| among the sample and its three
// inter-sample points. Group delay kDelay = 12 samples; the limiter's lookahead delay absorbs it and reports it.
// Fixed arrays, no allocation, audio-thread safe.
#include <cmath>
#include <algorithm>
namespace echojay {
struct TruePeakInterp
{
    static constexpr int kPhases = 4, kTaps = 24, kDelay = 12;
    float coef[kPhases][kTaps] {}; float hist[kTaps] {}; int pos = 0; bool ready = false;
    void prepare() noexcept
    {
        const double pi = 3.14159265358979323846;
        for (int ph = 0; ph < kPhases; ++ph)
        {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double x = ((double) k - 11.5) - (double) ph / (double) kPhases + 0.5;
                const double sinc = x == 0.0 ? 1.0 : std::sin (pi * x) / (pi * x);
                const double w = 0.42 - 0.5 * std::cos (2.0 * pi * ((double) k + 0.5) / (double) kTaps) + 0.08 * std::cos (4.0 * pi * ((double) k + 0.5) / (double) kTaps);
                coef[ph][k] = (float) (sinc * w); sum += sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) coef[ph][k] = (float) (coef[ph][k] / sum);
        }
        for (auto& h : hist) h = 0.0f; pos = 0; ready = true;
    }
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
