#pragma once

#include <array>
#include <cmath>

// THE BAND SCHEME AND THE BALLISTICS, as named constants and pure functions.
//
// WHY THIS HEADER EXISTS, WHICH IS A MEASUREMENT ARGUMENT AND NOT A TIDINESS
// ONE. Phase 1b of the Match Reference plan is a sequence of measurements: the
// macro bands stop being a 150 ms ballistic tail and become a windowed figure,
// and the change is judged by comparing the same pair before and after. A
// measurement of something that is not nailed down is not evidence. Before this
// header, changing 250.0 to 200.0 in the macro band edges reddened NOTHING in
// any suite, and no pin anywhere named a band edge or a ballistic coefficient.
// A number that can move silently cannot be the baseline of anything.
//
// NOTHING HERE CHANGES A VALUE. Every constant is the number that was already
// compiled, every expression is the same arithmetic in the same order and the
// same types. The point is that they now have ONE definition with a name, and a
// pin can state what they are.
//
// THE TILT KNEE IS NOT IN HERE, deliberately. MeterEngine.cpp's display tilt
// turns at 60 Hz, which is also the sub/low boundary, and the two are equal by
// coincidence rather than by rule: one is a visual shaping of the 64 display
// bins, the other is a boundary in the serialised pink-referenced measurement.
// Sharing a constant between them would make a later edit to either silently
// move the other, which is the defect class this header exists to close.
namespace echojay {

// ===========================================================================
// THE SIX MACRO BANDS (MeterEngine::computeSpectrum, the pink-referenced path)
// ===========================================================================
//
// THE TOP IS NOT A CONSTANT and cannot be. The sixth band ends at maxFreq,
// which is min(sampleRate * 0.5, 20000), so below a 40 kHz sample rate the air
// band's ceiling moves with the rate. The table therefore holds the six LOWER
// bounds, which are fixed, and the ceiling arrives as an argument.
inline constexpr std::array<double, 6> kMacroBandLoHz {
    20.0,     // sub
    60.0,     // low
    250.0,    // lowMid
    500.0,    // mid
    2000.0,   // highMid
    6000.0    // air
};

/** The six band names, in band order, as the serialiser and the prose use
    them. Kept beside the edges so a band cannot be renamed away from its
    boundary. */
inline const char* macroBandName (int i) noexcept
{
    static const char* n[6] = { "sub", "low", "lowMid", "mid", "highMid", "air" };
    return (i >= 0 && i < 6) ? n[i] : "";
}

struct MacroBandEdge { double lo, hi; };

/** The six bands, given the run's frequency ceiling. Each band runs from its
    own lower bound to the NEXT band's lower bound; the last runs to topHz.

    Half-open, [lo, hi), which is what the integration loop tests and what makes
    the bands a partition rather than a set of overlapping ranges. */
inline std::array<MacroBandEdge, 6> macroBandEdges (double topHz)
{
    std::array<MacroBandEdge, 6> e {};
    for (int i = 0; i < 5; ++i)
        e[(std::size_t) i] = { kMacroBandLoHz[(std::size_t) i],
                               kMacroBandLoHz[(std::size_t) i + 1] };
    e[(std::size_t) 5] = { kMacroBandLoHz[(std::size_t) 5], topHz };
    return e;
}

// ===========================================================================
// THE 64 DISPLAY BINS (the OTHER six-band scheme's underlying axis)
// ===========================================================================
//
// The 64 analysis bins are logarithmic from minFreq to maxFreq, and bin b spans
// [edge(b), edge(b+1)). EJSpectralEvidence::computeBands groups those bins into
// six bands by INDEX (0-9, 10-20, ...), so the Hz boundaries of that scheme are
// not written anywhere: they are implied by this formula and can only be
// COMPUTED. That is the whole reason this is extracted -- a pin can now state
// what the bin scheme's real edges are instead of trusting a label.
//
// TWO ENTRY POINTS, ONE ARITHMETIC. The hot path already hoists log2(minFreq)
// and log2(maxFreq) out of its loop, and computeSpectrum runs a full 2048-point
// FFT on the audio thread every block with no hop gate, so a self-contained
// form recomputing both logs per bin would add 128 transcendental calls per
// block. Bit-identical, and still a regression. The logs-taking form is what
// the hot path calls; the self-contained form computes the logs and delegates,
// so there is one expression and no copy to drift.

/** Bin b's LOWER edge, in Hz, from pre-computed log2 bounds. */
inline double specBinEdgeHzFromLogs (int b, int numBins, double logMin, double logMax)
{
    return std::pow (2.0, logMin + (logMax - logMin) * (double) b / (double) numBins);
}

/** Bin b's LOWER edge, in Hz. b == numBins gives the top of the last bin.

    maxFreq is the run's ceiling, min(sampleRate * 0.5, 20000), NOT a constant:
    every edge below a 40 kHz sample rate sits somewhere else. */
inline double specBinEdgeHz (int b, int numBins, double minFreq, double maxFreq)
{
    return specBinEdgeHzFromLogs (b, numBins, std::log2 (minFreq), std::log2 (maxFreq));
}

// ===========================================================================
// THE BALLISTICS
// ===========================================================================
//
// The macro bands and the display spectrum share one asymmetric one-pole: fast
// up, slow down. The TIME CONSTANTS are the contract and the coefficient is
// derived from the block duration, so the span does not move with the host's
// buffer size -- only the coefficient does. Stating that here, with the two
// constants named, is what lets a pin assert the four (rate, block) cases the
// Phase 1b survey computed.
//
// THE TYPES ARE PART OF THE ARITHMETIC. The division and the exp are in
// DOUBLE, the result is narrowed to float, and the subtraction happens in
// FLOAT. Reordering those or widening the subtraction changes the last bits,
// which is exactly what a "no behaviour change" commit must not do.
inline constexpr double kMeterAttackTauSec  = 0.01;   // ~10 ms
inline constexpr double kMeterReleaseTauSec = 0.15;   // ~150 ms

/** The one-pole coefficient for a block of bufDurSec against a time constant of
    tauSec. Not shared with the 0.5 s RMS smoother in processBlock, which is
    computed entirely in double and would change value if routed through here. */
inline float ballisticCoeff (double bufDurSec, double tauSec)
{
    return 1.0f - (float) std::exp (-bufDurSec / tauSec);
}

// ===========================================================================
// THE WHOLE-RUN BAND MEAN (Phase 1b commit 3)
// ===========================================================================
//
// POWER, NOT dB, AND THIS IS THE WHOLE POINT. A mean of logarithms is a
// GEOMETRIC mean: it answers "what level is typical" and it is dominated by the
// quiet frames, because -120 dB drags an average far harder than it contributes
// energy. A band average is an ARITHMETIC mean of POWER, which is what "how much
// of this band is in this record" means. The two disagree by more the wider the
// dynamic range, which is to say they disagree most on exactly the material a
// reference library is made of.
//
// eqCurve averages dB, because it averages the already-logarithmic display
// spectrum. That is a different quantity from this one and it is not being
// changed here.
//
// THE FLOOR IS NOT A MEASUREMENT. A block whose band power is zero contributes
// zero power, which is correct and harmless: it lowers the mean by its share and
// nothing else. It does NOT contribute -120 dB, which is what a dB-domain mean
// would do and which is why that mean is the defect this function exists to
// avoid.
struct BandMean
{
    bool  valid = false;      // false = no blocks, so there is no mean to report
    int   blocks = 0;
    double meanPower = 0.0;   // per-octave-normalised power, linear
    float  db = -120.0f;      // the same quantity in dB, for the consumers
};

/** The mean of a sequence of per-block band powers, and its dB.

    sumPower is the running total the engine keeps; blocks is how many were
    added. Separated from the engine so a pin can exercise it with no audio,
    no window and no thread. */
inline BandMean bandMeanFromSum (double sumPower, int blocks)
{
    BandMean m;
    if (blocks <= 0) return m;            // no blocks: no mean, NOT zero
    m.valid     = true;
    m.blocks    = blocks;
    m.meanPower = sumPower / (double) blocks;
    m.db = m.meanPower > 1e-12 ? (float) (10.0 * std::log10 (m.meanPower)) : -120.0f;
    return m;
}

} // namespace echojay
