#pragma once

// =============================================================================
//  THE COMPARE FIGURES: one derivation, and the one way a MatchSide is built.
// =============================================================================
//
// MOVED HERE 20 SEP 2026, NOT REWRITTEN. CompareFig, fillBandRel and both
// computeCompareFig overloads lived in an anonymous namespace in
// PluginProcessor.cpp, where nothing outside that translation unit could call
// them: not tools/mapfps_test, not an offline measurement, and not the editor.
// That is the same wall EJSpectralEvidence.h was created to get past on 11 Sep,
// and the note left at that move says why it matters: a figure the suite cannot
// link can only ever be pinned by a re-implementation of the thing being
// measured, which pins nothing.
//
// The bodies below are the bodies that were there, moved verbatim. The
// processor keeps its call sites unchanged through using-declarations, exactly
// as it did for BandLevels / avgDb / computeBands / appendTonalDiff.
//
// WHAT IS NOT MOVED: figBlock, buildCompareFiguresJson and buildCompareContext
// stay in PluginProcessor.cpp. They are the model's text and the card's JSON,
// they are pinned by text where they stand (se PIN12's N/A rendering among
// them), and they are not derivations.

#include <JuceHeader.h>
#include <array>

#include "MeterEngine.h"          // MeterData: the figures' input
#include "EJSpectralEvidence.h"   // SpectralEvidence: the macro bands and their stamp
#include "EJMatchProposal.h"      // MatchSide: what the Match screen reasons from

namespace echojay
{

/** ONE derivation of the compare figures, shared by the model's text table
    (figBlock) and the client-rendered figure card (buildCompareFiguresJson),
    so a visual can never disagree with the numbers the model reasons from.
    Sentinels are preserved: an unavailable reading stays at its sentinel
    (int/tp -100, lra 0, psr/plr/bandRel -999, overs -1) and renders/serialises
    as N/A, never a fabricated zero. */
struct CompareFig
{
    float integrated = -100.0f, lra = 0.0f, tp = -100.0f, psr = -999.0f,
          plr = -999.0f, crest = 0.0f, width = 0.0f, corr = 0.0f;
    int   overs = -1;
    std::array<float, 6> bandRel = { -999, -999, -999, -999, -999, -999 };
    bool  bandValid = false;
};

/** THE BAND RELATIVES, WITH THEIR DENOMINATOR FIXED.

    The mean is over the bands the scheme HAS, or the figure refuses. Six or
    nothing: counting only the bands above the floor produced a four-band mean
    under a six-band label, which is a real number against the wrong
    denominator carrying the same confidence as a good one.

    matchBandRelatives (EJMatchProposal.h) applies the same rule. That copy was
    made because this function could not be linked; now that it can be, closing
    it to one definition is a later commit and not this one, which moves code
    without changing what any of it computes. */
inline void fillBandRel (CompareFig& f, const std::array<float, 6>& db)
{
    for (int i = 0; i < 6; ++i)
        if (db[(size_t) i] <= -119.0f) return;   // six or nothing

    float sum = 0.0f;
    for (int i = 0; i < 6; ++i) sum += db[(size_t) i];
    const float mean = sum / 6.0f;
    f.bandValid = true;
    for (int i = 0; i < 6; ++i)
        f.bandRel[(size_t) i] = db[(size_t) i] - mean;
}

/** The figures from a side's MeterData alone: no bands, because MeterData's
    band field is the meter's state at whatever moment the source stopped. */
inline CompareFig computeCompareFig (const MeterData& m)
{
    CompareFig f;
    f.integrated = m.integrated;
    f.lra        = m.loudnessRange;
    f.tp = juce::jmax (m.truePeakMaxL, m.truePeakMaxR);
    if (f.tp <= -99.0f) f.tp = juce::jmax (m.truePeakL, m.truePeakR);
    f.psr = (m.psr > -99.0f) ? m.psr
          : (m.shortTermTruePeak > -99.0f && m.shortTerm > -99.0f)
                ? (m.shortTermTruePeak - m.shortTerm) : -999.0f;
    f.plr = (m.plr > -99.0f) ? m.plr
          : (f.tp > -99.0f && m.integrated > -99.0f) ? (f.tp - m.integrated) : -999.0f;
    f.crest = m.crestFactor;
    f.width = m.width;
    f.corr  = m.correlation;
    f.overs = m.oversCount;
    return f;
}

/** The same figures, with the band relatives taken from the side's own
    spectral evidence rather than from its MeterData. Every comparison path
    uses this one; the MeterData-only overload above exists for callers that
    have no evidence to offer and must then show no bands at all. */
inline CompareFig computeCompareFig (const MeterData& m, const SpectralEvidence& ev)
{
    CompareFig f = computeCompareFig (m);
    if (ev.hasMacro) fillBandRel (f, ev.macro);
    return f;
}

// -----------------------------------------------------------------------------
//  THE ONE WAY A MatchSide IS BUILT
// -----------------------------------------------------------------------------
//
// THE LIVE RULES TRAVEL WITH THE SIDE, NOT WITH THE CALLER. A caller that has
// to remember them is a caller that can forget them, and the Match screen, the
// suite and anything later all need the same answers:
//
//   LRA        A Live side's loudness range is suppressed, not caveated: a
//              session LRA is spread across whatever was played, which across
//              several songs is inter-song variance wearing the LRA label. The
//              compare context suppresses it at its own feed point, under
//              se PIN12, which holds those two lines by text; this is the same
//              fact stated for the MatchSide, and the two are deliberately not
//              merged, because merging them would redden a pin whose purpose is
//              to hold the literal lines it names. Said here so the duplication
//              is a recorded decision rather than a drift.
//   duration   0, meaning not known. A Live slot has no length, and
//              MatchSide's refusals read 0 as "no duration to judge".
//
// THE OVERS SENTINEL IS THE TRAP THIS FUNCTION EXISTS TO CLOSE. MeterData's
// oversCount defaults to 0, and MatchSide reads overs < 0 as unavailable, so a
// zero that was never measured would pass as a measured "no clipping" and the
// ceiling move would be silently withheld. Two sources produced such a zero:
//   - a RESTORED snapshot, whose meters object never carried the count. The
//     save now writes it and the restore defaults to -1 when it is absent
//     (PluginProcessor.cpp), so an older session reads as unavailable, which is
//     what it is;
//   - a REFERENCE, whose analyser measures no overs at all. That one is not
//     closed here: the analyser would have to measure them, which is a
//     measurement change and not a move. A reference therefore still reports 0
//     overs, and any consumer that treats a reference's overs as evidence is
//     reading a default. The ceiling move is about the MIX side, so the
//     proposal does not read it today.
inline MatchSide matchSideFrom (const CompareFig& f, const SpectralEvidence& ev,
                                float durationSeconds, bool isLive) noexcept
{
    MatchSide s;
    s.macro          = ev.macro;
    s.hasMacro       = ev.hasMacro;
    s.macroReduction = ev.macroReduction;

    s.durationSeconds = isLive ? 0.0f : juce::jmax (0.0f, durationSeconds);

    s.integrated  = f.integrated;
    s.truePeak    = f.tp;
    s.overs       = f.overs;
    s.lra         = isLive ? 0.0f : f.lra;
    s.psr         = f.psr;
    s.plr         = f.plr;
    s.crest       = f.crest;
    s.width       = f.width;
    s.correlation = f.corr;
    return s;
}

} // namespace echojay
