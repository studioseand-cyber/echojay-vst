#pragma once

// =============================================================================
//  THE MATCH PAGE: the shape you set up and press, and the picture it plays.
// =============================================================================
//
// MATCH_SCREEN_CONTRACT section 3 governs the picture and section 10 the scope:
// NO SERVER EDIT, so nothing here sends. What this page is, restructured 20 Sep
// 2026 after Kathy saw the first one:
//
//   YOUR CAPTURE ---- [ AI MATCH ] ---- THE REFERENCE
//   and under them, the picture, which is the whole point of the screen.
//
// THE ANALYSIS DOES NOT LIVE HERE. The moves list, the directional findings and
// the refusal list are gone from the screen; they belong in the chat, where
// every other piece of AI output in this product already goes, and they arrive
// there after the press (the next commit). What stays is ONE line above the
// button: whether a match is possible, and if not, the single most important
// reason. Without it the user presses a button that does nothing for reasons
// nobody gave them, which is this project's oldest defect in a new place.
//
// PURE, LIKE EJCodecPage.h. Rects, numbers and strings from a proposal. It
// reads no processor, no editor and no slot, so tools/mapfps_test pins the same
// arithmetic and the same sentence the screen draws.
//
// TWO LAYERS, FROM DIFFERENT DATA, AND THEY MUST NOT BE CONFLATED:
//   the curves  each side's 64 log bins (SpectralEvidence::bins). REAL
//               MEASURED SPECTRA, on ONE shared dB range.
//   the blocks  the six macro bands (kMacroBandLoHz), drawn as STEPS, because
//               a step is what the proposal asserts. A smooth line through six
//               numbers would invent shape the arithmetic never had.

#include <JuceHeader.h>
#include <array>

#include "EJBandScheme.h"        // kMacroBandLoHz, macroBandName
#include "EJMatchProposal.h"     // the proposal this page draws
#include "EJSpectralEvidence.h"  // SpectralEvidence, reductionName

namespace echojay
{

// -----------------------------------------------------------------------------
//  THE PAGE'S ROWS
// -----------------------------------------------------------------------------
//
// THE PICTURE TAKES WHAT IS LEFT, WHICH IS MOST OF IT. Three fixed rows at the
// top (the line, the setup row, a gap) and the plot below them, so the graphic
// grows with the window instead of being squeezed by text that is no longer
// here.
inline constexpr int kMatchPagePadTop    = 8;
inline constexpr int kMatchPagePadBottom = 10;
inline constexpr int kMatchPagePadSide   = 14;
inline constexpr int kMatchStatusH       = 16;   // the one line, above the button
inline constexpr int kMatchSetupH        = 34;   // names, link, button
inline constexpr int kMatchGap           = 10;
inline constexpr int kMatchButtonW       = 132;

/** THE CONNECTOR IS THE STRONGEST THING IN THE ROW (20 Sep 2026, Kathy). It
    used to be whatever was left after the names took the row, which was a
    10 px stub at every width: two ticks either side of the button rather than
    the button reaching out to both names.

    So the NAME is capped and the LINK takes the rest. At a wide window that
    gives the link several hundred pixels and the row reads as one connected
    thing; at the narrowest it still keeps kMatchLinkMinW, because a connector
    that vanishes when the window is small is a connector that says the two
    sides are unrelated exactly when the user most needs to see that they are
    not. Below that there is genuinely nothing to give and the link takes what
    remains rather than pushing the names to nothing. */
inline constexpr int kMatchLinkMinW = 28;
inline constexpr int kMatchNameMaxW = 220;

/** The plot's gutters inside the graph card. */
inline constexpr int kMatchPlotLabelW = 30;
inline constexpr int kMatchPlotAxisH  = 14;

/** The dB range the curves and the blocks share: the spectrum panel's own
    66 dB, so the picture reads like the spectrum the user already knows. A
    caller-supplied range is the whole point, since three things drawn in one
    rect that each chose their own would disagree about what a decibel is. */
inline constexpr float kMatchPlotSpanDb = 66.0f;

struct MatchPageRects
{
    juce::Rectangle<int> status;     ///< the one line, above the button
    juce::Rectangle<int> setup;      ///< the whole name-link-button row
    juce::Rectangle<int> mixName, refName, button;
    juce::Rectangle<int> linkLeft, linkRight;   ///< the two runs of the link
    juce::Rectangle<int> graph;
};

inline MatchPageRects matchPageLayout (juce::Rectangle<int> page)
{
    MatchPageRects r;
    auto a = page.reduced (kMatchPagePadSide, 0)
                 .withTrimmedTop (kMatchPagePadTop)
                 .withTrimmedBottom (kMatchPagePadBottom);

    r.status = a.removeFromTop (kMatchStatusH);
    r.setup  = a.removeFromTop (kMatchSetupH);
    a.removeFromTop (kMatchGap);
    r.graph  = a;

    // THE BUTTON IS THE MIDDLE OF THE ROW, the two names its ends, and the
    // link runs between each name and the button. The names take what the
    // button leaves, evenly, down to a floor: a long reference name shortens
    // rather than pushing the button off centre.
    auto row = r.setup;
    r.button = row.withSizeKeepingCentre (juce::jmin (kMatchButtonW, row.getWidth()),
                                          row.getHeight());
    const int side  = juce::jmax (0, (row.getWidth() - r.button.getWidth()) / 2);
    // The name is capped; the LINK takes everything else, and keeps its floor
    // until there is nothing left to keep it from.
    const int nameW = juce::jmax (0, juce::jmin (kMatchNameMaxW, side - kMatchLinkMinW));
    r.mixName  = { row.getX(), row.getY(), nameW, row.getHeight() };
    r.refName  = { row.getRight() - nameW, row.getY(), nameW, row.getHeight() };
    r.linkLeft  = { r.mixName.getRight(), row.getY(),
                    juce::jmax (0, r.button.getX() - r.mixName.getRight()), row.getHeight() };
    r.linkRight = { r.button.getRight(), row.getY(),
                    juce::jmax (0, r.refName.getX() - r.button.getRight()), row.getHeight() };
    return r;
}

/** The plot inside the graph card, gutters removed. */
inline juce::Rectangle<int> matchGraphPlot (juce::Rectangle<int> graph)
{
    auto p = graph.reduced (8, 8);
    p.removeFromLeft (kMatchPlotLabelW);
    p.removeFromBottom (kMatchPlotAxisH);
    return p;
}

// -----------------------------------------------------------------------------
//  THE PLOT'S TWO AXES
// -----------------------------------------------------------------------------
//
// THE SAME LOG AXIS paintSpectrumCurve uses, 20 Hz at the left edge and 20 kHz
// at the right, so a band boundary drawn here lands where that curve's 250 Hz
// is. If either mapping changes, they both change or the picture lies.
inline constexpr double kMatchPlotLoHz = 20.0;
inline constexpr double kMatchPlotHiHz = 20000.0;

inline float matchFreqToX (juce::Rectangle<int> plot, double hz) noexcept
{
    const double lo = std::log2 (kMatchPlotLoHz), hi = std::log2 (kMatchPlotHiHz);
    const double f  = juce::jlimit (kMatchPlotLoHz, kMatchPlotHiHz, hz);
    const double t  = (std::log2 (f) - lo) / (hi - lo);
    return (float) plot.getX() + (float) (t * (double) plot.getWidth());
}

inline float matchDbToY (juce::Rectangle<int> plot, float db, float dbMin, float dbMax) noexcept
{
    if (dbMax <= dbMin) return (float) plot.getBottom();
    const float t = (juce::jlimit (dbMin, dbMax, db) - dbMin) / (dbMax - dbMin);
    return (float) plot.getBottom() - t * (float) plot.getHeight();
}

/** A macro band's span on the axis. The sixth band ends at the plot's top
    frequency, which is what kMacroBandLoHz cannot carry (its ceiling moves
    with the sample rate; here the picture stops at 20 kHz like the axis). */
inline juce::Range<float> matchBandSpanX (juce::Rectangle<int> plot, int band) noexcept
{
    const int b  = juce::jlimit (0, 5, band);
    const double lo = kMacroBandLoHz[(std::size_t) b];
    const double hi = (b == 5) ? kMatchPlotHiHz : kMacroBandLoHz[(std::size_t) b + 1];
    return { matchFreqToX (plot, lo), matchFreqToX (plot, hi) };
}

/** The macro band a frequency falls in, 0 to 5. Below the first edge is the
    sub band and above the last is the air band: the picture stops at 20 kHz,
    the bands do not. */
inline int matchBandForHz (double hz) noexcept
{
    for (int b = 5; b >= 0; --b)
        if (hz >= kMacroBandLoHz[(std::size_t) b]) return b;
    return 0;
}

// -----------------------------------------------------------------------------
//  THE SIX DELTAS, AND THE SIX MOVES
// -----------------------------------------------------------------------------

/** Each band's delta, reference minus mix, on the six-band relatives. THE SAME
    EXPRESSION computeMatchProposal uses, written here because the proposal
    keeps a delta only on the bands that produced a move and the picture needs
    all six: a band below the floor still has a gap, and that gap is what the
    shading is. mr PIN17 pins the two equal band by band. */
inline bool matchBandDeltas (const MatchSide& mix, const MatchSide& ref,
                             std::array<float, 6>& out) noexcept
{
    std::array<float, 6> mixRel {}, refRel {};
    if (! mix.hasMacro || ! ref.hasMacro) return false;
    if (! matchBandRelatives (mix.macro, mixRel)) return false;
    if (! matchBandRelatives (ref.macro, refRel)) return false;
    for (int i = 0; i < 6; ++i)
        out[(std::size_t) i] = refRel[(std::size_t) i] - mixRel[(std::size_t) i];
    return true;
}

/** The proposal's band moves as six numbers, 0 where no move was proposed.
    Taken FROM THE PROPOSAL, not recomputed, so the morph moves exactly what
    the proposal offered and never a band it declined. */
inline std::array<float, 6> matchBandMoves (const MatchProposal& p) noexcept
{
    std::array<float, 6> m { 0, 0, 0, 0, 0, 0 };
    for (const auto& mv : p.moves)
        if (mv.kind == MatchMoveKind::Band && mv.band >= 0 && mv.band < 6)
            m[(std::size_t) mv.band] = mv.valueDb;
    return m;
}

// -----------------------------------------------------------------------------
//  THE MORPH
// -----------------------------------------------------------------------------
//
// THE MORPH APPLIES THE SIX STEPS TO THE MEASURED CURVE, and that result is
// honest: it is the user's own spectrum with the proposal's gains on it, step
// edges and all. A bin takes its OWN band's move and nothing is blended across
// a boundary, because blending would smooth the step back into the curve the
// contract forbids.
//
// t is the position, 0 to 1. NOT a smoother: the caller owns it, so the morph
// can be played, replayed and stopped.
inline float matchMorphedDb (float measuredDb, double hz,
                             const std::array<float, 6>& moveDb, float t) noexcept
{
    const float m = moveDb[(std::size_t) matchBandForHz (hz)];
    return measuredDb + juce::jlimit (0.0f, 1.0f, t) * m;
}

/** Ease for the morph: slow at both ends, so the curve reads as settling into
    the proposal rather than snapping to it. */
inline float matchMorphEase (float t) noexcept
{
    const float x = juce::jlimit (0.0f, 1.0f, t);
    return x * x * (3.0f - 2.0f * x);
}

// -----------------------------------------------------------------------------
//  ONE BAND'S BLOCK
// -----------------------------------------------------------------------------
//
// THREE LEVELS, IN THE CURVE'S OWN dB:
//   anchor     where the mix curve sits across this band
//   proposed   anchor + the move, or anchor when there is none
//   matched    anchor + the delta: where this band would sit if it matched
//
// The block runs anchor to proposed: that is the move. The shading runs
// proposed to matched: THE GAP THAT REMAINS, which is the output and not a
// shortfall. A capped move keeps its 3 dB block and gains a marker at matched,
// so a cap LOOKS capped rather than being explained away somewhere else.
//
// THE REFERENCE CURVE NEVER MOVES (plan 8A.2): nothing here writes to it.
struct MatchBandBlock
{
    int   band     = -1;
    bool  hasMove  = false;
    bool  capped   = false;
    bool  hasGap   = false;
    float anchorDb = 0.0f, proposedDb = 0.0f, matchedDb = 0.0f;
    float moveDb   = 0.0f, deltaDb    = 0.0f;
};

inline MatchBandBlock matchBandBlock (int band, float anchorDb, float deltaDb) noexcept
{
    MatchBandBlock b;
    b.band     = juce::jlimit (0, 5, band);
    b.anchorDb = anchorDb;
    b.deltaDb  = deltaDb;

    const auto d = matchBandMove (deltaDb);
    b.hasMove    = d.propose;
    b.capped     = d.capped;
    b.moveDb     = d.propose ? d.valueDb : 0.0f;
    b.proposedDb = anchorDb + b.moveDb;
    b.matchedDb  = anchorDb + deltaDb;
    b.hasGap     = std::abs (b.matchedDb - b.proposedDb) > 0.05f;
    return b;
}

// -----------------------------------------------------------------------------
//  THE ONE LINE
// -----------------------------------------------------------------------------
//
// WHETHER A MATCH IS POSSIBLE, AND IF NOT, THE SINGLE MOST IMPORTANT REASON.
// One line, not a list: the list is the chat's job after the press.
//
// THE ORDER OF IMPORTANCE, and why (reordered 20 Sep 2026: the moves used to
// come first and hid every reason behind them):
//   1  a length that cannot be judged, which refuses the bands AND the
//      dynamics, so it is the widest reason and the first to say;
//   2  bands that are not an average, which refuses the spectrum proposal
//      whatever the length;
//   3  no six-band measurement at all;
//   4  the moves, which say how many there are and how many are spectrum,
//      because that is what the press is about to play. They sit BELOW the
//      three reasons above, each of which removes an axis and therefore
//      explains why the count is as small as it is, and ABOVE the one below,
//      which removes nothing;
//   5  the gain floor, which is GOOD NEWS and must not read as a complaint:
//      two sides inside the floor already agree, and saying so as a failure
//      would be the feature reporting its own success as a fault.
//
// TWO QUESTIONS, TWO FIELDS (20 Sep 2026). `possible` used to answer both of
// these at once, and they came apart the moment a refusal was allowed to
// outrank the moves in the line:
//
//   playable  IS THERE ANYTHING FOR THE PRESS TO PLAY. Purely "the proposal
//             carries a move", independent of which branch won the line. The
//             press and the button's live styling ask this one.
//   possible  IS THE HEADLINE THE OPTIMISTIC ONE. True only in the moves
//             branch below. The status line's colour asks this one.
//
// They disagree exactly where the reordering intends them to: a capture whose
// length is unknown still has its ceiling move to play, and the line still
// leads with the length. One field could not say both without the press
// refusing a move it holds.
struct MatchReadiness
{
    bool         possible = false;   ///< the headline is the optimistic one
    bool         playable = false;   ///< the proposal carries a move to play
    bool         goodNews = false;   ///< nothing to do because the sides agree
    juce::String line;
};

inline MatchReadiness matchReadiness (const MatchProposal& p)
{
    MatchReadiness r;
    int bandMoves = 0;
    for (const auto& m : p.moves)
        if (m.kind == MatchMoveKind::Band) ++bandMoves;

    // SET BEFORE ANY BRANCH, so which reason wins the line cannot change
    // whether the press has something to play. This is the expression
    // `possible` carried before the two meanings were split, which is why the
    // press and the button behave exactly as they did.
    r.playable = ! p.moves.empty();

    auto firstOf = [&p] (MatchRefusalKind k) -> const MatchRefusal*
    {
        for (const auto& rf : p.refusals) if (rf.kind == k) return &rf;
        return nullptr;
    };

    // A REFUSAL THAT REMOVES AN AXIS OUTRANKS THE MOVES THAT SURVIVED IT
    // (20 Sep 2026). A capture whose length nobody knows still has its ceiling
    // move, and "A match is possible: 1 move" as the only thing on screen would
    // be the page answering a question the user did not ask while withholding
    // the one that explains the rest. The moves are still played, still
    // counted, and still go to the chat; what changes is which sentence leads.
    if (const auto* rf = firstOf (MatchRefusalKind::BandsTooShort))     { r.line = rf->message; return r; }
    if (const auto* rf = firstOf (MatchRefusalKind::DynamicsTooShort))  { r.line = rf->message; return r; }
    if (const auto* rf = firstOf (MatchRefusalKind::BandsNotAverage))   { r.line = rf->message; return r; }
    if (p.bandsUnavailableWhy.isNotEmpty())
    {
        r.line = "No match: " + p.bandsUnavailableWhy + ".";
        return r;
    }

    // THE MOVES OUTRANK THE GAIN FLOOR, which is the one refusal that is not a
    // missing axis: levels already matching is not a reason to hide spectrum
    // work that is ready to play.
    if (! p.moves.empty())
    {
        r.possible = true;
        r.line = "A match is possible: " + juce::String ((int) p.moves.size())
               + (p.moves.size() == 1 ? " move" : " moves")
               + (bandMoves > 0 ? ", " + juce::String (bandMoves) + " on the spectrum." : ".");
        return r;
    }

    if (const auto* rf = firstOf (MatchRefusalKind::GainBelowFloor))
    {
        r.goodNews = true;
        r.line     = rf->message;
        return r;
    }
    r.line = "Nothing to propose: the two sides carry no difference this can act on.";
    return r;
}

/** What the press says when there is nothing to play. A refused proposal
    cannot animate, and the press must SAY so rather than playing nothing. */
inline juce::String matchPressRefusedText (const MatchReadiness& r)
{
    return r.goodNews ? "Nothing to play: these two already match."
                      : "Nothing to play, and the line above says why.";
}

/** THE PICTURE'S OWN SENTENCE when there is no proposed curve to draw. ""
    when there are blocks. */
inline juce::String matchNoCurveText (const MatchProposal& p, bool haveDeltas)
{
    if (haveDeltas && p.bandsUnavailableWhy.isEmpty())
        return {};
    if (p.bandsUnavailableWhy.isNotEmpty())
        return "No proposed curve: " + p.bandsUnavailableWhy + ".";
    return "No proposed curve: there are no six-band relatives to compare.";
}

/** THE PROVENANCE, ON THE PICTURE (contract section 3, last paragraph). A
    whole-file average against a ballistic tail is not a like-for-like curve,
    and the prose saying so elsewhere is not the picture saying it. "" when
    both sides were reduced the same way. */
inline juce::String matchProvenanceText (const SpectralEvidence& mix, const SpectralEvidence& ref)
{
    if (! mix.valid || ! ref.valid) return "one side has no spectrum to draw";
    if (mix.reduction == ref.reduction) return {};
    return juce::String ("yours: ") + reductionName (mix.reduction)
         + "   reference: " + reductionName (ref.reduction)
         + "   (different measurements, compare with that in mind)";
}

} // namespace echojay
