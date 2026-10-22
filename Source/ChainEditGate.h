#pragma once
// ChainEditGate — COMMIT 2 (17 Sep 2026): ITEM 1, the rack loading state.
//
// ONE pure decision, used by the chain panel's overlay, by every panel edit
// callback, and by the chat Build/Apply route: is the chain panel editable
// right now? The OWN chain is NEVER gated. A LINK rack is editable ONLY while
// its borrow session is ENGAGED (COMMIT 2c, 17 Sep 2026: `borrowed` — the
// SAME fact that paints the rack pill orange, PluginEditor.cpp v.borrowed, so
// the gate and the colour can never disagree). The lock state alone is not
// enough: the lock is taken synchronously inside the select, so Held-but-not-
// engaged is a real window (seconds long while the rack's states are pulled)
// in which the COMMIT 2 gate opened early (blue rack, no overlay, edits sent to
// the Link mid-read). Idle / WaitRecency / HeldByOther / Held-not-engaged are
// all "not yet": greyed loading state, nothing touchable, one line of state
// text. The companion text function is the single source of that line, so
// the overlay and the Build/Apply refusal can never say different things.
//
// Pinned by tools/overlay_gate_harness (4 states x {own, link} x {engaged, not})
// and tools/overlay_liveness_guard (the real objects, select -> engage).
#include <JuceHeader.h>
#include "PluginProcessor.h"

inline bool chainEditableFor (bool isLinkRack, EchoJayProcessor::RackLockState state, bool borrowed) noexcept
{
    juce::ignoreUnused (state);                                      // the lock is necessary for engage, not sufficient for editing
    if (! isLinkRack) return true;                                   // the own chain, always
    return borrowed;                                                 // a Link rack: only when its session is engaged (orange)
}

/** ---- "CONNECTING" MUST BE TRUE (19 Oct 2026, his 17:11 run) ----------------------------------
    *"The V2 rack view's connect to a Link hangs on 'connecting'. Find why on this Link and make the
    view either connect or say plainly it couldn't."*

    IT WAS NEVER CONNECTING. His log: the vocal Link `4814a16004` was released at 17:04:09, and at
    17:06:02 the V2 engaged `bb411b8240` - the Kick - with no engage attempt for the vocal after
    that. A borrow session is single-occupancy, so while it sat on the Kick the vocal's lock state
    was **Idle**, and Idle returned "Connecting to rack..." - a progress message for a process that
    was not running and never would. He watched it for minutes and then opened the Link's own window
    instead, which is the right conclusion to draw from a line that cannot come true.

    Idle means NO LOCK IS HELD AT ALL. Nothing is in flight; there is nothing to wait for. So it now
    says what is true, and when the session is sitting on ANOTHER rack it names it - because that is
    the fact the user needs to act on, and "select this one" is the action.

    `connectedElsewhere` is the display name of the rack that currently holds the session, when that
    is a DIFFERENT rack from the one being described. Empty otherwise, and the caller is the only
    thing that can know it (the gate sees one rack at a time). Defaulted so a caller that cannot say
    gets the honest general wording rather than a wrong specific one.                               */
inline juce::String chainLockStateText (EchoJayProcessor::RackLockState state, bool borrowed,
                                        const juce::String& connectedElsewhere = {},
                                        bool engageInFlight = true)
{
    if (borrowed) return {};                                         // editable: no text, overlay down
    switch (state)
    {
        case EchoJayProcessor::RackLockState::Idle:
            // NOT a progress message: nothing is connecting. Name the other rack when there is one.
            return connectedElsewhere.trim().isNotEmpty()
                     ? juce::String::fromUTF8 ("\xE2\x80\x9C") + connectedElsewhere.trim()
                           + juce::String::fromUTF8 ("\xE2\x80\x9D is the connected rack \xE2\x80\x93 select this one to edit it here.")
                     : juce::String ("Not connected to this rack.");
        case EchoJayProcessor::RackLockState::HeldByOther: return "Rack held by another EchoJay";
        case EchoJayProcessor::RackLockState::WaitRecency: return juce::String::fromUTF8 ("Waiting for audio\xE2\x80\xA6");
        case EchoJayProcessor::RackLockState::Held:
            // ---- HELD IS NOT ALWAYS CONNECTING (Sean's 21:05/21:09, 20 Oct 2026) -------------
            //
            // *"Clicking another rack hangs on 'connecting to rack'."*  Three times in one evening,
            // and twice the honest answer was that nothing was connecting at all: the lock was held
            // for the rack on screen, the 20 Oct ruling means viewing does NOT engage a session, and
            // a stranded read had stopped any engage from ever starting. "Connecting to rack..." on
            // a state that will never finish is the same class of untruth as a floor printed as a
            // measurement - it describes a process that is not running.
            //
            // So HELD now asks whether anything is actually in flight. When something is - a read, a
            // pending engage, an apply - the old words are right and stay. When nothing is, the rack
            // is simply held and unengaged, which is the ordinary resting state of a viewed Link
            // since the ruling, and it says so.
            //
            // The parameter DEFAULTS TRUE so every existing caller and leg keeps today's words until
            // it is taught to answer the question; only the caller that knows says otherwise.
            return engageInFlight
                     ? juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6")
                     : juce::String ("Not connected to this rack.");
    }
    return juce::String ("Not connected to this rack.");
}
