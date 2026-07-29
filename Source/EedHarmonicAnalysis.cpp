/*
    EedHarmonicAnalysis.cpp  —  see EedHarmonicAnalysis.h.
*/

#include "EedHarmonicAnalysis.h"

#include <algorithm>
#include <cmath>

namespace echojay::harmonic
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // The rate the coarse pass runs at. Low enough that the autocorrelation is
    // cheap, high enough that its Nyquist is well clear of the top of the search
    // range so the decimation cannot fold a harmonic down onto the fundamental.
    constexpr double kCoarseRate = 6000.0;

    // Below this the frame is noise, not a note.
    constexpr float kLockClarity = 0.45f;
    constexpr float kLockRms     = 1.0e-4f;   // about -80 dBFS

    // Normalised autocorrelation at one lag. Normalised by BOTH windows' energy
    // rather than by the frame's, so the number is a correlation coefficient in
    // -1..1 and a quiet tail cannot make a lag look more periodic than it is.
    double normalisedAcf (const float* x, int n, int lag) noexcept
    {
        const int len = n - lag;
        if (len <= 0) return 0.0;

        double num = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double a = (double) x[i], b = (double) x[i + lag];
            num += a * b;
            e1  += a * a;
            e2  += b * b;
        }

        const double den = std::sqrt (e1 * e2);
        return den > 1.0e-20 ? num / den : 0.0;
    }

    // Sub-sample peak position from three samples around a maximum. Returns the
    // offset in -1..+1 to add to the centre index.
    double parabolicOffset (double yLeft, double yMid, double yRight) noexcept
    {
        const double denom = yLeft - 2.0 * yMid + yRight;
        if (std::fabs (denom) < 1.0e-20) return 0.0;
        const double d = 0.5 * (yLeft - yRight) / denom;
        return std::max (-1.0, std::min (1.0, d));
    }
}

// ---------------------------------------------------------------------------
double goertzelMagnitude (const float* x, int numSamples,
                          double sampleRate, double freqHz) noexcept
{
    if (x == nullptr || numSamples <= 0 || sampleRate <= 0.0
        || freqHz <= 0.0 || freqHz >= sampleRate * 0.5)
        return 0.0;

    const double w    = 2.0 * kPi * freqHz / sampleRate;
    const double coef = 2.0 * std::cos (w);

    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double s = (double) x[i] + coef * s1 - s2;
        s2 = s1;
        s1 = s;
    }

    const double mag2 = s1 * s1 + s2 * s2 - coef * s1 * s2;

    // Scaled by 2/N so the result is the AMPLITUDE of the component, independent
    // of how long a frame the caller happened to pass.
    return std::sqrt (std::max (0.0, mag2)) * (2.0 / (double) numSamples);
}

// ---------------------------------------------------------------------------
int wholeCycleLength (int maxSamples, double sampleRate, double freqHz) noexcept
{
    if (maxSamples <= 0 || sampleRate <= 0.0 || freqHz <= 0.0) return 0;

    const double period = sampleRate / freqHz;
    if (period <= 1.0) return 0;

    const double cycles = std::floor ((double) maxSamples / period);
    if (cycles < 1.0) return 0;

    const int n = (int) std::lround (cycles * period);
    return std::min (maxSamples, std::max (0, n));
}

// ---------------------------------------------------------------------------
FundamentalEstimate estimateFundamental (const float* mono, int numSamples,
                                         double sampleRate,
                                         float minHz, float maxHz)
{
    FundamentalEstimate out;

    if (mono == nullptr || numSamples < 256 || sampleRate <= 0.0
        || minHz <= 0.0f || maxHz <= minHz)
        return out;

    // ---- decimate ----------------------------------------------------------
    // A box average over D samples is both the anti-alias filter and the
    // decimator. Its nulls sit at multiples of the decimated rate, which is
    // exactly where anything that would fold onto the search band comes from.
    const int decim = std::max (1, (int) std::lround (sampleRate / kCoarseRate));
    const int m     = numSamples / decim;
    if (m < 64) return out;

    const double coarseRate = sampleRate / (double) decim;

    std::vector<float> x ((std::size_t) m);
    for (int i = 0; i < m; ++i)
    {
        float acc = 0.0f;
        for (int k = 0; k < decim; ++k) acc += mono[i * decim + k];
        x[(std::size_t) i] = acc / (float) decim;
    }

    // Remove the mean. A DC offset makes every lag correlate with every other
    // one, so a frame with any offset reads as perfectly periodic at whatever
    // lag is checked first.
    double mean = 0.0;
    for (float v : x) mean += (double) v;
    mean /= (double) m;

    double energy = 0.0;
    for (auto& v : x) { v -= (float) mean; energy += (double) v * (double) v; }

    // The RMS is measured on the FULL-RATE frame, not the decimated one: the
    // decimator is a low-pass, and a bright quiet signal would read louder than
    // it is if measured after it.
    {
        double e = 0.0;
        for (int i = 0; i < numSamples; ++i) e += (double) mono[i] * (double) mono[i];
        out.rms = (float) std::sqrt (e / (double) numSamples);
    }

    if (out.rms < kLockRms || energy <= 0.0) return out;

    // ---- coarse pass -------------------------------------------------------
    const int lagMin = std::max (2, (int) std::floor (coarseRate / (double) maxHz));
    const int lagMax = std::min (m / 2, (int) std::ceil (coarseRate / (double) minHz));
    if (lagMax <= lagMin + 2) return out;

    std::vector<double> r ((std::size_t) (lagMax + 1), 0.0);
    double bestR = -1.0;
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        r[(std::size_t) lag] = normalisedAcf (x.data(), m, lag);
        bestR = std::max (bestR, r[(std::size_t) lag]);
    }

    if (bestR <= 0.0) return out;

    // Sub-octave guard: take the FIRST local maximum that is within a whisker of
    // the best one. A periodic signal peaks just as hard at 2T and 3T as at T,
    // and "pick the global maximum" resolves that coin-flip at random — which is
    // the classic octave error, and it flickers frame to frame.
    const double threshold = 0.90 * bestR;
    int coarseLag = -1;
    for (int lag = lagMin + 1; lag < lagMax; ++lag)
    {
        const double v = r[(std::size_t) lag];
        if (v >= threshold && v >= r[(std::size_t) (lag - 1)] && v >= r[(std::size_t) (lag + 1)])
        {
            coarseLag = lag;
            break;
        }
    }
    if (coarseLag < 0) return out;

    const double coarseRefined = (double) coarseLag
        + parabolicOffset (r[(std::size_t) (coarseLag - 1)],
                           r[(std::size_t) coarseLag],
                           r[(std::size_t) (coarseLag + 1)]);

    // ---- fine pass ---------------------------------------------------------
    // The coarse lag is only accurate to a decimated sample, which is `decim`
    // full-rate samples. Searching that neighbourhood at the full rate turns an
    // error of about 1% into one of about 0.05% — and since the harmonic bins
    // sit at k*f0, the top bar sees that error multiplied by eight.
    const int centre = (int) std::lround (coarseRefined * (double) decim);
    const int fineLo = std::max (2, centre - decim - 1);
    const int fineHi = std::min (numSamples / 2 - 1, centre + decim + 1);
    if (fineHi <= fineLo + 1) return out;

    int    bestLag  = fineLo;
    double bestFine = -2.0;
    for (int lag = fineLo; lag <= fineHi; ++lag)
    {
        const double v = normalisedAcf (mono, numSamples, lag);
        if (v > bestFine) { bestFine = v; bestLag = lag; }
    }

    double lag = (double) bestLag;
    if (bestLag > fineLo && bestLag < fineHi)
        lag += parabolicOffset (normalisedAcf (mono, numSamples, bestLag - 1),
                                bestFine,
                                normalisedAcf (mono, numSamples, bestLag + 1));

    if (lag <= 1.0) return out;

    out.hz      = (float) (sampleRate / lag);
    out.clarity = (float) std::max (0.0, bestFine);
    out.locked  = out.clarity >= kLockClarity
               && out.hz >= minHz && out.hz <= maxHz;

    return out;
}

// ---------------------------------------------------------------------------
void harmonicMagnitudesDb (const float* signal,
                           const float* reference,
                           int numSamples,
                           double sampleRate,
                           double fundamentalHz,
                           float* outDb, int numHarmonics,
                           float floorDb)
{
    if (outDb == nullptr || numHarmonics <= 0) return;

    for (int i = 0; i < numHarmonics; ++i) outDb[i] = floorDb;

    if (signal == nullptr || reference == nullptr
        || numSamples <= 0 || sampleRate <= 0.0 || fundamentalHz <= 0.0)
        return;

    const double ref = goertzelMagnitude (reference, numSamples, sampleRate, fundamentalHz);

    // No fundamental to measure against means no ratio. Report the floor rather
    // than a shape computed by dividing by noise.
    if (ref < 1.0e-6) return;

    for (int h = 0; h < numHarmonics; ++h)
    {
        const double f = fundamentalHz * (double) (h + 1);
        if (f >= sampleRate * 0.5) break;      // above Nyquist: leave it at the floor

        const double m = goertzelMagnitude (signal, numSamples, sampleRate, f);
        if (m <= 1.0e-9) continue;

        outDb[h] = (float) std::max ((double) floorDb, 20.0 * std::log10 (m / ref));
    }
}

} // namespace echojay::harmonic
