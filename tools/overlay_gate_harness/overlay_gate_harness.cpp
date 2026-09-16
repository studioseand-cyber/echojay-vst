// overlay_gate_harness — COMMIT 2 guard (17 Sep 2026): ITEM 1, the rack
// loading state. ONE pure function decides whether the chain panel is
// editable: chainEditableFor(isLinkRack, rackLockState). The own chain is
// NEVER gated; a Link rack is editable ONLY while the lock is Held. The
// companion chainLockStateText(state) is the single source of the one-line
// state text the overlay shows and the chat Build/Apply route refuses with.
//
// Table: 4 lock states x {own, link}. RED on today's code because the
// function does not exist (and today's behaviour permits edits in Idle:
// remoteWriteLocked only disables bypass/remove, PluginEditor.h:2775).
// The SESSION-OPEN case (a Link rack selected on load, not yet orange) is
// the {link, Idle} row of this same table — no separate path.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainEditGate.h"
#include <cstdio>

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
using S = EchoJayProcessor::RackLockState;
const char* nm (S s)
{
    switch (s) { case S::Idle: return "Idle"; case S::Held: return "Held";
                 case S::WaitRecency: return "WaitRecency"; case S::HeldByOther: return "HeldByOther"; }
    return "?";
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("overlay_gate_harness: chainEditableFor(isLinkRack, state) x 4 states x {own, link}\n");
    const S states[] = { S::Idle, S::Held, S::WaitRecency, S::HeldByOther };

    std::printf ("== OWN chain: editable in EVERY state (never gated) ==\n");
    for (S s : states)
        check (chainEditableFor (false, s) == true, juce::String ("own + ") + nm (s) + " -> editable");

    std::printf ("== LINK rack: editable ONLY when Held ==\n");
    for (S s : states)
    {
        const bool want = (s == S::Held);
        check (chainEditableFor (true, s) == want,
               juce::String ("link + ") + nm (s) + (want ? " -> editable" : " -> BLOCKED"));
    }
    check (chainEditableFor (true, S::Idle) == false,
           "SESSION-OPEN case: Link rack selected on load, lock Idle (not yet orange) -> BLOCKED (same row, same path)");

    std::printf ("== the one line of state text (overlay AND Build/Apply refusal share it) ==\n");
    check (chainLockStateText (S::Idle)        == juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6"), "Idle        -> \"Connecting to rack…\"");
    check (chainLockStateText (S::HeldByOther) == "Rack held by another EchoJay",                             "HeldByOther -> \"Rack held by another EchoJay\"");
    check (chainLockStateText (S::WaitRecency) == juce::String::fromUTF8 ("Waiting for audio\xE2\x80\xA6"),  "WaitRecency -> \"Waiting for audio…\"");
    check (chainLockStateText (S::Held).isEmpty(),                                                             "Held        -> (no text: overlay cleared)");

    std::printf ("\n==== overlay_gate_harness: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
