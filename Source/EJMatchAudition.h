#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <optional>
#include "EJMatchHandOff.h"   // MatchHost, MatchSlotPair, the page view, the hand-off row this sits under

// =============================================================================
//  ROUND 8b: THE AUDITION SWITCH AND THE LEVEL MATCH (Sean's ruling, 30 Oct 2026)
// =============================================================================
//
// THE SWITCH flips what you hear between the mix and the reference, through
// MatchHost::setAudible. A's setAudible TOGGLES the side it is given (a side
// already audible is muted), so the switch never names the side already
// audible, with one exception below for a Live mix.
//
// THE REFERENCE PLAYS AT ITS REAL LEVEL BY DEFAULT. The level gap, from the page
// view for the selected window (matchPageFor's levelGapDb), is shown beside the
// switch before the first flip: "Reference is 6.2 dB louder" or "Reference is
// 3.1 dB quieter".
//
// THE LEVEL MATCH is a toggle, off by default. It cuts the REFERENCE only, and
// the line reads the gain the host actually applied: "Matched 24 of 30 dB" when
// the host's floor stops it, never just "matched". When the reference is
// quieter there is nothing to cut, so no toggle is offered: never one shown on
// and doing nothing.
//
// NEVER AN OLD ANSWER. The gain is asked for again, and the line redrawn from
// that call's return, whenever the window, either slot, the reference or the
// toggle changes. The answer is held with the comparison it was for, and a
// line is drawn from it only while that comparison is still the one on screen.
//
// THE CALL IS NOT ON THIS BASE. MatchHost has no setReferenceGainDb yet
// (REQUESTS_FOR_A_8, R8-1). So K's logic is written against the agreed
// interface, MatchReferenceGainPort below, which the pins drive with a fake. The
// page has no port until A's call lands (matchLevelPortOf in EJMatchTab.cpp
// returns nullptr), so it shows the gap line and offers no toggle.
// =============================================================================

namespace echojay
{

/** THE AGREED INTERFACE for A's call (R8-1). When MatchHost gains it, the page's
    port is a two-line adapter over it. */
struct MatchReferenceGainPort
{
    virtual ~MatchReferenceGainPort() = default;
    /** Sets the reference's playback gain: 0 is its real level, negative a
        cut. Returns the gain actually applied, which the floor can stop short
        of wantedDb. */
    virtual float setReferenceGainDb (float wantedDb) = 0;
    /** The deepest cut the host applies, dB (negative). */
    virtual float referenceGainFloorDb() const = 0;
};

// -----------------------------------------------------------------------------
// THE WORDS
// -----------------------------------------------------------------------------

/** One decimal, and none when it is a whole number: "6.2", "24". */
inline juce::String matchLevelDbText (float db)
{
    const float r = std::round (std::abs (db) * 10.0f) / 10.0f;
    return std::abs (r - std::round (r)) < 0.001f ? juce::String ((int) std::round (r)) : juce::String (r, 1);
}

/** A gap under this reads as the same level, and offers no match. */
inline constexpr float kMatchLevelSameDb = 0.05f;

/** The gap line, or "" when the gap is not known. */
inline juce::String matchLevelGapLine (std::optional<float> gapDb)
{
    if (! gapDb) return {};
    if (*gapDb >= kMatchLevelSameDb)  return "Reference is " + matchLevelDbText (*gapDb) + " dB louder";
    if (*gapDb <= -kMatchLevelSameDb) return "Reference is " + matchLevelDbText (*gapDb) + " dB quieter";
    return "Reference is at your mix's level";
}

/** The toggle is offered only when there is a cut to make and a call to make it with. */
inline bool matchLevelMatchOffered (std::optional<float> gapDb, const MatchReferenceGainPort* port)
{
    return port != nullptr && gapDb && *gapDb >= kMatchLevelSameDb;
}

/** What the host did, from its own answer: "Matched 24 of 30 dB". */
inline juce::String matchLevelMatchedLine (float appliedDb, float wantedDb)
{
    return "Matched " + matchLevelDbText (appliedDb) + " of " + matchLevelDbText (wantedDb) + " dB";
}

// -----------------------------------------------------------------------------
// THE STATE, in the panel's own properties (MatchPanel is declared in A's
// PluginEditor.h and gains no members)
// -----------------------------------------------------------------------------

inline juce::Identifier matchLevelOnId()      { return "ejMatchLevelOn"; }
inline juce::Identifier matchLevelKeyId()     { return "ejMatchLevelKey"; }
inline juce::Identifier matchLevelAppliedId() { return "ejMatchLevelApplied"; }
inline juce::Identifier matchLevelWantedId()  { return "ejMatchLevelWanted"; }

/** One slot, as a key: its kind and what picks it. */
inline juce::String matchSlotKey (const CompareSlotState* s)
{
    if (s == nullptr) return "none";
    return juce::String ((int) s->kind) + ":" + juce::String (s->index) + ":" + s->wsReviewId;
}

/** The comparison an answer is for: the window, both slots, the reference, and
    what was asked (the toggle and the cut, to 0.1 dB). */
inline juce::String matchLevelKey (const MatchPageView& v, const MatchSlotPair& slots, float wantedDb)
{
    juce::Array<juce::var> a { juce::var (v.w.mode == MatchWindowMode::Whole ? "whole" : "loudest30"),
                               juce::var (matchSlotKey (slots.mix)), juce::var (matchSlotKey (slots.ref)),
                               juce::var (v.refKey), juce::var (std::round (wantedDb * 10.0f) / 10.0f) };
    return juce::JSON::toString (juce::var (a), true);
}

/** The gain this comparison asks for: the cut when the match is on and offered, else 0. */
inline float matchLevelWantedDb (const MatchPageView& v, const MatchReferenceGainPort* port,
                                 const juce::NamedValueSet& page)
{
    const bool on = (bool) page.getWithDefault (matchLevelOnId(), false);
    return on && matchLevelMatchOffered (v.levelGapDb, port) ? -*v.levelGapDb : 0.0f;
}

/** ASKS THE HOST AGAIN when the comparison or the toggle has changed, and holds
    the answer with the comparison it was for. A toggle left on where no match
    is offered (the reference became quieter) is turned off. True when the call
    was made. Nothing without a port. */
inline bool matchLevelSync (MatchReferenceGainPort* port, const MatchPageView& v, const MatchSlotPair& slots,
                            juce::NamedValueSet& page)
{
    if (port == nullptr) return false;
    if (! matchLevelMatchOffered (v.levelGapDb, port)) page.set (matchLevelOnId(), false);
    const float wanted = matchLevelWantedDb (v, port, page);
    const auto key = matchLevelKey (v, slots, wanted);
    if (page.getWithDefault (matchLevelKeyId(), juce::var()).toString() == key) return false;
    const float applied = port->setReferenceGainDb (wanted);
    page.set (matchLevelKeyId(), key);
    page.set (matchLevelAppliedId(), (double) applied);
    page.set (matchLevelWantedId(), (double) wanted);
    return true;
}

/** What the audition row shows. */
struct MatchLevelLine
{
    juce::String gap;        ///< "Reference is 6.2 dB louder"; "" when not known
    bool offerToggle = false;
    bool on = false;
    juce::String matched;    ///< "Matched 24 of 30 dB" while on, from the held answer
};

/** The row, from the view and the held answer. The matched line is drawn only
    while the held answer is for the comparison on screen; otherwise it is empty
    until the next sync asks again. */
inline MatchLevelLine matchLevelLineOf (const MatchPageView& v, const MatchSlotPair& slots,
                                        const MatchReferenceGainPort* port, const juce::NamedValueSet& page)
{
    MatchLevelLine l;
    l.gap = matchLevelGapLine (v.levelGapDb);
    l.offerToggle = matchLevelMatchOffered (v.levelGapDb, port);
    l.on = l.offerToggle && (bool) page.getWithDefault (matchLevelOnId(), false);
    if (! l.on) return l;
    const float wanted = matchLevelWantedDb (v, port, page);
    if (page.getWithDefault (matchLevelKeyId(), juce::var()).toString() != matchLevelKey (v, slots, wanted))
        return l;
    const float applied = (float) (double) page.getWithDefault (matchLevelAppliedId(), 0.0);
    l.matched = matchLevelMatchedLine (applied, wanted);
    return l;
}

// -----------------------------------------------------------------------------
// THE SWITCH
// -----------------------------------------------------------------------------

/** The side setAudible gets for one press. Hearing the reference: back to the
    mix. Hearing the mix, or nothing: the reference. Never the side already
    audible, because A's setAudible would mute it; EXCEPT a Live mix, which is
    the host's own signal and has no stream to make audible, so going back to
    it means switching the reference off (setAudible on the reference's side). */
inline MatchRefSide matchAuditionFlipTarget (bool refIsTop, bool refAudible, bool mixIsLive)
{
    const auto refSide = refIsTop ? MatchRefSide::Top : MatchRefSide::Bottom;
    const auto mixSide = refIsTop ? MatchRefSide::Bottom : MatchRefSide::Top;
    if (refAudible) return mixIsLive ? refSide : mixSide;
    return refSide;
}

/** The switch's words: what you are hearing now. */
inline juce::String matchAuditionLabel (bool refAudible)
{
    return refAudible ? "Hearing reference" : "Hearing mix";
}
inline constexpr const char* kMatchLevelToggleLabel = "Match level";

// -----------------------------------------------------------------------------
// WHERE IT SITS: one row under the hand-off row, inside the graph card
// -----------------------------------------------------------------------------

inline constexpr int kMatchAuditionSwitchW = 112;
inline constexpr int kMatchLevelToggleW    = 84;

struct MatchAuditionRects { juce::Rectangle<int> line, toggle, flip; };

inline MatchAuditionRects matchAuditionRects (juce::Rectangle<int> graph)
{
    MatchAuditionRects o;
    const auto sw = matchWindowSwitchRect (graph);
    const auto hr = matchHandOffRects (graph);
    if (sw.isEmpty() || hr.planLine.isEmpty()) return o;
    const auto inner = graph.reduced (kMatchWindowSwitchPad);
    // Under the hand-off row and the plan line (matchHandOffRects).
    const int y = hr.planLine.getBottom() + kMatchHandOffGap;
    if (y + kMatchWindowSwitchH > inner.getBottom()) return o;
    o.flip   = { sw.getRight() - kMatchAuditionSwitchW, y, kMatchAuditionSwitchW, kMatchWindowSwitchH };
    o.toggle = { o.flip.getX() - kMatchHandOffGap - kMatchLevelToggleW, y, kMatchLevelToggleW, kMatchWindowSwitchH };
    o.line   = { inner.getX(), y, o.toggle.getX() - kMatchHandOffGap - inner.getX(), kMatchWindowSwitchH };
    return o;
}

/** Every string the row can put on the page. */
inline juce::StringArray matchAuditionPageStrings()
{
    return { matchAuditionLabel (false), matchAuditionLabel (true), kMatchLevelToggleLabel,
             "Reference is ", " dB louder", " dB quieter", "Reference is at your mix's level", "Matched " };
}

} // namespace echojay
