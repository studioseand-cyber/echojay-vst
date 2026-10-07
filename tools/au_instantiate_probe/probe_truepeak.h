// probe_truepeak.h - THE TRUE-PEAK ESTIMATE (ITU-R BS.1770-4 Annex 2's method: 4x oversampling, then the sample peak of the
// oversampled signal). LIMITER_PROFILE_SPEC v0.1 section 3 asks for "true peak (ITU-R BS.1770, 4x oversampled)"; until 7 Oct
// 2026 the probe's `out_true_peak_db` was a 4x Catmull-Rom cubic between consecutive samples, said as an approximation.
//
// THE INTERPOLATOR: a 4x polyphase FIR, kTapsPerPhase taps per phase (4 x 48 = 192 taps at the oversampled rate), a sinc at
// the input's Nyquist (zeros every 4 oversampled samples) under a Kaiser window (beta kKaiserBeta, about -80 dB stopband),
// every phase normalised to unity DC gain. Flat within 0.1 dB to about 20 kHz at 48 kHz; what remains is the 4x bound itself
// (an inter-sample peak can still sit between two oversampled points: at most 20 log10 cos (pi f / (4 fs)) under the true
// amplitude - 0.001 dB at 997 Hz, 0.47 dB at 20 kHz), which is BS.1770's own limit and is said in the record's method.
// BS.1770's Annex 2 coefficients are not copied here: a table recalled wrongly would read 0.1 dB off on a 0.1 dB bar; a
// filter designed in code is checked by the suite (RoundTripTest testTruePeak) on sines whose sample peak sits between samples.
//
// STANDALONE (std only): the probe renders with it and the suite pins it from tools/ejmap/tests by relative include.
#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace ejprobe::truepeak
{

inline constexpr int kOversample = 4;
inline constexpr int kTapsPerPhase = 48;
inline constexpr double kKaiserBeta = 8.0;
inline constexpr double kPi = 3.14159265358979323846;

inline double besselI0 (double x)
{
    double sum = 1.0, term = 1.0; const double q = x * x / 4.0;
    for (int k = 1; k < 60; ++k) { term *= q / ((double) k * (double) k); sum += term; if (term < 1e-14 * sum) break; }
    return sum;
}

// phases[p][k] multiplies x[n - k] to give the oversampled point p/4 of a sample AFTER the filter's group delay: y[4n + p]
struct Phases { std::array<std::array<double, kTapsPerPhase>, kOversample> h {}; };
inline const Phases& phases()
{
    static const Phases ph = []
    {
        Phases out; const int N = kOversample * kTapsPerPhase; const double centre = (N - 1) / 2.0;
        for (int p = 0; p < kOversample; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < kTapsPerPhase; ++k)
            {
                const int m = kOversample * k + p;                       // the tap's index in the 192-tap prototype
                const double x = (m - centre) / (double) kOversample;    // in INPUT samples from the centre
                const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x);
                const double r = (m - centre) / centre;                  // -1..1 across the window
                const double kaiser = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (kKaiserBeta);
                out.h[(size_t) p][(size_t) k] = sinc * kaiser; sum += sinc * kaiser;
            }
            for (auto& v : out.h[(size_t) p]) v /= sum;                 // unity DC gain per phase
        }
        return out;
    }();
    return ph;
}

// ONE CHANNEL's running estimate: push EVERY sample from the start of the render (the filter must be warm - a history of zeros
// in front of a running sine is a step, and the interpolator rings on it by +0.2 dB), then resetPeaks() where the measured span
// begins and read the largest absolute oversampled value from there (and the plain sample peak).
struct Tracker
{
    std::array<double, kTapsPerPhase> hist {}; int head = 0;
    double truePeak = 0.0, samplePeak = 0.0;
    void resetPeaks() { truePeak = 0.0; samplePeak = 0.0; }
    void push (double x)
    {
        samplePeak = std::max (samplePeak, std::abs (x));
        hist[(size_t) head] = x; head = (head + 1) % kTapsPerPhase;
        const auto& ph = phases();
        for (int p = 0; p < kOversample; ++p)
        {
            double y = 0.0; int i = head;                                // hist[head] is the oldest = x[n - (T-1)] ... walk newest first
            for (int k = 0; k < kTapsPerPhase; ++k) { i = (i + kTapsPerPhase - 1) % kTapsPerPhase; y += ph.h[(size_t) p][(size_t) k] * hist[(size_t) i]; }
            truePeak = std::max (truePeak, std::abs (y));
        }
        truePeak = std::max (truePeak, samplePeak);                      // an oversampled point never sits under a sample it passes through
    }
};

// whole-buffer form (the suite): the true peak of a vector, the tracker run over it; `warm` samples at the front are pushed but not read
inline double truePeakOf (const std::vector<double>& x, size_t warm = 0) { Tracker t; for (size_t i = 0; i < x.size(); ++i) { if (i == warm) t.resetPeaks(); t.push (x[i]); } return t.truePeak; }
inline double toDb (double a) { return a > 0.0 ? 20.0 * std::log10 (a) : -999.0; }

// THE BS.1770-4 REFERENCE (Annex 2's 48-tap, 4-phase interpolating FIR; 7 Oct follow-up 2): the cross-check for the filter above.
// TRANSCRIBED FROM THE PUBLISHED TABLE AS RECALLED - no download was made. Its published structure is pinned (phase 3 is phase 0
// reversed, phase 2 is phase 1 reversed), and the suite pins this filter and the designed one against each other and against generated
// Tech 3341-style true-peak cases; a transcription slip large enough to matter shows there. Note the table's own property: phase 1 and
// phase 2 sum to 0.973 (-0.24 dB at DC), phase 0 and 3 to 1.0016 - the standard's filter, not a transcription artefact.
inline constexpr int kRefTaps = 12;
inline const std::array<std::array<double, kRefTaps>, 4>& referencePhases()
{
    static const std::array<std::array<double, kRefTaps>, 4> h { {
        { 0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625, 0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500 },
        { -0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250, 0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375 },
        { -0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125, 0.7797851562500, 0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875 },
        { -0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750, 0.9721679687500, 0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750 } } };
    return h;
}
struct ReferenceTracker
{
    std::array<double, kRefTaps> hist {}; int head = 0; double truePeak = 0.0;
    void resetPeaks() { truePeak = 0.0; }
    void push (double x)
    {
        hist[(size_t) head] = x; head = (head + 1) % kRefTaps;
        for (const auto& ph : referencePhases())
        {
            double y = 0.0; int i = head;
            for (int k = 0; k < kRefTaps; ++k) { i = (i + kRefTaps - 1) % kRefTaps; y += ph[(size_t) k] * hist[(size_t) i]; }
            truePeak = std::max (truePeak, std::abs (y));
        }
    }
};
inline double referenceTruePeakOf (const std::vector<double>& x, size_t warm = 0) { ReferenceTracker t; for (size_t i = 0; i < x.size(); ++i) { if (i == warm) t.resetPeaks(); t.push (x[i]); } return t.truePeak; }

} // namespace ejprobe::truepeak
