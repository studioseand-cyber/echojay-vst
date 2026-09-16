#pragma once
// ChainEditGate — COMMIT 2 (17 Sep 2026): ITEM 1, the rack loading state.
//
// ONE pure decision, used by the chain panel's overlay, by every panel edit
// callback, and by the chat Build/Apply route: is the chain panel editable
// right now? The OWN chain is NEVER gated. A LINK rack is editable ONLY while
// its lock is Held (the rack has gone orange). Idle (acquiring — including a
// rack selected on session load, not yet orange), WaitRecency and HeldByOther
// are all "not yet": greyed loading state, nothing touchable, one line of
// state text. The companion text function is the single source of that line,
// so the overlay and the Build/Apply refusal can never say different things.
//
// Pinned by tools/overlay_gate_harness (4 states x {own, link}).
#include <JuceHeader.h>
#include "PluginProcessor.h"

inline bool chainEditableFor (bool isLinkRack, EchoJayProcessor::RackLockState state) noexcept
{
    if (! isLinkRack) return true;                                   // the own chain, always
    return state == EchoJayProcessor::RackLockState::Held;          // a Link rack: only when held
}

inline juce::String chainLockStateText (EchoJayProcessor::RackLockState state)
{
    switch (state)
    {
        case EchoJayProcessor::RackLockState::Idle:        return juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6");
        case EchoJayProcessor::RackLockState::HeldByOther: return "Rack held by another EchoJay";
        case EchoJayProcessor::RackLockState::WaitRecency: return juce::String::fromUTF8 ("Waiting for audio\xE2\x80\xA6");
        case EchoJayProcessor::RackLockState::Held:        break;
    }
    return {};                                                       // editable: no text, overlay down
}
