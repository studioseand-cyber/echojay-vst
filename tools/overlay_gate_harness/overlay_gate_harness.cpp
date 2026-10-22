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
    // ---- 19 Oct 2026 (his 17:11 run): IDLE IS NOT "CONNECTING" ----------------------------------
    // The vocal Link's session had been released and the V2's single borrow session was engaged on
    // the KICK, so the vocal's state was Idle - no lock, nothing in flight - and this line said
    // "Connecting to rack..." for as long as he watched it. A progress message for a process that is
    // not running cannot come true, and he gave up and opened the Link's own window.
    check (chainLockStateText (S::Idle, false)        == juce::String ("Not connected to this rack."),
           "Idle        -> \"Not connected to this rack.\"  (RED as it stood: \"Connecting to rack…\", "
           "a progress message for a process that was not running)");
    check (! chainLockStateText (S::Idle, false).containsIgnoreCase ("connecting"),
           "Idle        -> and it never says \"connecting\" in any wording");
    // AND WHEN THE SESSION IS ON ANOTHER RACK, it is named, because the name is the thing he can act
    // on. This is the case his 17:11 run actually hit.
    {
        const auto t = chainLockStateText (S::Idle, false, "88_7even Kick_2_01");
        check (t.contains ("88_7even Kick_2_01") && t.containsIgnoreCase ("select this one"),
               "Idle + another rack connected -> names it and says what to do", t);
        check (! t.containsIgnoreCase ("connecting"),
               "Idle + another rack connected -> still never says \"connecting\"", t);
    }
    // THE OTHER DIRECTION: Held IS in flight, and still says so.
    check (chainLockStateText (S::Held, false)        == juce::String::fromUTF8 ("Connecting to rack\xE2\x80\xA6"), "Held, not engaged -> \"Connecting to rack…\"");
    check (chainLockStateText (S::HeldByOther, false) == "Rack held by another EchoJay",                             "HeldByOther -> \"Rack held by another EchoJay\"");
    check (chainLockStateText (S::WaitRecency, false) == juce::String::fromUTF8 ("Waiting for audio\xE2\x80\xA6"),  "WaitRecency -> \"Waiting for audio…\"");
    check (chainLockStateText (S::Held, true).isEmpty(),                                                             "Held + engaged -> (no text: overlay cleared)");

    std::printf ("== HELD is not always CONNECTING (Sean's 21:05/21:09, 20 Oct 2026) ==\n");
    // *"Clicking another rack hangs on 'connecting to rack'."*  Three times in one evening, and
    // twice nothing was connecting at all: the lock was held for the rack on screen, the 20 Oct
    // ruling means viewing does not engage a session, and a stranded read had stopped any engage
    // from starting. "Connecting to rack..." on a state that will never finish describes a process
    // that is not running - the same class of untruth as a floor printed as a measurement.
    {
        const auto inFlight = chainLockStateText (S::Held, false, {}, true);
        const auto resting  = chainLockStateText (S::Held, false, {}, false);
        check (inFlight.containsIgnoreCase ("connecting"),
               "HC1 HELD with something genuinely in flight still says Connecting", inFlight);
        check (! resting.containsIgnoreCase ("connecting"),
               "HC2 RED-FIRST: HELD with NOTHING in flight does NOT say Connecting - it is the "
               "resting state of a viewed Link since the ruling, not a process", resting);
        check (resting == "Not connected to this rack.",
               "HC3 ...it says the same honest thing Idle says, because it is the same fact to him",
               resting);
        // THE DEFAULT KEEPS TODAY'S WORDS, so every caller and leg not yet taught the question is
        // unchanged - only the overlay, which can actually answer it, passes false.
        check (chainLockStateText (S::Held, false) == inFlight,
               "HC4 the default is in-flight, so an untaught caller behaves exactly as before");
        // AND THE OTHER STATES ARE UNTOUCHED BY THE NEW ARGUMENT.
        check (chainLockStateText (S::Idle, false, {}, false) == chainLockStateText (S::Idle, false, {}, true)
                 && chainLockStateText (S::HeldByOther, false, {}, false) == chainLockStateText (S::HeldByOther, false, {}, true)
                 && chainLockStateText (S::WaitRecency, false, {}, false) == chainLockStateText (S::WaitRecency, false, {}, true),
               "HC5 Idle, HeldByOther and WaitRecency read the same either way - the question only "
               "governs HELD");
        check (chainLockStateText (S::Held, true, {}, false).isEmpty(),
               "HC6 an ENGAGED rack still shows no overlay at all, whatever the question says");
    }

    std::printf ("\n==== overlay_gate_harness: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
