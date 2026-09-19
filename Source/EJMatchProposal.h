#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <vector>
#include "EJSpectralEvidence.h"   // SpectralReduction, reductionIsAverage, reductionName
#include "EJBandScheme.h"         // macroBandName, the six macro bands

// =============================================================================
//  MATCH REFERENCE, PHASE 2a: THE PROPOSAL, AS ARITHMETIC ONLY.
// =============================================================================
//
// MATCH_REFERENCE_PLAN sections 2.1, 3, 4 and 5. This computes what Match
// Reference would propose against one reference and applies nothing. The Match
// sub-tab exists (19 Sep 2026) and draws matchPageStatement, at the end of this
// file; computeMatchProposal is still called only from tools/mapfps_test.
//
// WHY A HEADER AND NOT PluginProcessor.cpp. The compare figures live in an
// anonymous namespace there (CompareFig, computeCompareFig, fillBandRel), where
// the gate cannot link them. bandDeltas was moved out of that namespace for the
// same reason. A proposal the suite cannot link would be pinned by a copy of
// itself.
//
// PLAIN VALUES IN, PLAIN VALUES OUT. It reads no processor, no editor and no
// slot. The caller hands over each side's six macro bands and their stamp, the
// figures the plan tiers, and the durations. The figures use CompareFig's
// sentinels, so a caller can pass CompareFig values straight through.
//
// THE SIX-OR-NOTHING RULE IS A SECOND COPY. fillBandRel
// (PluginProcessor.cpp:3421) refuses band relatives unless all six bands are
// above the floor, and matchBandRelatives below applies the same rule. It is a
// copy because fillBandRel is in that anonymous namespace. Moving it here so
// there is one definition is a later commit, not this one.
//
// THE REFUSALS, AND THE TWO THAT ARE ABSENT. Plan section 5 lists five. Three
// can be decided from what a side carries, and they are here:
//
//   capture too short        durationSeconds against kMatchBandMinSeconds and
//                            kMatchDynamicsMinSeconds
//   peak hold on either side reductionIsAverage on macroReduction, the same
//                            predicate se PIN1 pins
//   clipping upstream        true peak above 0 dBTP or overs above 0; this is
//                            not a refusal, it puts the ceiling move first
//
// And one the plan's section 5 does not list, added with M2's answer:
//
//   gain below its floor     the two integrated figures closer than
//                            kMatchGainFloorDb. A refusal naming the floor and
//                            BOTH figures, never a silent absence. A capture
//                            under kMatchDynamicsMinSeconds never reaches it:
//                            the duration refusal already says why there is no
//                            loudness move, and two refusals for one absent
//                            move would teach the user nothing.
//
// Two are absent because nothing could decide them:
//
//   substitution   CaptureSnapshot::outputSubstitution is "" on every capture
//                  that exists, because startCapture already refuses to start
//                  while anything replaces the output (PluginProcessor.cpp,
//                  CAPTURE EXCLUSION stage 2). A check against a field that is
//                  always empty would pass every capture and prove nothing.
//   moved reference  the plan says a moved file keeps its stored eqCurve and
//                  figures. Nothing persists them: state saves referencePaths
//                  only, and a restore re-analyses and silently skips a missing
//                  file. EJReferenceIndex.h has the schema but no plugin
//                  translation unit includes it. Open list item 162.
//
// WHAT reductionIsAverage REFUSES TODAY. It is true only for WholeFileAverage
// and WholeWindowAverage. So besides a peak hold, it also refuses a Live side
// (RollingWindowPowerMean) and a saved review (BallisticTail). The refusal
// names the reduction it saw rather than calling everything a peak hold.
// =============================================================================

namespace echojay
{

// -----------------------------------------------------------------------------
// THE CONSTANTS. This header's own, named here.
// -----------------------------------------------------------------------------
//
// NOT THE THRESHOLD AT EJSpectralEvidence.h:509. That one is a bare 2.0f in
// appendTonalDiff. It works on the 64 display bins, grouped by a DIFFERENT
// six-band split (20-60, 60-200, 200-600, 600-2k, 2k-6k, 6k-20k), and each
// side is normalised to its LOUDEST band. These two work on the six macro bands
// (EJBandScheme.h: 20-60, 60-250, 250-500, 500-2k, 2k-6k, 6k-top), each side
// normalised to its six-band MEAN. Two schemes, two thresholds. Plan section 3
// calls this "the existing flag threshold of 2 dB", and that conflates them.
// The other one is unchanged by this header.
//
// All five numbers are chosen, not measured. Four are plan open question M1;
// the fifth, kMatchGainFloorDb, is M2's answer and adds itself to that list.
// Section 7 has the experiment that settles the two durations. Nothing yet
// settles the two floors or the cap.

/** A band move is proposed when the magnitude of its delta is >= this.
    ">=", not "exceeds": exactly 2.0 dB proposes a move, 1.999 does not. That
    is the existing convention; the comment at EJSpectralEvidence.h:481 says
    "exceeds" while its code tests >=, and this comment does not repeat that. */
inline constexpr float kMatchBandFloorDb = 2.0f;

/** No single band move is larger than this in one run, whatever the gap. */
inline constexpr float kMatchBandCapDb = 3.0f;

/** Below this many seconds, on either side, no band move is proposed. */
inline constexpr float kMatchBandMinSeconds = 30.0f;

/** Below this many seconds, on either side, no loudness move and no dynamics
    finding (LRA, PSR, PLR, crest) is offered. */
inline constexpr float kMatchDynamicsMinSeconds = 60.0f;

/** The gain offset is proposed when the two integrated figures are this far
    apart or more. ">=" exactly as kMatchBandFloorDb: a gap of exactly 1.0 dB
    proposes a move, 0.999 is refused.

    ITS OWN FLOOR, NOT THE BANDS' 2 dB (plan M2, answered 19 Sep 2026). A level
    difference is audible well below 2 dB, and every other comparison in the
    proposal is unreliable while the two sides sit at different loudnesses, so
    getting them level is closer to the first move than to an optional one.

    1.0 IS CHOSEN, NOT MEASURED. Nothing stands behind it, the same as the
    other four thresholds in plan M1. Do not read it as a measured JND.

    NO CAP, AND NONE IS DECIDED. The band cap exists because a large band delta
    is more likely to be a measurement artefact than a real difference. That
    argument does not obviously carry to level: each integrated figure is one
    gated mean over the whole run, not a relative level read off a narrow band
    of bins. Whether a large level gap is ever an artefact has not been measured
    either, so it is left open rather than capped by analogy or declared safe.
    There is no kMatchGainCapDb, deliberately, and not 3 dB copied from the
    bands. */
inline constexpr float kMatchGainFloorDb = 1.0f;

// -----------------------------------------------------------------------------
// INPUT
// -----------------------------------------------------------------------------

/** One side of the comparison. The figures use CompareFig's sentinels. */
struct MatchSide
{
    std::array<float, 6> macro { -120, -120, -120, -120, -120, -120 };
    bool                 hasMacro = false;
    SpectralReduction    macroReduction = SpectralReduction::Unknown;

    /** 0 = not known (a Live slot has no length). */
    float durationSeconds = 0.0f;

    float integrated  = -100.0f;   ///< LUFS, <= -99 unavailable
    float truePeak    = -100.0f;   ///< dBTP, <= -99 unavailable
    int   overs       = -1;        ///< < 0 unavailable
    float lra         = 0.0f;      ///< LU, <= 0 unavailable
    float psr         = -999.0f;   ///< dB, <= -99 unavailable
    float plr         = -999.0f;   ///< dB, <= -99 unavailable
    float crest       = 0.0f;      ///< dB, always measured (CompareFig)
    float width       = 0.0f;      ///< percent, always measured
    float correlation = 0.0f;      ///< -1..+1, always measured
};

// -----------------------------------------------------------------------------
// OUTPUT
// -----------------------------------------------------------------------------

enum class MatchTier { Exact, Bounded, Directional };

enum class MatchMoveKind
{
    Ceiling,   ///< exact: a true-peak ceiling, offered only when the mix clips
    Gain,      ///< exact: the integrated-loudness offset
    Band       ///< bounded: one macro band, floored and capped
};

/** One proposed move. Only Exact and Bounded tiers produce these.

    valueDb and measuredDb, by kind:
      Band     valueDb is the move, measuredDb the band delta it came from
      Gain     both are the loudness difference, reference minus mix
      Ceiling  valueDb is the ceiling in dBTP, measuredDb the mix's true peak */
struct MatchMove
{
    MatchMoveKind kind = MatchMoveKind::Band;
    MatchTier     tier = MatchTier::Bounded;
    int           band = -1;          ///< 0..5 for Band, -1 otherwise
    float         valueDb = 0.0f;
    float         measuredDb = 0.0f;
    bool          capped = false;     ///< Band only: the gap was larger than the cap
};

enum class MatchFigure { LRA = 0, PSR, PLR, Crest, Width, Correlation, Count };

inline const char* matchFigureName (MatchFigure f) noexcept
{
    switch (f)
    {
        case MatchFigure::LRA:         return "LRA";
        case MatchFigure::PSR:         return "PSR";
        case MatchFigure::PLR:         return "PLR";
        case MatchFigure::Crest:       return "crest";
        case MatchFigure::Width:       return "width";
        case MatchFigure::Correlation: return "correlation";
        case MatchFigure::Count:       break;
    }
    return "";
}

/** A directional finding: a real measurement that does not specify a move.

    IT HAS NO FIELD FOR A MOVE, AND MUST NEVER GROW ONE. The difference is
    evidence to quote. A finding that carried a value would invite the UI to
    apply it, and plan decision 2 says a directional finding gets no apply
    affordance at all. */
struct MatchFinding
{
    MatchFigure figure = MatchFigure::LRA;
    float       mixValue = 0.0f;
    float       refValue = 0.0f;
    float       difference = 0.0f;    ///< mix minus reference
};

enum class MatchRefusalKind
{
    BandsTooShort,      ///< a side is shorter than kMatchBandMinSeconds
    DynamicsTooShort,   ///< a side is shorter than kMatchDynamicsMinSeconds
    BandsNotAverage,    ///< a side's macro bands are not an average reduction
    GainBelowFloor      ///< the two integrated figures are closer than kMatchGainFloorDb
};

/** A refusal names its threshold and the value that failed it (decision 7). */
struct MatchRefusal
{
    MatchRefusalKind  kind = MatchRefusalKind::BandsTooShort;
    /** "the capture" or "the reference"; for GainBelowFloor, which is about
        both, "the capture and the reference". */
    juce::String      side;
    float             thresholdSeconds = 0.0f; ///< the TooShort kinds
    float             measuredSeconds  = 0.0f; ///< the TooShort kinds, 0 = unknown
    SpectralReduction measuredReduction = SpectralReduction::Unknown; ///< BandsNotAverage

    // GainBelowFloor only. TWO-SIDED, where the TooShort kinds are one-sided:
    // the refusal is about the gap between two figures, so it carries both
    // figures and the floor, not one "measured" value against a limit. The
    // generalisation into value plus unit is open list 189, triggered by a
    // fifth field group.
    float             gainFloorDb   = 0.0f;     ///< GainBelowFloor: kMatchGainFloorDb
    float             captureLufs   = -100.0f;  ///< GainBelowFloor: the capture's integrated
    float             referenceLufs = -100.0f;  ///< GainBelowFloor: the reference's integrated

    juce::String      message;
};

struct MatchProposal
{
    /** In the order they are offered: Ceiling, then Gain, then the bands in
        band order. The ceiling therefore comes ahead of anything spectral
        whenever there is one. */
    std::vector<MatchMove>    moves;
    std::vector<MatchFinding> findings;
    std::vector<MatchRefusal> refusals;

    /** The sum of the band moves, so a proposal that tilts the whole mix reads
        as a tilt rather than as six separate small decisions. Always
        matchBandMoveSum (moves). */
    float bandMoveSumDb = 0.0f;

    /** True peak above 0 dBTP or overs above 0 on the mix side. */
    bool clipping = false;

    /** Band crest exists on three bands, not the six these moves are on, so
        nothing decided whether a band's excess is density (EQ) or peaks
        (dynamics). Plan decision 4: said, never silently guessed. True
        whenever a band move is proposed. */
    bool bandsWithoutDiscriminator = false;

    /** Why no band moves were considered, when a side has no six-band
        measurement. Not a refusal: there is nothing to refuse. "" otherwise. */
    juce::String bandsUnavailableWhy;

    /** The sentences that go with the moves: one per capped band, the
        discriminator sentence, the clipping sentence. */
    juce::StringArray notes;
};

// -----------------------------------------------------------------------------
// THE ARITHMETIC
// -----------------------------------------------------------------------------

/** One band's decision from its delta. delta is what the mix band would need to
    reach the reference: positive means add. At or above the floor the move is
    the delta, capped at kMatchBandCapDb with its sign kept; below the floor
    there is no move. A NaN delta proposes nothing. */
struct MatchBandDecision
{
    bool  propose = false;
    float valueDb = 0.0f;
    bool  capped = false;
};

inline MatchBandDecision matchBandMove (float deltaDb) noexcept
{
    const float mag = std::abs (deltaDb);
    if (! (mag >= kMatchBandFloorDb))
        return {};
    const bool  capped = mag > kMatchBandCapDb;
    const float v      = capped ? kMatchBandCapDb : mag;
    return { true, deltaDb < 0.0f ? -v : v, capped };
}

/** The sum of the Band moves in a list, in list order. The ONE expression the
    proposal's bandMoveSumDb is taken from. */
inline float matchBandMoveSum (const std::vector<MatchMove>& moves) noexcept
{
    float s = 0.0f;
    for (const auto& m : moves)
        if (m.kind == MatchMoveKind::Band)
            s += m.valueDb;
    return s;
}

/** Each band relative to the side's own six-band mean, or false when any band
    is on the floor (six or nothing; see the copy note at the top). */
inline bool matchBandRelatives (const std::array<float, 6>& db, std::array<float, 6>& rel) noexcept
{
    for (int i = 0; i < 6; ++i)
        if (db[(size_t) i] <= -119.0f) return false;
    float sum = 0.0f;
    for (int i = 0; i < 6; ++i) sum += db[(size_t) i];
    const float mean = sum / 6.0f;
    for (int i = 0; i < 6; ++i) rel[(size_t) i] = db[(size_t) i] - mean;
    return true;
}

/** Whether a gap in integrated loudness, reference minus capture, proposes a
    gain move: its magnitude >= kMatchGainFloorDb. A NaN gap proposes nothing. */
inline bool matchGainProposes (float deltaDb) noexcept
{
    return std::abs (deltaDb) >= kMatchGainFloorDb;
}

/** The gain refusal's message: the floor and BOTH integrated figures, and the
    gap between them.

    THE GAP IS TRUNCATED, NOT ROUNDED, to two decimals, for the reason
    matchSecondsText truncates: a gap of 0.999 dB must never print as 1.00
    beside a 1.0 dB floor it failed. The two figures are printed to two
    decimals as well; the gap is stated outright so that two figures which
    happen to round 1.00 apart cannot be read as meeting the floor. */
inline juce::String matchGainFloorMessage (float captureLufs, float referenceLufs)
{
    const float gap = std::floor (std::abs (referenceLufs - captureLufs) * 100.0f) / 100.0f;
    return "No gain move: the capture measures " + juce::String (captureLufs, 2)
         + " LUFS integrated and the reference " + juce::String (referenceLufs, 2)
         + " LUFS, " + juce::String (gap, 2) + " dB apart, and a gain move needs at least "
         + juce::String (kMatchGainFloorDb, 1) + " dB.";
}

/** Seconds as the refusal prints them: one decimal, TRUNCATED, so 29.96 s
    reads 29.9 and never 30.0 beside a 30 s threshold it failed. */
inline juce::String matchSecondsText (float s)
{
    return juce::String (std::floor (s * 10.0f) / 10.0f, 1);
}

/** A too-short refusal's message. It names the threshold and the measured
    length, or says the length is not known. */
inline juce::String matchTooShortMessage (const juce::String& what, const juce::String& side,
                                          float thresholdSeconds, float measuredSeconds)
{
    const juce::String need = "a " + what + " needs at least "
                            + juce::String ((int) thresholdSeconds) + " s";
    if (measuredSeconds <= 0.0f)
        return "No " + what + ": the length of " + side + " is not known, and " + need + ".";
    return "No " + what + ": " + side + " is " + matchSecondsText (measuredSeconds)
         + " s long, and " + need + ".";
}

/** The proposal. mix is the user's side, ref the reference. */
inline MatchProposal computeMatchProposal (const MatchSide& mix, const MatchSide& ref)
{
    MatchProposal p;

    const MatchSide*    sides[2] = { &mix, &ref };
    const juce::String  names[2] = { "the capture", "the reference" };

    // ---- The refusals that gate the rest ----------------------------------
    bool bandsRefused = false, dynamicsRefused = false;
    for (int s = 0; s < 2; ++s)
    {
        const float d = sides[s]->durationSeconds;
        if (d < kMatchBandMinSeconds)
        {
            MatchRefusal r;
            r.kind = MatchRefusalKind::BandsTooShort;
            r.side = names[s];
            r.thresholdSeconds = kMatchBandMinSeconds;
            r.measuredSeconds  = juce::jmax (0.0f, d);
            r.message = matchTooShortMessage ("band proposal", names[s], kMatchBandMinSeconds, d);
            p.refusals.push_back (r);
            bandsRefused = true;
        }
        if (d < kMatchDynamicsMinSeconds)
        {
            MatchRefusal r;
            r.kind = MatchRefusalKind::DynamicsTooShort;
            r.side = names[s];
            r.thresholdSeconds = kMatchDynamicsMinSeconds;
            r.measuredSeconds  = juce::jmax (0.0f, d);
            r.message = matchTooShortMessage ("loudness or dynamics proposal", names[s],
                                              kMatchDynamicsMinSeconds, d);
            p.refusals.push_back (r);
            dynamicsRefused = true;
        }
        if (sides[s]->hasMacro && ! reductionIsAverage (sides[s]->macroReduction))
        {
            MatchRefusal r;
            r.kind = MatchRefusalKind::BandsNotAverage;
            r.side = names[s];
            r.measuredReduction = sides[s]->macroReduction;
            r.message = "No band proposal: the bands of " + names[s] + " are a "
                      + juce::String (reductionName (sides[s]->macroReduction))
                      + ", and a proposal needs an average across the whole file or capture.";
            p.refusals.push_back (r);
            bandsRefused = true;
        }
    }

    // ---- Exact: the ceiling, first whenever the mix clips -----------------
    const bool tpKnown = mix.truePeak > -99.0f;
    p.clipping = (tpKnown && mix.truePeak > 0.0f) || mix.overs > 0;
    if (p.clipping)
    {
        // The reference's own ceiling where it has one at or below 0 dBTP,
        // otherwise 0 dBTP itself.
        const float ceiling = (ref.truePeak > -99.0f) ? juce::jmin (0.0f, ref.truePeak) : 0.0f;
        MatchMove m;
        m.kind = MatchMoveKind::Ceiling;
        m.tier = MatchTier::Exact;
        m.valueDb = ceiling;
        m.measuredDb = mix.truePeak;
        p.moves.push_back (m);

        juce::String s = "The capture";
        if (tpKnown) s += " peaks at " + juce::String (mix.truePeak, 1) + " dBTP";
        if (mix.overs > 0) s += (tpKnown ? " with " : " has ") + juce::String (mix.overs) + " inter-sample overs";
        s += ", so something upstream is already clipping. The ceiling comes first, ahead of anything spectral.";
        p.notes.add (s);
    }

    // ---- Exact: the gain offset --------------------------------------------
    // A duration refusal comes first and this is never reached: a capture too
    // short for a loudness claim gets that one refusal, not a second one here.
    if (! dynamicsRefused && mix.integrated > -99.0f && ref.integrated > -99.0f)
    {
        const float delta = ref.integrated - mix.integrated;
        if (matchGainProposes (delta))
        {
            MatchMove m;
            m.kind = MatchMoveKind::Gain;
            m.tier = MatchTier::Exact;
            m.valueDb = delta;      // no cap: see kMatchGainFloorDb
            m.measuredDb = delta;
            p.moves.push_back (m);
        }
        else
        {
            MatchRefusal r;
            r.kind          = MatchRefusalKind::GainBelowFloor;
            r.side          = "the capture and the reference";
            r.gainFloorDb   = kMatchGainFloorDb;
            r.captureLufs   = mix.integrated;
            r.referenceLufs = ref.integrated;
            r.message       = matchGainFloorMessage (mix.integrated, ref.integrated);
            p.refusals.push_back (r);
        }
    }

    // ---- Bounded: the six band moves ---------------------------------------
    std::array<float, 6> mixRel {}, refRel {};
    if (! mix.hasMacro || ! ref.hasMacro)
        p.bandsUnavailableWhy = ! mix.hasMacro ? "the capture has no six-band measurement"
                                               : "the reference has no six-band measurement";
    else if (! matchBandRelatives (mix.macro, mixRel))
        p.bandsUnavailableWhy = "a band of the capture is on the floor, so there is no six-band mean";
    else if (! matchBandRelatives (ref.macro, refRel))
        p.bandsUnavailableWhy = "a band of the reference is on the floor, so there is no six-band mean";
    else if (! bandsRefused)
    {
        for (int i = 0; i < 6; ++i)
        {
            const float delta = refRel[(size_t) i] - mixRel[(size_t) i];
            const auto  d = matchBandMove (delta);
            if (! d.propose) continue;
            MatchMove m;
            m.kind = MatchMoveKind::Band;
            m.tier = MatchTier::Bounded;
            m.band = i;
            m.valueDb = d.valueDb;
            m.measuredDb = delta;
            m.capped = d.capped;
            p.moves.push_back (m);
            if (d.capped)
                p.notes.add ("The " + juce::String (macroBandName (i)) + " gap is "
                             + juce::String (std::abs (delta), 1) + " dB, larger than one move closes."
                             " This proposes " + juce::String (kMatchBandCapDb, 1)
                             + " dB; running it again is a choice to go further.");
        }
    }
    p.bandMoveSumDb = matchBandMoveSum (p.moves);
    for (const auto& m : p.moves)
        if (m.kind == MatchMoveKind::Band) { p.bandsWithoutDiscriminator = true; break; }
    if (p.bandsWithoutDiscriminator)
        p.notes.add ("These EQ moves are proposed without band crest on the same six bands, "
                     "so nothing has decided whether a band's excess is density or peaks.");

    // ---- Directional: evidence, never a move -------------------------------
    auto find = [&p] (MatchFigure f, float a, float b)
    {
        MatchFinding x;
        x.figure = f;
        x.mixValue = a;
        x.refValue = b;
        x.difference = a - b;
        p.findings.push_back (x);
    };
    if (! dynamicsRefused)
    {
        if (mix.lra > 0.0f     && ref.lra > 0.0f)     find (MatchFigure::LRA, mix.lra, ref.lra);
        if (mix.psr > -99.0f   && ref.psr > -99.0f)   find (MatchFigure::PSR, mix.psr, ref.psr);
        if (mix.plr > -99.0f   && ref.plr > -99.0f)   find (MatchFigure::PLR, mix.plr, ref.plr);
        find (MatchFigure::Crest, mix.crest, ref.crest);
    }
    find (MatchFigure::Width,       mix.width,       ref.width);
    find (MatchFigure::Correlation, mix.correlation, ref.correlation);

    return p;
}

// -----------------------------------------------------------------------------
// THE MATCH PAGE'S STATEMENT, before the proposal is drawn there.
// -----------------------------------------------------------------------------
//
// What a user who opens the Match sub-tab reads: what the page is for, and the
// four things it will show, from MATCH_REFERENCE_PLAN section 6 (the tiered
// moves, the directional findings with no apply affordance, the refusals with
// their numbers) and section 5 (a refusal names its number), and that nothing
// has been written. Not "coming soon", and not a spinner: the last line says
// plainly that this version does not compute a proposal.
//
// THE NUMBERS COME FROM THE CONSTANTS ABOVE, so the page cannot promise a floor
// or a cap the arithmetic does not use. Pinned by mr PIN12.
struct MatchPageStatement
{
    juce::String      title;
    juce::String      lead;
    juce::StringArray items;    // exactly four, in plan section 6's order
    juce::String      status;
};

inline MatchPageStatement matchPageStatement()
{
    auto dB = [] (float v) { return juce::String (v, 1) + " dB"; };
    MatchPageStatement s;
    s.title = "MATCH REFERENCE";
    s.lead  = "Match proposes moves that would bring your capture closer to the selected "
              "reference. Compare describes the difference; Match says what to do about it. "
              "This page will show:";
    s.items.add ("Proposed moves, grouped by tier. Exact: a gain offset of at least "
                 + dB (kMatchGainFloorDb) + ", and a true-peak ceiling when the capture clips. "
                 "Bounded: EQ moves on six bands, each at least " + dB (kMatchBandFloorDb)
                 + " and at most " + dB (kMatchBandCapDb)
                 + ". Every move shows the measurement it came from.");
    s.items.add ("Directional findings, kept apart from the moves: LRA, PSR, PLR, crest, width "
                 "and correlation, each with its number and no apply button. A finding is "
                 "evidence, not a move.");
    s.items.add ("Refusals, wherever a proposal cannot be trusted, each naming the threshold and "
                 "the measured value that failed it.");
    s.items.add ("That nothing has been written. A proposal changes nothing in your chain by itself.");
    s.status = "This version does not compute a proposal yet.";
    return s;
}

} // namespace echojay
