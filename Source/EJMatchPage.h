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

/** The waveform strip under each picker: one per side, full width of its
    picker. 26 px is enough to read a shape and not enough to pretend this is
    the picture, which is still the thing below. */
inline constexpr int kMatchWaveH   = 26;
inline constexpr int kMatchWaveGap = 4;

// -----------------------------------------------------------------------------
//  WHICH SLOT IS THE REFERENCE, DECIDED ONCE
// -----------------------------------------------------------------------------
//
// ROLE IS DECIDED BY CONTENT AND THEN HELD. On entry to the page the slot
// holding a Reference is the reference side and the other is the mix side;
// with neither or both, the reference is the bottom slot and the mix the top.
//
// THEN IT IS STORED AND NOT RECOMPUTED. Deriving it per paint, which is what
// refBarIsTop() did, means a pick can change which side is which underneath
// the user: put a reference in the top slot and the two names, the two
// waveforms and the direction of every move swap over with no gesture that
// asked for it.
//
// THE REASON THIS IS STABLE, and it is what the pins hold: the mix picker
// offers Live, snapshots and captures and CAN NEVER PLACE A REFERENCE, and the
// reference picker writes into the slot that is already the reference side. So
// no pick can move a role from one slot to the other, and the flip cannot
// happen at all rather than being corrected after it does.
enum class MatchRefSide { Top, Bottom };

inline MatchRefSide matchRefSideOnEntry (bool topIsReference, bool botIsReference)
{
    // Top only is the one case that puts the reference at the top. Bottom only,
    // neither and both all land on the bottom: with neither there is nothing to
    // honour, and with both the bottom is the one the reference bar already
    // drives, so the page agrees with the bar instead of contradicting it.
    return (topIsReference && ! botIsReference) ? MatchRefSide::Top : MatchRefSide::Bottom;
}

/** What a side's waveform IS, in words, because the two sides are not the same
    span of time and the drawing must not imply they are.

    A live side is a WINDOW onto something still running; a capture or a
    reference is the WHOLE of a thing. Saying "live, last 3.4 s" beside
    "whole file, 2:48" is the honest version of two strips drawn the same
    width. */
inline juce::String matchWaveSpan (bool rolling, float seconds)
{
    if (seconds <= 0.0f)
        return rolling ? juce::String ("live, waiting for signal")
                       : juce::String ("length unknown");
    if (rolling)
        return "live, last " + juce::String (seconds, 1) + " s";

    const int total = (int) (seconds + 0.5f);
    const int mins  = total / 60, secs = total % 60;
    return "whole file, " + juce::String (mins) + ":"
         + (secs < 10 ? "0" : "") + juce::String (secs);
}

// -----------------------------------------------------------------------------
//  THE AXIS ROW: FOUR TILES, ONE SELECTED
// -----------------------------------------------------------------------------
//
// ONE GRAMMAR, FOUR VOCABULARIES. Every picture shows the same three things,
// your mix, the reference and THE GAP BETWEEN THEM, with the gap the brightest
// thing on the page. What changes per axis is the terms it is drawn in, because
// a spectrum, a level, a dynamic range and a stereo image are not the same kind
// of quantity and drawing them the same way would say they were.
//
// SELECTING A TILE CHANGES THE PICTURE AND NOTHING ELSE. It sends nothing,
// applies nothing and writes nothing, which is why these controls belong in
// step one although section 10 excluded the ASK: the rule was that a control
// which cannot send is dead, and these do something without sending.
inline constexpr int kMatchAxisH   = 30;
inline constexpr int kMatchAxisGap = 6;
inline constexpr int kMatchAxisCount = 4;

enum class MatchAxis { Spectrum = 0, Loudness, Dynamics, Stereo };

inline const char* matchAxisName (MatchAxis a)
{
    switch (a)
    {
        case MatchAxis::Spectrum: return "SPECTRUM";
        case MatchAxis::Loudness: return "LOUDNESS";
        case MatchAxis::Dynamics: return "DYNAMICS";
        case MatchAxis::Stereo:   return "STEREO IMAGE";
    }
    return "SPECTRUM";
}

/** TWO OF THE FOUR CAN BE PROPOSED AND TWO CANNOT, and that is a fact about the
    proposal rather than a choice about the screen: computeMatchProposal emits
    Gain, Ceiling and Band moves and nothing else, so dynamics and stereo image
    have no move to play whatever the two sides say. */
inline bool matchAxisCanPropose (MatchAxis a)
{
    return a == MatchAxis::Spectrum || a == MatchAxis::Loudness;
}

/** What the press says on an axis that carries no moves.

    NOT A DISABLED BUTTON. A control that looks dead with no reason given is
    exactly what the headline rework removed, so the press answers instead, in
    the same words and through the same refused-for-a-moment machinery the gain
    floor already uses. */
inline juce::String matchAxisPressText (MatchAxis a)
{
    if (matchAxisCanPropose (a)) return {};
    return juce::String (a == MatchAxis::Dynamics ? "Dynamics" : "Stereo image")
         + " is shown, not proposed: a match moves level and the spectrum, "
           "and this difference is here to be read.";
}

/** Whether a tile can draw at all, and if not, WHICH SIDE is missing what.

    A TILE WITH A FIELD MISSING ON ONE SIDE SAYS SO RATHER THAN DRAWING A
    DEFAULT. Every sentinel in MatchSide is a value a picture would happily
    render: -100 LUFS is a position on a rail, 0 LU is a width, -1 overs is a
    number. Drawn without this gate they are all measurements of something that
    was never measured. */
struct MatchAxisState
{
    bool         drawable = true;
    juce::String why;          ///< empty when drawable
};

inline MatchAxisState matchAxisState (MatchAxis a, const MatchSide& mix, const MatchSide& ref)
{
    auto no = [] (const juce::String& s) { return MatchAxisState { false, s }; };

    switch (a)
    {
        case MatchAxis::Spectrum:
            if (! mix.hasMacro && ! ref.hasMacro) return no ("Neither side has a six-band measurement.");
            if (! mix.hasMacro)                   return no ("Your mix has no six-band measurement.");
            if (! ref.hasMacro)                   return no ("The reference has no six-band measurement.");
            return {};

        case MatchAxis::Loudness:
            if (mix.integrated <= -99.0f && ref.integrated <= -99.0f)
                return no ("Neither side has an integrated loudness.");
            if (mix.integrated <= -99.0f) return no ("Your mix has no integrated loudness.");
            if (ref.integrated <= -99.0f) return no ("The reference has no integrated loudness.");
            return {};

        case MatchAxis::Dynamics:
            // THE CREST IS THE TILE; THE LOUDNESS RANGE IS ONE OF ITS TWO
            // DIMENSIONS. This used to refuse whenever either side's lra was
            // 0, which for a Live side it always is, so a live mix could never
            // see its own dynamics. That was refusing a whole picture because
            // half of one shape was missing. Crest IS measured live and does
            // mean something live; lra is not, and matchSideFrom suppresses it
            // deliberately, because a session loudness range is spread across
            // whatever was played and is inter-song variance wearing the label.
            //
            // So: refuse only when a CREST is missing, and let the picture say
            // what the missing range is rather than drawing around it.
            if (mix.crest <= 0.0f && ref.crest <= 0.0f)
                return no ("Neither side has a crest this can compare.");
            if (mix.crest <= 0.0f) return no ("Your mix has no crest measurement.");
            if (ref.crest <= 0.0f) return no ("The reference has no crest measurement.");
            return {};

        case MatchAxis::Stereo:
            // width and correlation are measured on every side, live included.
            return {};
    }
    return {};
}

// -----------------------------------------------------------------------------
//  THE WAVE STRIP'S TWO PARTS: A TRANSPORT BUTTON AND THE WAVE ITSELF
// -----------------------------------------------------------------------------
//
// ONE ANSWER, READ BY THE PAINT AND BY THE PRESS. A click on the wave is a
// SEEK and a click on the button is play or stop, so the two regions have to
// agree to the pixel; computing them twice is how they come to disagree.
//
// THE BUTTON IS ON THE OUTER EDGE of each strip, so the two sides' controls sit
// at the two edges of the page rather than facing each other across the middle.
inline constexpr int kMatchWaveBtnW = 16;

inline juce::Rectangle<int> matchWaveTransport (juce::Rectangle<int> lane, bool isRef)
{
    if (lane.getWidth() <= kMatchWaveBtnW * 2) return {};
    return isRef ? lane.removeFromRight (kMatchWaveBtnW)
                 : lane.removeFromLeft  (kMatchWaveBtnW);
}

/** What is left for the waveform once the button has taken its edge. THE SEEK
    FRACTION IS OF THIS RECT, and therefore of the FILE, not of the panel: the
    two strips are different widths and a position in samples cannot come from
    one of them. */
inline juce::Rectangle<int> matchWaveLane (juce::Rectangle<int> lane, bool isRef)
{
    auto l = lane;
    if (l.getWidth() > kMatchWaveBtnW * 2)
    {
        if (isRef) l.removeFromRight (kMatchWaveBtnW);
        else       l.removeFromLeft  (kMatchWaveBtnW);
    }
    return l;
}

/** Where in the file a click at x lands, 0 to 1. */
inline float matchWaveSeekFraction (juce::Rectangle<int> waveLane, int x)
{
    if (waveLane.getWidth() <= 0) return 0.0f;
    return juce::jlimit (0.0f, 1.0f,
                         (float) (x - waveLane.getX()) / (float) waveLane.getWidth());
}

/** What the dynamics tile must SAY about one side, beside its shape.

    TWO THINGS A SHAPE CANNOT CARRY ON ITS OWN. A side with no loudness range
    would otherwise get a narrow shape, and narrow means LOW VARIANCE, which is
    a measurement nobody took. And a LIVE side's crest is a rolling reading of
    whatever is playing now, not a figure over a whole track, so a window and a
    whole track would sit side by side with nothing saying which was which.

    The same spirit as the spectrum picture's provenance caption and the
    waveform strip's span: where two figures are not like for like, the picture
    says so rather than the prose elsewhere. */
inline juce::String matchDynamicsSideNote (const MatchSide& s)
{
    // A live side has no length: matchSideFrom writes 0 for a Live slot, which
    // is the same fact the waveform strip reads to call itself rolling.
    const bool rolling = s.durationSeconds <= 0.0f;
    const bool noRange = s.lra <= 0.0f;

    if (rolling && noRange) return "rolling window, no loudness range";
    if (rolling)            return "rolling window";
    if (noRange)            return "no loudness range";
    return {};
}

/** The width a shape takes when its side has no loudness range: a fixed width,
    so the shape keeps its crest height and says nothing it does not know. */
inline constexpr float kMatchDynNoRangeW = 0.18f;   ///< fraction of the lane

// -----------------------------------------------------------------------------
//  THE TARGET IS A ZONE WITH A WIDTH, NOT A LINE
// -----------------------------------------------------------------------------
//
// The idea the whole picture turns on, taken from the reference Kathy sent:
// YOU SEE WHETHER YOU ARE INSIDE THE TARGET rather than subtracting two curves
// in your head. A line says "be exactly here", which is a thing no proposal
// asks for; a band says "anywhere in here is fine", which is what the
// arithmetic actually means.
//
// AND THE WIDTH IS NOT A DESIGN CHOICE. IT IS THE PROPOSAL'S OWN FLOOR, the
// threshold below which no move is emitted: kMatchBandFloorDb for the spectrum
// and kMatchGainFloorDb for the level. INSIDE THE BAND MEANS NOTHING WOULD BE
// MOVED. That is why these functions read the constants rather than carrying
// numbers of their own: a later edit that tuned the picture's band without
// tuning the floor would draw a tolerance the proposal does not have, and the
// user would be told they were fine while a move was waiting for them.
//
// TWO AXES HAVE A ZONE AND TWO DO NOT, and the reason is the same one:
// A ZONE CLAIMS A TOLERANCE, and we only have a tolerance where the proposal
// has a floor. computeMatchProposal emits Gain, Ceiling and Band moves, so
// dynamics and stereo image get markers and no band. Drawing one there would
// invent a threshold nobody set and imply a move that cannot be made.
inline float matchAxisZoneDb (MatchAxis a) noexcept
{
    switch (a)
    {
        case MatchAxis::Spectrum: return kMatchBandFloorDb;   // the band move's floor
        case MatchAxis::Loudness: return kMatchGainFloorDb;   // the gain move's floor
        case MatchAxis::Dynamics:
        case MatchAxis::Stereo:   return 0.0f;                // no move, so no tolerance
    }
    return 0.0f;
}

inline bool matchAxisHasZone (MatchAxis a) noexcept { return matchAxisZoneDb (a) > 0.0f; }

/** Is this band's difference inside the zone, meaning nothing would be moved?

    WRITTEN AS THE PROPOSAL WRITES IT, negated: matchBandMove emits nothing when
    `! (mag >= kMatchBandFloorDb)`, so inside is exactly that test and the
    picture cannot disagree with the moves drawn over it. */
inline bool matchInsideBandZone (float mixRelDb, float refRelDb) noexcept
{
    const float mag = std::abs (refRelDb - mixRelDb);
    return ! (mag >= kMatchBandFloorDb);
}

/** THE PICTURE'S SAMPLING: how many columns a ribbon is built from. Here
    rather than in the editor because the panel's own trail method takes a row,
    so the type has to be visible to both. */
inline constexpr int kMatchRibbonCols = 96;
using MatchRibbonRow = std::array<float, (std::size_t) kMatchRibbonCols>;

/** The tiles, evenly across the row with a gap between them. Integer division
    leaves the remainder on the last tile rather than a gap that drifts. */
inline juce::Rectangle<int> matchAxisTile (juce::Rectangle<int> row, int i)
{
    if (i < 0 || i >= kMatchAxisCount || row.getWidth() <= 0) return {};
    const int gaps  = kMatchAxisGap * (kMatchAxisCount - 1);
    const int tileW = juce::jmax (0, (row.getWidth() - gaps) / kMatchAxisCount);
    const int x     = row.getX() + i * (tileW + kMatchAxisGap);
    const int w     = (i == kMatchAxisCount - 1) ? juce::jmax (0, row.getRight() - x) : tileW;
    return { x, row.getY(), w, row.getHeight() };
}


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
    juce::Rectangle<int> mixPick, refPick, button;   ///< the two pickers and the press
    juce::Rectangle<int> mixWave, refWave;           ///< a waveform under each picker
    juce::Rectangle<int> linkLeft, linkRight;        ///< the two runs of the link
    juce::Rectangle<int> axisRow;                    ///< the four tiles
    juce::Rectangle<int> graph;
    /** THE READOUT ROW, along the bottom of the graph card. APPENDED, for the
        reason open list 207 records: the suite links across this struct. */
    juce::Rectangle<int> readout;
};

// -----------------------------------------------------------------------------
//  THE READOUT ROW: THE NUMBERS, IN THE METER STRIP'S LANGUAGE
// -----------------------------------------------------------------------------
//
// SAME FAMILY AS THE MAIN METER STRIP at the top of the plugin: one row of
// cells, thin vertical dividers between them, label above in small muted
// uppercase, value below. Not rounded boxes.
//
// EACH CELL CARRIES BOTH SIDES. With the filament gone the two trail colours
// are the only key to which fan is which, so the mix value is drawn in the mix
// trail's colour and the reference value in the reference trail's, with a thin
// divider between them. THIS ROW IS CARRYING THE KEY.
inline constexpr int kMatchReadoutH     = 34;   // label line + value line
inline constexpr int kMatchReadoutLabelH = 11;
inline constexpr int kMatchReadoutMaxW  = 190;  // a cell never grows past this

/** One cell: a short label and the two sides' already-formatted values.
    NOTHING IS RECOMPUTED HERE. The caller passes what it already drew from,
    because two sources for one number is how they drift apart. */
struct MatchReadoutCell
{
    juce::String label;      ///< short name only, no unit
    juce::String mixText;    ///< already formatted, unit included
    juce::String refText;
};

/** A figure formatted to this page's precision rules, or a DASH when the side
    does not have it.

    A MISSING FIGURE AND A ZERO ARE NOT THE SAME THING and the row must not
    make them look alike, so an unavailable value is "-" rather than "0.0". */
inline juce::String matchReadoutValue (bool have, float v, int decimals,
                                       const juce::String& unit)
{
    if (! have) return "-";

    // ZERO DECIMALS IS NOT ZERO DECIMALS IN JUCE, and this guard is here
    // because the old stereo line shipped reading "width 46.3896 / 32.5188 %"
    // from a call whose argument said 0. juce_String.cpp's writeDouble only
    // sets std::fixed and a precision WHEN numDecPlaces > 0:
    //
    //     if (numDecPlaces > 0) { o.setf (std::ios_base::fixed);
    //                             o.precision ((std::streamsize) numDecPlaces); }
    //     o << n;
    //
    // so 0 or less skips the formatting entirely and the value goes out at the
    // stream's DEFAULT SIX SIGNIFICANT FIGURES. A whole number therefore needs
    // the INTEGER constructor, not a rounded float one.
    const juce::String num = decimals > 0 ? juce::String (v, decimals)
                                          : juce::String (juce::roundToInt (v));
    return num + (unit.isEmpty() ? juce::String() : " " + unit);
}

/** The same for a count, which takes no decimals. */
inline juce::String matchReadoutCount (int n)
{
    return n < 0 ? juce::String ("-") : juce::String (n);
}

/** One cell's rect. Cells divide the row evenly, capped at
    kMatchReadoutMaxW so a two-cell axis does not stretch two figures across a
    1700 px window; the capped row stays centred. */
inline juce::Rectangle<int> matchReadoutCellRect (juce::Rectangle<int> row, int i, int count)
{
    if (count <= 0 || i < 0 || i >= count || row.getWidth() <= 0) return {};
    const int even  = row.getWidth() / count;
    const int cellW = juce::jmin (kMatchReadoutMaxW, even);
    const int total = cellW * count;
    const int x0    = row.getX() + (row.getWidth() - total) / 2;
    return { x0 + i * cellW, row.getY(), cellW, row.getHeight() };
}


inline MatchPageRects matchPageLayout (juce::Rectangle<int> page)
{
    MatchPageRects r;
    auto a = page.reduced (kMatchPagePadSide, 0)
                 .withTrimmedTop (kMatchPagePadTop)
                 .withTrimmedBottom (kMatchPagePadBottom);

    r.status = a.removeFromTop (kMatchStatusH);
    r.setup  = a.removeFromTop (kMatchSetupH);
    // The waveform strip rides UNDER the row rather than inside it: the pickers
    // keep the height they had, so the row still reads as one line of controls
    // and the shapes sit below the thing they belong to.
    auto waveRow = a.removeFromTop (kMatchWaveH);
    a.removeFromTop (kMatchWaveGap);
    // THE AXIS ROW SITS BETWEEN THE SETUP AND THE PICTURE, because it names
    // what the picture below is about: put it under the graph and it would be a
    // legend for something already drawn.
    r.axisRow = a.removeFromTop (kMatchAxisH);
    a.removeFromTop (kMatchAxisGap);
    a.removeFromTop (kMatchGap);
    r.graph  = a;
    // The row sits inside the card, below the plot and above the card's own
    // bottom gutter, so both are derived from graph and cannot disagree.
    r.readout = r.graph.reduced (8, 8).withTrimmedLeft (kMatchPlotLabelW)
                        .removeFromBottom (kMatchPlotAxisH + kMatchReadoutH)
                        .withTrimmedBottom (kMatchPlotAxisH);

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
    r.mixPick  = { row.getX(), row.getY(), nameW, row.getHeight() };
    r.refPick  = { row.getRight() - nameW, row.getY(), nameW, row.getHeight() };
    r.linkLeft  = { r.mixPick.getRight(), row.getY(),
                    juce::jmax (0, r.button.getX() - r.mixPick.getRight()), row.getHeight() };
    r.linkRight = { r.button.getRight(), row.getY(),
                    juce::jmax (0, r.refPick.getX() - r.button.getRight()), row.getHeight() };

    // Each waveform sits under its own picker and is exactly as wide, so a
    // strip can never be read as belonging to the other side.
    r.mixWave = { r.mixPick.getX(), waveRow.getY(), r.mixPick.getWidth(), waveRow.getHeight() };
    r.refWave = { r.refPick.getX(), waveRow.getY(), r.refPick.getWidth(), waveRow.getHeight() };
    return r;
}

/** The plot inside the graph card, gutters removed. */
inline juce::Rectangle<int> matchGraphPlot (juce::Rectangle<int> graph)
{
    auto p = graph.reduced (8, 8);
    p.removeFromLeft (kMatchPlotLabelW);
    p.removeFromBottom (kMatchPlotAxisH);
    // THE ROW COMES OUT OF THE PLOT, NOT OUT OF THE CARD, so R.graph is
    // unchanged and every pin that measures the card still measures the same
    // rect. Only the picture gets shorter.
    p.removeFromBottom (kMatchReadoutH);
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
