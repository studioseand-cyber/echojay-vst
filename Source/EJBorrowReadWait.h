#pragma once
// =============================================================================
//  EJBorrowReadWait — what to do when a borrowed dial read's window runs out
//  (18 October 2026, queue item 22; Sean: "wait on BUSY with a bound, then say
//   it stayed unreachable for N s")
// =============================================================================
//
//  THE DEFECT THIS ANSWERS. Selecting a Link pulls its plugins' state one slot
//  at a time over the ctrl-cmd channel, with a 5 s window per slot and one
//  re-request. The 17 Oct gate's S6 log:
//
//      EJRackLock: released uid=d684c7e220
//      EJBorrow: dial info cleared for uid=d684c7e220 (rack rebuilt or Link gone)
//      EJBorrow: slot 1 ("EchoJay Compressor") of d684c7e220 unanswered after 5.0 s - re-requesting once
//      EJBorrow: read for d684c7e220 ABANDONED after 0 of 3 slots - the selection is now (none)
//
//  The row before had rebuilt that rack. A Link loading plugins does not answer
//  a state pull, so the window expired, the request was sent AGAIN at the worst
//  possible moment, and the read ended with nothing — after which the agent
//  told a user their track was gone. Every line there is true and the conclusion
//  was false, because nothing in the chain could tell BUSY from DEAD.
//
//  Since 18 Oct a Link publishes a BUSY byte into its registry row before it
//  loads a single plugin, so the question can simply be asked. Three rules:
//
//   1. THE RETRY IS FOR A LOST REQUEST. A rack that says it is rebuilding has
//      not lost anything, so waiting on it must not spend the retry — and must
//      not re-send, because a second request to a rack that is about to answer
//      the first is how an answer arrives twice.
//   2. THE WAIT IS BOUNDED, by the same figure as every other busy wait in this
//      build (EchoJayProcessor::kBusyWaitHardMaxMs). One question, one number.
//   3. WHAT IT SAYS WHEN IT GIVES UP IS A READING, NOT A GUESS. "no answer from
//      the Link in 5.0 s, two requests" describes a broken Link; said about one
//      that was busy, it is the second false thing a user is told. A rack that
//      was rebuilding is reported as having stayed unreachable for N s; a rack
//      whose registry row has gone is reported as gone; and only a rack that is
//      plainly there and silent keeps the old sentence.
//
//  WHY IT IS A FUNCTION AND NOT THREE BRANCHES IN A LAMBDA. It lives inside a
//  nested callAfterDelay chain in the editor, which a guard cannot reach without
//  a window, a message loop and a second process answering ctrl-cmds. The policy
//  and its sentences are pure, so they are here, asserted directly by
//  borrow_read_wait_guard, and the editor is left holding only the plumbing.
#include <JuceHeader.h>

namespace echojay::borrowread
{
enum class Next
{
    WaitOnBusy,   ///< the rack says it is rebuilding: poll again, do NOT re-send, do NOT spend the retry
    Retry,        ///< the one re-request, with a fresh seq (9 Sep 2026)
    GiveUp        ///< this slot is abandoned; `failure` names it and why, for the §5e refusal list
};

/// What the caller knows at the moment a slot's window expired.
struct Reading
{
    double       elapsedS  = 0.0;    ///< since the FIRST request for this slot (the retry does not reset it)
    bool         retried   = false;  ///< the one re-request has already been used for this slot
    bool         sawBusy   = false;  ///< this slot's read has found the rack rebuilding at least once
    bool         found     = false;  ///< the registry still lists a row for that uid
    bool         busy      = false;  ///< ...and that row says the rack is rebuilding
    int          hardMaxMs = 30000;  ///< the bound (EchoJayProcessor::kBusyWaitHardMaxMs)
    juce::String slotName;           ///< for the sentence
    juce::String uid;                ///< for the log line
    int          slot1     = 1;      ///< 1-based slot number, for both
    int          seq       = 0;      ///< the last request's seq, for the old sentence
};

struct Decision
{
    Next         next    = Next::Retry;
    bool         sawBusy = false;   ///< write back into the read's per-slot state
    juce::String failure;           ///< GiveUp only: the named failure for the user-facing list
    juce::String logLine;           ///< empty when there is nothing new worth a line
};

/// One decision, no side effects. `sayWaiting` is false once this slot has already logged its waiting line,
/// so a thirty-second rebuild produces one line rather than thirty.
inline Decision decide (const Reading& r, bool sayWaiting)
{
    Decision d;
    d.sawBusy = r.sawBusy;
    const auto where = "slot " + juce::String (r.slot1) + " (\"" + r.slotName + "\") of " + r.uid;
    const auto secs  = juce::String (r.elapsedS, 1);

    if (r.busy)
    {
        d.sawBusy = true;
        // Rule 2: bounded on the TOTAL wait for this slot, which is what the figure means on the chain-ack
        // side too. Past it, rule 3's sentence - Sean's own words for it.
        if (r.elapsedS * 1000.0 < (double) r.hardMaxMs)
        {
            d.next = Next::WaitOnBusy;
            if (sayWaiting)
                d.logLine = "EJBorrow: " + where + " has not answered in " + secs + " s and that rack says it is "
                            "REBUILDING - waiting on it (up to " + juce::String (r.hardMaxMs / 1000) + " s), not "
                            "re-requesting: a busy rack has not lost the request";
            return d;
        }
        d.next    = Next::GiveUp;
        d.failure = r.slotName + " (that rack stayed unreachable for " + secs + " s - it was rebuilding its "
                                 "chain throughout)";
        d.logLine = "EJBorrow: " + where + " STAYED UNREACHABLE for " + secs + " s - rebuilding throughout, so "
                    "this slot is given up with that said";
        return d;
    }

    if (! r.retried)
    {
        d.next    = Next::Retry;
        d.logLine = "EJBorrow: " + where + " unanswered after " + secs + " s (seq " + juce::String (r.seq)
                  + ") - re-requesting once";
        return d;
    }

    d.next = Next::GiveUp;
    if (! r.found)
        d.failure = r.slotName + " (that rack is no longer in this session)";
    else if (r.sawBusy)
        d.failure = r.slotName + " (that rack stayed unreachable for " + secs + " s while it rebuilt its chain)";
    else
        d.failure = r.slotName + " (no answer from the Link in " + secs + " s, two requests, last seq "
                  + juce::String (r.seq) + ")";
    return d;
}
} // namespace echojay::borrowread
