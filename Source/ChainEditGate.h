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

inline juce::String chainLockStateText (EchoJayProcessor::RackLockState state, bool borrowed)
{
    if (borrowed) return {};                                         // editable: no text, overlay down
    switch (state)
    {
        case EchoJayProcessor::RackLockState::Idle:        return juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6");
        case EchoJayProcessor::RackLockState::HeldByOther: return "Rack held by another EchoJay";
        case EchoJayProcessor::RackLockState::WaitRecency: return juce::String::fromUTF8 ("Waiting for audio\xE2\x80\xA6");
        case EchoJayProcessor::RackLockState::Held:        return juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6");   // held, not yet engaged
    }
    return juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6");
}
