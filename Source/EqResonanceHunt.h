/*
    EqResonanceHunt.h  —  the resonance detector behind the EQ's
    `eq_action: tame_resonances` (P3 of SURGICAL_EQ_ENHANCEMENTS.md).

    Deliberately JUCE-free, like EqEngine/EqMove, so the detection logic is
    unit-testable under plain g++ (test/eq_hunt_test.cpp). The processor feeds
    it the PRE-EQ capture from its analysis ring and turns the peaks it returns
    into bands through the ordinary applyEqMoves merge; nothing here touches
    the engine.

    Method
    ------
    Welch-averaged long-window FFT → fractional-octave smoothing at two widths:
    a FINE trace (≈1/24 oct — resonances survive) and a wide ENVELOPE
    (≈2/3 oct — resonances are ironed out, the broadband shape remains). A
    resonance is a local maximum of the fine trace standing at least `marginDb`
    above the envelope: prominence over the local spectral floor, not absolute
    level, which is what makes the detector indifferent to overall tilt and
    broadband content. Candidates are ranked by prominence, thinned to a
    minimum spacing, and capped.

    Levels are AMPLITUDE-calibrated (a full-scale sine reads ≈ 0 dBFS) so the
    envelope level can be handed straight to a dynamic band's threshold — the
    engine's detector measures the band-passed sine amplitude in the same
    units.
*/

#pragma once

#include "EqFft.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace echojay
{

struct ResonancePeak
{
    float freqHz       = 0.0f;
    float prominenceDb = 0.0f;   // how far it stands above the local envelope
    float q            = 8.0f;   // from the peak's half-prominence bandwidth
    float levelDb      = 0.0f;   // peak amplitude, dBFS-calibrated
    float envelopeDb   = 0.0f;   // the local spectral floor it stands on
};

struct ResonanceHuntParams
{
    float marginDb      = 4.0f;          // sensitivity: low≈6, medium≈4, high≈2.5
    float loHz          = 80.0f;
    float hiHz          = 12000.0f;
    int   maxPeaks      = 4;
    float minSpacingOct = 1.0f / 3.0f;
};

// Analyse `n` samples at `fs`; write up to `maxOut` peaks ordered by
// prominence (strongest first). Returns how many were found. Returns 0 for
// silence or for content with no resonant structure — finding nothing is a
// valid, reportable answer, not an error.
inline int findResonances (const float* x, int n, double fs,
                           const ResonanceHuntParams& p,
                           ResonancePeak* out, int maxOut)
{
    if (x == nullptr || n < 2048 || fs <= 0.0 || out == nullptr || maxOut <= 0)
        return 0;

    // ---- silence gate ------------------------------------------------------
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) sumSq += (double) x[i] * (double) x[i];
    if (std::sqrt (sumSq / (double) n) < 1.0e-5)         // < ~-100 dBFS RMS
        return 0;

    // ---- Welch-averaged power spectrum -------------------------------------
    // 8192-point windows at 50% overlap over whatever was captured; a shorter
    // capture degrades to fewer (>= 1) averages rather than failing.
    const int fftSize = std::min (8192, 1 << (int) std::floor (std::log2 ((double) n)));
    const int hop     = fftSize / 2;
    const int bins    = fftSize / 2 + 1;

    std::vector<float> window ((size_t) fftSize);
    double windowSum = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        window[(size_t) i] = 0.5f * (1.0f - (float) std::cos (2.0 * 3.14159265358979323846
                                                              * (double) i / (double) fftSize));
        windowSum += (double) window[(size_t) i];
    }

    std::vector<double> power ((size_t) bins, 0.0);
    std::vector<float>  re ((size_t) fftSize), im ((size_t) fftSize);

    int frames = 0;
    for (int start = 0; start + fftSize <= n; start += hop)
    {
        for (int i = 0; i < fftSize; ++i)
        {
            re[(size_t) i] = x[start + i] * window[(size_t) i];
            im[(size_t) i] = 0.0f;
        }
        eqFft (re.data(), im.data(), fftSize, false);
        for (int k = 0; k < bins; ++k)
            power[(size_t) k] += (double) re[(size_t) k] * re[(size_t) k]
                               + (double) im[(size_t) k] * im[(size_t) k];
        ++frames;
    }
    if (frames == 0) return 0;

    // Amplitude calibration: |X| * 2 / sum(w) makes a full-scale sine ≈ 1.
    const double ampNorm = 2.0 / windowSum;
    std::vector<float> db ((size_t) bins);
    for (int k = 0; k < bins; ++k)
    {
        const double amp = std::sqrt (power[(size_t) k] / (double) frames) * ampNorm;
        db[(size_t) k] = (float) (20.0 * std::log10 (std::max (amp, 1.0e-10)));
    }

    // ---- fractional-octave smoothing at two widths -------------------------
    // Same prefix-sum trick the analyzer uses: O(bins) regardless of how many
    // bins the top octave's window spans.
    auto octaveSmooth = [&] (const std::vector<float>& src, float octFrac,
                             std::vector<float>& dst)
    {
        std::vector<float> prefix ((size_t) bins + 1, 0.0f);
        for (int k = 0; k < bins; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + src[(size_t) k];

        const float half  = octFrac * 0.5f;
        const float loMul = std::pow (2.0f, -half);
        const float hiMul = std::pow (2.0f,  half);
        dst.assign ((size_t) bins, 0.0f);
        for (int k = 0; k < bins; ++k)
        {
            int lo = (int) std::floor ((float) k * loMul);
            int hi = (int) std::ceil  ((float) k * hiMul);
            lo = std::max (0, std::min (bins - 1, lo));
            hi = std::max (lo, std::min (bins - 1, hi));
            dst[(size_t) k] = (prefix[(size_t) hi + 1] - prefix[(size_t) lo])
                            / (float) (hi - lo + 1);
        }
    };

    std::vector<float> fine, envelope;
    octaveSmooth (db, 1.0f / 24.0f, fine);      // resonances survive
    octaveSmooth (db, 2.0f / 3.0f,  envelope);  // resonances ironed flat

    // ---- peak picking ------------------------------------------------------
    const double binHz = fs / (double) fftSize;
    const int kLo = std::max (1, (int) std::ceil  ((double) p.loHz / binHz));
    const int kHi = std::min (bins - 2, (int) std::floor ((double) p.hiHz / binHz));

    struct Cand { int bin; float prom; };
    std::vector<Cand> cands;
    for (int k = kLo; k <= kHi; ++k)
    {
        const float v = fine[(size_t) k];
        if (v <= fine[(size_t) k - 1] || v < fine[(size_t) k + 1]) continue;   // not a local max
        const float prom = v - envelope[(size_t) k];
        if (prom < p.marginDb) continue;
        cands.push_back ({ k, prom });
    }

    std::sort (cands.begin(), cands.end(),
               [] (const Cand& a, const Cand& b) { return a.prom > b.prom; });

    // ---- thin by spacing, cap, and measure each keeper ---------------------
    const int cap = std::min (maxOut, std::max (1, p.maxPeaks));
    std::vector<int> kept;
    int found = 0;

    for (const Cand& c : cands)
    {
        if (found >= cap) break;

        bool tooClose = false;
        for (int kb : kept)
        {
            const float dist = std::fabs (std::log2 ((float) c.bin / (float) kb));
            if (dist < p.minSpacingOct) { tooClose = true; break; }
        }
        if (tooClose) continue;
        kept.push_back (c.bin);

        // Q from sharpness: the half-prominence width around the peak. Walk
        // outward on the fine trace until it has fallen halfway back to the
        // envelope; that span is the resonance's bandwidth.
        const float halfDown = fine[(size_t) c.bin] - c.prom * 0.5f;
        int lo = c.bin, hi = c.bin;
        while (lo > kLo      && fine[(size_t) (lo - 1)] > halfDown) --lo;
        while (hi < kHi      && fine[(size_t) (hi + 1)] > halfDown) ++hi;
        const float bwHz = (float) ((hi - lo + 1) * binHz);
        const float freq = (float) (c.bin * binHz);
        const float q    = std::max (1.0f, std::min (36.0f,
                                     freq / std::max (bwHz, (float) binHz)));

        ResonancePeak& r = out[found++];
        r.freqHz        = freq;
        r.prominenceDb  = c.prom;
        r.q             = q;
        r.levelDb       = fine[(size_t) c.bin];
        r.envelopeDb    = envelope[(size_t) c.bin];
    }

    return found;
}

// The sensitivity names the schema advertises, resolved to margins here so the
// UI, the action parser and the docs cannot disagree about what "medium" is.
inline float resonanceMarginForSensitivity (int index) noexcept   // 0=low 1=medium 2=high
{
    switch (index)
    {
        case 0:  return 6.0f;
        case 2:  return 2.5f;
        case 1:
        default: return 4.0f;
    }
}

} // namespace echojay
