#pragma once
// =============================================================================================
//  EJMatchChatCard.h - THE VISIBLE CARD A SUCCESSFUL PRESS LEAVES BEHIND
//  (Sean's 2a test of 31 Oct 2026, 13:45, on CCB49D4F; Kathy agreeing the same hour)
// =============================================================================================
//  His words:
//
//      *"AI MATCH with enough Live audio made a match_report, and the log says 'offered to the
//      Whole session chat, which is not built yet (U2) - nothing was posted'. So a successful press
//      shows NOTHING, which reads as 'the button isn't wired'."*
//
//  He is right, and the log line was mine. postToSessionChat was a stub that logged and dropped the
//  card, because A3's Whole session chat does not exist yet. A press that WORKED was therefore
//  indistinguishable from a press that did nothing, which is the same fault as 2a (a correct
//  refusal reaching a person as silence) in the opposite direction.
//
//  His ruling: until A3 exists, the report goes to the chat that is OPEN NOW, never into nothing.
//  Kathy agrees and asked for the message id back so her "Report in chat" line can only appear when
//  a card really arrived.
//
//  ---- WHY THE WORDS HERE ARE NOT NEW WORDS ---------------------------------------------------
//
//  The report's wording belongs to EJMatchReport.h and must have exactly one author. So this builds
//  the visible prose out of the PAYLOAD'S OWN STRINGS - moves[].text and discriminator, which the
//  report builder already wrote - plus matchReportHeading, which is that builder's own function. No
//  sentence is invented here, which is why a change to the report's wording cannot leave this
//  behind.
//
//  Pure, so a pin can read it without a chat, a window or a host.
// =============================================================================================

#include <JuceHeader.h>
#include "EJMatchReport.h"
#include "EJMatchHandOff.h"   // kMatchReportKind, and the payload this reads

namespace echojay
{

/** The prose a posted card shows, from the payload the press built. Empty when the payload carries
    nothing a person could read, which the caller treats as "nothing was posted" rather than
    drawing an empty row. */
inline juce::String matchCardProse (const juce::String& kind, const juce::var& payload)
{
    if (kind != kMatchReportKind) return {};
    const auto* o = payload.getDynamicObject();
    if (o == nullptr) return {};

    // THE HEADING, through the report builder's own function so the two cannot differ.
    const auto refName = payload.getProperty ("reference", juce::var())
                                .getProperty ("name", juce::var()).toString();
    const auto windowStr = payload.getProperty ("window", juce::var()).toString();
    const auto mode = windowStr == "loudest30" ? MatchWindowMode::Loudest30 : MatchWindowMode::Whole;

    juce::StringArray out;
    if (refName.isNotEmpty()) out.add (matchReportHeading (refName, mode));

    // THE LINES, as the report wrote them. A NAMED var first: getProperty returns by value and a
    // getArray() on a temporary reads a freed list (the 28 Oct rule, four rows and three rounds).
    const juce::var moves = payload.getProperty ("moves", juce::var());
    if (auto* arr = moves.getArray())
        for (const auto& m : *arr)
        {
            const auto t = m.getProperty ("text", juce::var()).toString();
            if (t.isNotEmpty()) out.add (t);
        }

    const auto disc = payload.getProperty ("discriminator", juce::var()).toString();
    if (disc.isNotEmpty()) out.add (disc);

    // A heading on its own is not a report. If the press had nothing to say, the page's own line
    // says so and no card is posted: a row reading only "Matched against X" would be the silence
    // his test found, dressed up.
    if (out.size() <= 1) return {};
    return out.joinIntoString ("\n");
}

} // namespace echojay
