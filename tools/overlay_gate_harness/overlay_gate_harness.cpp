// overlay_gate_harness — COMMIT 2 guard, extended by COMMIT 2c (17 Sep 2026):
// ONE pure function decides whether the chain panel is editable:
// chainEditableFor(isLinkRack, rackLockState, borrowed). The own chain is
// NEVER gated; a Link rack is editable ONLY when its borrow session is
// ENGAGED (borrowed == true) — the SAME fact that paints the rack pill
// orange (PluginEditor.cpp: v.borrowed), so gate and colour cannot disagree.
// Held-but-not-engaged is NOT editable (RED on the 2-argument COMMIT 2 gate,
// which opened on Held). chainLockStateText(state, borrowed) is the single
// source of the one-line overlay text and the Build/Apply refusal.
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
    std::printf ("overlay_gate_harness: chainEditableFor(isLinkRack, state, borrowed) x 4 states x {own, link} x {borrowed, not}\n");
    const S states[] = { S::Idle, S::Held, S::WaitRecency, S::HeldByOther };

    std::printf ("== OWN chain: editable in EVERY state, borrowed or not (never gated) ==\n");
    for (S s : states)
        for (bool b : { false, true })
            check (chainEditableFor (false, s, b) == true, juce::String ("own + ") + nm (s) + (b ? " + borrowed" : "") + " -> editable");

    std::printf ("== LINK rack: editable ONLY when the borrow session is ENGAGED ==\n");
    for (S s : states)
        check (chainEditableFor (true, s, false) == false, juce::String ("link + ") + nm (s) + " + NOT engaged -> BLOCKED");
    check (chainEditableFor (true, S::Held, false) == false, "Held-but-not-engaged (the blue window) -> BLOCKED (RED on the COMMIT 2 gate)");
    check (chainEditableFor (true, S::Held, true)  == true,  "Held + engaged (the pill is orange) -> editable");
    check (chainEditableFor (true, S::Idle, false) == false, "SESSION-OPEN case: Link rack selected on load, not engaged -> BLOCKED");

    std::printf ("== the one line of state text (overlay AND Build/Apply refusal share it) ==\n");
    check (chainLockStateText (S::Idle, false)        == juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6"), "Idle        -> \"Connecting to rack…\"");
    check (chainLockStateText (S::Held, false)        == juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6"), "Held, not engaged -> \"Connecting to rack…\"");
    check (chainLockStateText (S::HeldByOther, false) == "Rack held by another EchoJay",                             "HeldByOther -> \"Rack held by another EchoJay\"");
    check (chainLockStateText (S::WaitRecency, false) == juce::String::fromUTF8 ("Waiting for audio\xE2\x80\xA6"),  "WaitRecency -> \"Waiting for audio…\"");
    check (chainLockStateText (S::Held, true).isEmpty(),                                                             "Held + engaged -> (no text: overlay cleared)");

    std::printf ("\n==== overlay_gate_harness: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
