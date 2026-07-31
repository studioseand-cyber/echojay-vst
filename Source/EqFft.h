/*
    EqFft.h  —  minimal iterative radix-2 complex FFT, JUCE-free.

    Shared by the EQ's linear-phase convolver (EqEngine.cpp) and the
    resonance-hunt detector (EqResonanceHunt.h). juce::dsp::FFT cannot be used
    in either place because both are part of the JUCE-free, g++-tested DSP core
    — and the two callers must agree on one implementation rather than each
    carrying its own.

    Allocation-free and in-place, so it is safe on the audio thread once the
    caller's buffers exist. Twiddles are recurrence-generated in double and
    applied in float, which keeps the error of an 8192-point transform well
    under the float quantisation the samples already carry.
*/

#pragma once

#include <cmath>
#include <utility>

namespace echojay
{

// n must be a power of two. inverse == true applies the 1/n scale, so
// fft(ifft(x)) == x without caller-side bookkeeping.
inline void eqFft (float* re, float* im, int n, bool inverse) noexcept
{
    // bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }

    constexpr double kTwoPi = 6.28318530717958647692;
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = (inverse ? kTwoPi : -kTwoPi) / (double) len;
        const double wr = std::cos (ang), wi = std::sin (ang);

        for (int i = 0; i < n; i += len)
        {
            double cwr = 1.0, cwi = 0.0;
            const int half = len >> 1;
            for (int k = 0; k < half; ++k)
            {
                const int a = i + k, b = a + half;
                const float ur = re[a], ui = im[a];
                const float vr = (float) (re[b] * cwr - im[b] * cwi);
                const float vi = (float) (re[b] * cwi + im[b] * cwr);
                re[a] = ur + vr; im[a] = ui + vi;
                re[b] = ur - vr; im[b] = ui - vi;
                const double nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }

    if (inverse)
    {
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
    }
}

} // namespace echojay
