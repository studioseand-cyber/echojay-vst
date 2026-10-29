#pragma once

#include <JuceHeader.h>
#include <optional>
#include "EJMatchTab.h"           // MatchHost, MatchSides, MatchSlotPair, CaptureSnapshot
#include "EJMatchWindowSides.h"   // matchSidesForWindow, matchWindowMemory

// =============================================================================
//  THE MATCH PAGE OVER MATCHHOST: which capture and which window it reads.
// =============================================================================
//
// One place turns the host's answers into the page's, so paint, the press and
// the hand-off cannot disagree about which capture or which window they are on.
// It is a header, not a static in EJMatchTab.cpp, so a pin can drive it with a
// MatchHost of its own.
//
// THE MIX SIDE'S SERIES IS THE SLOT'S OWN CAPTURE (round 5a; A's 7db71816,
// Kathy's R4-2). MatchHost::captureForSlot answers for the slot on screen: a
// Snapshot slot gets the capture at its own index, never the newest one, and
// every other kind gets nothing. latestCapture() is not read here: an older
// snapshot in the mix slot windowed from the newest capture would be two
// moments presented as one.
//
// NO SUBSTITUTE. A slot with no capture of its own (Live, a workspace review,
// anything not a Snapshot) gives the window no mix series, so in "loudest30"
// that side has no bands and `why` says so (EJMatchWindowSides.h). A Live mix
// slot keeps 4a's line, "Capture at least 30 s of the chorus to make a plan."
// =============================================================================

namespace echojay
{

struct MatchPageView
{
    MatchWindowedSides w;
    MatchReadinessContext ctx;
    juce::String refKey;
    /** The capture the mix slot is showing, by value; nullopt for any slot that
        is not a Snapshot. */
    std::optional<CaptureSnapshot> mixCapture;
};

/** The sides over the reference's window, the mix's series from its own slot
    and the reference's from selectedReference() (C2 section 5.2). */
inline MatchPageView matchPageViewOf (const MatchHost& host, const MatchSides& sides,
                                      const MatchSlotPair& slots)
{
    MatchPageView v;
    const auto* ref = host.selectedReference();
    v.refKey = ref != nullptr ? ref->path : juce::String();
    const auto mode = matchWindowMemory().get (v.refKey);
    const bool live = slots.mix != nullptr && slots.mix->kind == CompareSlotState::Kind::Live;
    if (slots.mix != nullptr) v.mixCapture = host.captureForSlot (*slots.mix);
    v.w = matchSidesForWindow<MeterEngine::HopFrame> (sides.mix, sides.ref,
                                                      v.mixCapture ? &v.mixCapture->hopFrames : nullptr,
                                                      ref != nullptr ? &ref->hopFrames : nullptr,
                                                      mode, live);
    v.ctx.mixIsLive = live;
    if (mode == MatchWindowMode::Loudest30 && ! live) v.ctx.windowWhy = v.w.why;
    return v;
}

} // namespace echojay
