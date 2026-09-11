#pragma once

// ===========================================================================
// SPECTRAL EVIDENCE: WHAT A SPECTRUM IS, AND OVER WHAT WINDOW (11 Sep 2026)
//
// THE DEFECT THIS CLOSES (COMPARE_REFERENCE_PLAN section 1). Compare's tonal
// advice subtracted a whole-capture spectrum from the reference's last 150
// milliseconds. The reference side came from ReferenceResult::data, which is
// engine.getMeterData() taken after the final block of the file, and both
// spectrum and macroBandDb inside it are ballistic (10 ms attack, 150 ms
// release). So one side described a whole performance and the other described
// a fade out, and the difference was handed to the model as fact with an
// instruction to interpret rather than restate.
//
// The whole-file average was already being computed and thrown away:
// ReferenceAnalyser accumulates eqCurve across every block. It was read by
// three overloads that have no callers.
//
// WHY A STRUCT AND NOT JUST A SWAPPED ARGUMENT. Swapping data.spectrum for
// eqCurve fixes the reference side and leaves the capture side still choosing
// between a whole-window average and a whole-window peak hold by channel type,
// and leaves nobody able to tell which they got. A number whose provenance is
// unrecorded cannot be checked, and this comparison has now been wrong twice
// for exactly that reason. So each side carries WHAT it measured and OVER WHAT,
// the prose says so, and a mismatch is stated rather than averaged over.
//
// Header-only inline in the manner of EJCaptureGuard.h and EJDialWrites.h: the
// band helpers used to sit in an anonymous namespace in PluginProcessor.cpp,
// where neither the gate nor an offline measurement could reach them. They live
// here now so the shipped path, tools/mapfps_test and the before/after
// measurement all drive ONE implementation. A measurement that re-implements
// the thing it measures is a second opinion, not a verification.
// ===========================================================================

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace echojay
{

// ---------------------------------------------------------------------------
// THE UNSET SENTINEL, AND WHY IT IS NOT THE FLOOR.
//
// CaptureSnapshot::avgSpectrum and ::peakSpectrum used to default to {}, which
// for a dB-valued array is 0 dB in every bin: not a quiet spectrum but an
// impossibly loud flat one. It produces a confident wrong answer in two
// different shapes, and the first version of this comment described only the
// second, which the gate caught by the pin failing.
//
//   ONE SIDE zero-filled (the realistic case: an unguarded read of a restored
//   snapshot). The zero-filled side's bands are all 0 dB, so ITS max is 0 and
//   every (side - itsMax) term vanishes. The delta collapses to the OTHER
//   side's own tilt relative to its own loudest band, which for a normal mix is
//   tens of dB in the extremes. The diff then reports a large deficit in every
//   band but the loudest, invented entirely from an uninitialised array.
//
//   BOTH SIDES zero-filled. Now every band is exactly 0.0 and it reads as
//   perfect agreement.
//
// Neither is distinguishable from a real measurement. A real floor (-120 dB)
// would launder the same way, because avgDb clamps to -100 before averaging and
// a clamped flat array is still flat. The sentinel is therefore BELOW anything
// the clamp will touch, and avgDb detects it instead of clamping it, so an unset
// spectrum reads as an impossible measurement and the diff refuses.
//
// hasDualSpectrum remains the gate. This catches the day somebody forgets it.
// ---------------------------------------------------------------------------
inline constexpr float kSpectrumUnsetDb = -1000.0f;

/** True when a bin carries the unset sentinel rather than a measurement. */
inline bool binIsUnset (float db) noexcept { return db <= -999.0f; }

/** An array in the unset state: every bin the sentinel. Use as the default for
    any spectrum field that may be read before it is filled. */
inline std::array<float, 64> unsetSpectrum() noexcept
{
    std::array<float, 64> a;
    a.fill (kSpectrumUnsetDb);
    return a;
}

// ---------------------------------------------------------------------------
// WHAT A SPECTRUM IS
// ---------------------------------------------------------------------------

/** How a spectrum was reduced over its window. The distinction that matters
    for a comparison is average versus anything else: a peak hold keeps the
    loudest excursion in every bin, so against an average it reads as too much
    of everything transient, and a ballistic reading is a tail rather than a
    window at all. */
enum class SpectralReduction
{
    Unknown = 0,        ///< recorded by nothing; a stored reading with no provenance
    WholeFileAverage,   ///< mean of every analysis block of a file (eqCurve)
    WholeWindowAverage, ///< mean of every block of a capture (avgSpectrum)
    WholeWindowPeakHold,///< per-bin max over a capture (peakSpectrum)
    BallisticTail,      ///< the meter's own reading: ~10 ms attack, ~150 ms release
    LiveInstant,        ///< the same ballistic reading, taken live, of no fixed window

    /** The rolling 25 fps frame ring reduced by its MEAN (MeterEngine's
        reduceSpectrumWindow with useMean). It has its own name because it is
        NOT the same statistic as a capture's whole-window average, and calling
        it one would be the exact fault this vocabulary exists to prevent.

        Each ring frame is already the MAX of the display bins across its 40 ms,
        so a mean of frames is a mean of maxima and sits ABOVE a true per-block
        average on transient material. MeterEngine.h states this at the
        reduction itself. The peak case has no such gap: a max of per-frame
        maxima IS exactly the capture's peak hold, so that one is stamped
        WholeWindowPeakHold rather than given a name of its own. */
    RollingMeanOfMaxima
};

/** One side's spectral evidence: the bins, what they are, and what they cover.
    windowSeconds is 0 when the window is not bounded (a live or ballistic
    reading), which is itself part of the answer. */
struct SpectralEvidence
{
    std::array<float, 64> bins = unsetSpectrum();
    SpectralReduction     reduction = SpectralReduction::Unknown;
    float                 windowSeconds = 0.0f;
    bool                  valid = false;   ///< false = do not compare, say why

    /** True when this side's LOUDNESS figures (integrated, LRA) are accumulated
        continuously by the always-running meter rather than over the span the
        bins above describe. A live source is a MIXTURE: its spectrum is bounded
        (a ring window) or instantaneous (ballistic), while its LUFS has been
        integrating since the meters were last reset. Stamping the whole struct
        with the spectrum's answer would understate the loudness figures and
        overstate nothing, which is still a wrong label. */
    bool loudnessIsContinuous = false;
};

/** Only an average over a bounded window is a fair subject for a tonal delta.
    Everything else is comparable ONLY to itself, and the prose has to say so. */
inline bool reductionIsAverage (SpectralReduction r) noexcept
{
    return r == SpectralReduction::WholeFileAverage
        || r == SpectralReduction::WholeWindowAverage;
}

/** Prose name, for the context block the model reads. */
inline const char* reductionName (SpectralReduction r) noexcept
{
    switch (r)
    {
        case SpectralReduction::WholeFileAverage:    return "average across the whole file";
        case SpectralReduction::WholeWindowAverage:  return "average across the whole capture";
        case SpectralReduction::WholeWindowPeakHold: return "peak hold across the whole capture";
        case SpectralReduction::RollingMeanOfMaxima: return "mean of 40 ms maxima across the rolling window";
        case SpectralReduction::BallisticTail:       return "meter reading of roughly the last 150 ms";
        case SpectralReduction::LiveInstant:         return "live meter reading, no fixed window";
        case SpectralReduction::Unknown:             break;
    }
    return "reduction not recorded";
}

/** One line per side, so a reader can tell a whole-file average from a 150 ms
    tail without opening the source. This is section 1.5 item 4. */
inline juce::String spectralProvenanceLine (const juce::String& label,
                                            const SpectralEvidence& ev)
{
    juce::String s;
    s << "  " << label << ": " << reductionName (ev.reduction);
    if (ev.windowSeconds > 0.0f)
        s << ", window " << juce::String (ev.windowSeconds, 1) << " s";
    if (! ev.valid)
        s << " (NO SPECTRAL DATA)";
    s << "\n";
    // The one statistic that is close enough to an average to be mistaken for
    // one has to say that it is not, at the point it is named.
    if (ev.reduction == SpectralReduction::RollingMeanOfMaxima)
        s << "    (each frame is already a 40 ms maximum, so this sits above a true "
             "average on transients and is NOT bit-comparable with a capture)\n";
    return s;
}

/** The caveat sentence, empty when there is nothing to qualify.

    DECIDED ONCE (section 1.5 item 2): a mismatched pair still gets its tonal
    diff, with this sentence beside it, rather than the diff being suppressed.
    Suppressing removes the feature's only actionable output in order to avoid
    saying something imprecise, when qualifying it says more and lies less. The
    sentence carries the instruction not to quantify, because the direction
    survives a reduction mismatch and the magnitude does not. */
inline juce::String tonalDiffCaveat (const SpectralEvidence& a,
                                     const SpectralEvidence& b,
                                     const juce::String& labelA,
                                     const juce::String& labelB)
{
    if (! a.valid || ! b.valid)
        return "CAVEAT: one side has no spectral measurement, so no tonal "
               "comparison was made. Do not describe tonal balance at all.\n";

    if (reductionIsAverage (a.reduction) && reductionIsAverage (b.reduction))
        return {};

    const auto& oddLabel = reductionIsAverage (a.reduction) ? labelB : labelA;
    const auto& oddEv    = reductionIsAverage (a.reduction) ? b : a;

    juce::String s;
    s << "CAVEAT: these two spectra were not reduced the same way (" << oddLabel
      << " is a " << reductionName (oddEv.reduction)
      << "), so the DIRECTION of each band difference is meaningful and the size "
         "is not. Say which way the balance leans and do not quote or imply any "
         "dB amount.\n";
    return s;
}

/** Says that a side's loudness figures and its spectrum describe different
    spans, when that is true of either side. Empty otherwise.

    SAYING WHAT IS TRUE OF EACH, rather than stamping the struct with the
    pessimistic case. getMeterData() is a mixture: spectrum and macroBandDb are
    ballistic, while integrated and loudnessRange come from integrators that
    have been running since the meters were last reset. Labelling the whole
    reading "live instant" understated the loudness half. */
inline juce::String mixedSpanNote (const SpectralEvidence& a,
                                   const SpectralEvidence& b,
                                   const juce::String& labelA,
                                   const juce::String& labelB)
{
    const bool ca = a.loudnessIsContinuous, cb = b.loudnessIsContinuous;
    if (! ca && ! cb) return {};
    juce::String who = ca && cb ? (labelA + " and " + labelB)
                     : ca       ? labelA : labelB;
    juce::String s;
    s << "NOTE: on " << who << " the loudness figures (integrated, LRA) have been "
         "accumulating continuously since the meters were last reset, which is a "
         "different span from the spectrum described above. Do not read them as "
         "covering the same stretch of audio.\n";
    return s;
}

// ---------------------------------------------------------------------------
// THE BAND REDUCTION. Moved verbatim from the anonymous namespace in
// PluginProcessor.cpp (11 Sep 2026), with the sentinel detection added.
//
// Aggregate 64 log-spaced spectrum bins (20Hz-20kHz) into 6 musical bands.
// Bins are already in dB. We average in the linear (power) domain to avoid
// log-domain skew, then convert back to dB.
// Band boundaries (bin indices, inclusive):
//   Sub      20-60 Hz   bins  0-9
//   Low      60-200 Hz  bins 10-20
//   Low-mid  200-600 Hz bins 21-30
//   Mid      600Hz-2k    bins 31-41
//   High-mid 2k-6k Hz   bins 42-52
//   High     6k-20k Hz  bins 53-63
//
// THE TILT IS DELIBERATELY NOT CORRECTED HERE. spectrum[64] carries display
// shaping, and MeterEngine.h warns that integrating tilted bins breaks the
// pink-referenced property. For a DELTA the tilt largely cancels, because each
// side is normalised by its own loudest band before subtracting. A tilted delta
// is usable; a tilted absolute claim is not. Do not grow an absolute
// pink-referenced claim out of these numbers.
// ---------------------------------------------------------------------------

struct BandLevels { float sub, low, lowMid, mid, highMid, high; };

inline float avgDb (const std::array<float, 64>& s, int lo, int hi)
{
    double sumLin = 0.0;
    int n = 0, unset = 0;
    for (int i = lo; i <= hi; ++i) {
        double db = (double) s[(size_t) i];
        if (binIsUnset ((float) db)) { ++unset; continue; }   // never clamped into a level
        if (db < -100.0) db = -100.0; // clamp floor
        sumLin += std::pow (10.0, db / 10.0);
        ++n;
    }
    if (unset > 0 && n == 0) return kSpectrumUnsetDb;   // an unset band stays unset
    if (n == 0 || sumLin <= 1e-20) return -100.0f;
    return (float) (10.0 * std::log10 (sumLin / (double) n));
}

inline BandLevels computeBands (const std::array<float, 64>& s)
{
    return {
        avgDb (s,  0,  9),
        avgDb (s, 10, 20),
        avgDb (s, 21, 30),
        avgDb (s, 31, 41),
        avgDb (s, 42, 52),
        avgDb (s, 53, 63)
    };
}

/** The six band names, in band order, as the prose uses them. */
inline const char* bandName (int i) noexcept
{
    static const char* n[6] = { "sub (below 60Hz)", "lows (60-200Hz)",
                                "low-mids (200-600Hz)", "mids (600Hz-2kHz)",
                                "high-mids (2-6kHz)", "highs (above 6kHz)" };
    return (i >= 0 && i < 6) ? n[i] : "";
}

/** THE SIX DELTAS, EXTRACTED so appendTonalDiff, the gate and the offline
    measurement all read the same arithmetic. Positive = the mix side has more
    of that band than the reference side, after each side is normalised by its
    own loudest band.

    valid=false means the comparison must not be made: either side unset, or
    either side at the floor across the board. */
struct BandDeltaSet
{
    std::array<float, 6> delta = { 0, 0, 0, 0, 0, 0 };
    bool valid = false;
    bool mixHasSignal = false, refHasSignal = false;
    bool mixUnset = false, refUnset = false;
};

inline BandDeltaSet bandDeltas (const std::array<float, 64>& mixSpec,
                                const std::array<float, 64>& refSpec)
{
    const auto mb = computeBands (mixSpec);
    const auto rb = computeBands (refSpec);
    const float mix[6] { mb.sub, mb.low, mb.lowMid, mb.mid, mb.highMid, mb.high };
    const float ref[6] { rb.sub, rb.low, rb.lowMid, rb.mid, rb.highMid, rb.high };

    BandDeltaSet out;
    for (int i = 0; i < 6; ++i)
    {
        if (binIsUnset (mix[i])) out.mixUnset = true;
        if (binIsUnset (ref[i])) out.refUnset = true;
        if (mix[i] > -80.0f && ! binIsUnset (mix[i])) out.mixHasSignal = true;
        if (ref[i] > -80.0f && ! binIsUnset (ref[i])) out.refHasSignal = true;
    }
    if (out.mixUnset || out.refUnset) return out;               // valid stays false
    if (! out.mixHasSignal || ! out.refHasSignal) return out;   // valid stays false

    // Normalise both by their loudest band so overall-level differences
    // (already covered by LUFS) don't dominate the tonal diff.
    float mixMax = -200.0f, refMax = -200.0f;
    for (int i = 0; i < 6; ++i) { mixMax = juce::jmax (mixMax, mix[i]);
                                  refMax = juce::jmax (refMax, ref[i]); }
    for (int i = 0; i < 6; ++i)
        out.delta[(size_t) i] = (mix[i] - mixMax) - (ref[i] - refMax);
    out.valid = true;
    return out;
}

/** Append plain-language tonal diff lines. Only flags bands where the
    difference exceeds 2 dB; below that is noise. "Your mix has more/less X" is
    phrased from the user's perspective relative to the reference. */
inline void appendTonalDiff (juce::String& ctx,
                             const std::array<float, 64>& mixSpec,
                             const std::array<float, 64>& refSpec,
                             const juce::String& mixLabel,
                             const juce::String& refLabel)
{
    const auto d = bandDeltas (mixSpec, refSpec);

    if (d.mixUnset || d.refUnset)
    {
        // An impossible measurement, not an agreement. See kSpectrumUnsetDb.
        ctx += "TONAL BALANCE: no spectral measurement is available for one side, "
               "so frequency content was not compared.\n";
        return;
    }
    if (! d.valid)
    {
        ctx += "TONAL BALANCE: Not enough signal to compare frequency content.\n";
        return;
    }

    juce::String tonalLines;
    int flagged = 0;
    for (int i = 0; i < 6; ++i)
    {
        const float delta = d.delta[(size_t) i];
        if (std::abs (delta) >= 2.0f)
        {
            juce::String line = "- ";
            if (delta > 0)
                line += mixLabel + " has more " + bandName (i) + " than " + refLabel
                      + " (+" + juce::String (delta, 1) + " dB relative)";
            else
                line += mixLabel + " has less " + bandName (i) + " than " + refLabel
                      + " (" + juce::String (delta, 1) + " dB relative)";
            line += "\n";
            tonalLines += line;
            ++flagged;
        }
    }

    if (flagged == 0)
        ctx += "TONAL BALANCE: Very similar across the frequency range - no notable band differences.\n";
    else
    {
        ctx += "TONAL BALANCE DIFFERENCES (relative, already normalised for overall level):\n";
        ctx += tonalLines;
    }
}

} // namespace echojay
