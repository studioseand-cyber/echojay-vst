// borrow_read_wait_guard — 18 October 2026, queue item 22. THE BORROWED DIAL READ, when it gets no answer.
//
// Sean, 18 Oct: *"do the borrowed dial read next (item 22: wait on BUSY with a bound, then say 'it stayed
// unreachable for N s')."*
//
// WHAT WENT WRONG, from the 17 Oct gate's own S6 log. Selecting a Link pulls its plugins' state one slot at a
// time, 5 s per slot, one re-request. The row before had rebuilt that rack:
//
//     EJBorrow: dial info cleared for uid=d684c7e220 (rack rebuilt or Link gone)
//     EJBorrow: slot 1 ("EchoJay Compressor") of d684c7e220 unanswered after 5.0 s - re-requesting once
//     EJBorrow: read for d684c7e220 ABANDONED after 0 of 3 slots - the selection is now (none)
//
// A Link loading plugins does not answer a state pull. So the window expired, the request went out AGAIN at the
// worst possible moment, the read ended with nothing, and the agent then told a user their track was gone.
// Every line is true; the conclusion was false; nothing in the chain could tell BUSY from DEAD.
//
// THE LEGS are the three rules in EJBorrowReadWait.h, each asserted on both sides of its own boundary - because
// a guard that only tests the new case cannot tell a fix from a change of behaviour:
//   (1) a busy rack is WAITED on, and the wait does not re-send and does not spend the retry;
//   (2) ...and it is BOUNDED: past kBusyWaitHardMaxMs the slot is given up;
//   (3) ...and what it says then is Sean's sentence - "it stayed unreachable for N s" - never the old
//       "no answer from the Link ... two requests", which describes a broken Link;
//   (4) a rack that is NOT busy still gets exactly the old behaviour: one retry, then the old sentence
//       verbatim. This is the leg that says the change is additive;
//   (5) a rack whose registry row has GONE is reported as gone rather than as silent;
//   (6) a rack that WAS busy and has just stopped is reported as having been unreachable while it rebuilt,
//       not as a Link that ignored two requests;
//   (7) the waiting line is said ONCE per slot, not once per poll;
//   (8) and `sawBusy` is carried forward, because leg (6) is only possible if it is.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJBorrowReadWait.h"
#include <cstdio>

using namespace echojay::borrowread;

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }

// A slot's window has just expired. Everything the policy needs, with the defaults of the case that produced
// the defect: slot 1, a named plugin, a live row.
Reading at (double elapsedS, bool retried, bool sawBusy, bool found, bool busy)
{
    Reading r;
    r.elapsedS = elapsedS; r.retried = retried; r.sawBusy = sawBusy; r.found = found; r.busy = busy;
    r.hardMaxMs = 30000;
    r.slotName = "EchoJay Compressor"; r.uid = "d684c7e220"; r.slot1 = 1; r.seq = 3024;
    return r;
}
const char* nameOf (Next n)
{ return n == Next::WaitOnBusy ? "WaitOnBusy" : n == Next::Retry ? "Retry" : "GiveUp"; }
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("borrow_read_wait_guard: a borrowed dial read against a rack that is rebuilding (item 22)\n");

    // ---- (1) A BUSY RACK IS WAITED ON, NOT RE-ASKED ------------------------------------------------------
    std::printf ("== (1) the rack says it is rebuilding ==\n");
    {
        const auto d = decide (at (5.0, false, false, true, true), true);
        std::printf ("    at 5.0 s, retry unused, busy -> %s\n", nameOf (d.next));
        check (d.next == Next::WaitOnBusy,
               "(1) a rack that says it is REBUILDING is waited on - the old code re-requested here, which is a "
               "second request to a rack that is about to answer the first",
               nameOf (d.next));
        check (d.failure.isEmpty(),
               "(1) ...and nothing is reported to the user yet, because nothing has failed yet");
        check (d.logLine.contains ("REBUILDING") && d.logLine.contains ("not re-requesting")
                 && d.logLine.contains ("has not lost the request"),
               "(1) ...and the log says what it is doing and why", d.logLine);
        check (d.sawBusy, "(8) ...and it records that this slot has seen the rack busy, which leg (6) needs");
    }
    // THE RETRY IS NOT SPENT. The same reading, but with the retry already used: still a wait, because the
    // retry is for a LOST request and a busy rack has not lost one.
    {
        const auto d = decide (at (5.0, true, true, true, true), false);
        check (d.next == Next::WaitOnBusy,
               "(1) the retry is for a lost request, so a busy rack is waited on whether or not it has been "
               "used - and a wait never consumes it either",
               nameOf (d.next));
    }

    // ---- (2) AND THE WAIT IS BOUNDED ---------------------------------------------------------------------
    std::printf ("== (2) and the patience has an end ==\n");
    {
        const auto in  = decide (at (29.9, false, true, true, true), false);
        const auto out = decide (at (30.1, false, true, true, true), false);
        check (in.next == Next::WaitOnBusy && out.next == Next::GiveUp,
               "(2) bounded by kBusyWaitHardMaxMs (30 s): still waiting at 29.9 s, given up at 30.1 s - one "
               "figure for every busy wait in this build, so the two channels cannot drift apart",
               juce::String ("29.9 -> ") + nameOf (in.next) + ", 30.1 -> " + nameOf (out.next));

        // ---- (3) ...AND SEAN'S SENTENCE ------------------------------------------------------------------
        std::printf ("    failure at the bound: %s\n", out.failure.toRawUTF8());
        check (out.failure.contains ("stayed unreachable for 30.1 s"),
               "(3) the sentence is the one Sean asked for: it stayed unreachable for N s", out.failure);
        check (out.failure.contains ("rebuilding its chain throughout"),
               "(3) ...and it says WHY it was unreachable, which is the fact the user is looking at");
        check (! out.failure.contains ("no answer from the Link") && ! out.failure.contains ("two requests"),
               "(3) ...and NOT the old sentence, which describes a broken Link and was the second false thing "
               "said about a working one",
               out.failure);
    }

    // ---- (4) A RACK THAT IS NOT BUSY IS TREATED EXACTLY AS BEFORE ---------------------------------------
    std::printf ("== (4) nothing about a silent, idle rack has changed ==\n");
    {
        const auto first = decide (at (5.0, false, false, true, false), true);
        check (first.next == Next::Retry && first.logLine.contains ("re-requesting once")
                 && first.logLine.contains ("seq 3024"),
               "(4) the first expiry against a rack that is NOT rebuilding still re-requests once, with the "
               "seq in the line (9 Sep 2026 behaviour, untouched)",
               first.logLine);
        const auto second = decide (at (10.0, true, false, true, false), false);
        std::printf ("    failure after two requests: %s\n", second.failure.toRawUTF8());
        check (second.next == Next::GiveUp
                 && second.failure == "EchoJay Compressor (no answer from the Link in 10.0 s, two requests, "
                                      "last seq 3024)",
               "(4) ...and the second expiry gives the OLD sentence verbatim. A rack that is there and silent "
               "really has ignored two requests",
               second.failure);
    }

    // ---- (5) A ROW THAT HAS GONE IS SAID TO HAVE GONE ---------------------------------------------------
    std::printf ("== (5) the registry has no row for that uid any more ==\n");
    {
        const auto d = decide (at (10.0, true, false, false, false), false);
        std::printf ("    failure: %s\n", d.failure.toRawUTF8());
        check (d.next == Next::GiveUp && d.failure.contains ("no longer in this session"),
               "(5) an absent row is reported as absent - a fact the registry has, unlike a guess about why a "
               "Link is quiet",
               d.failure);
    }

    // ---- (6) BUSY, THEN STOPPED -------------------------------------------------------------------------
    std::printf ("== (6) it was rebuilding, and by the time we gave up it was not ==\n");
    {
        const auto d = decide (at (11.5, true, true, true, false), false);
        std::printf ("    failure: %s\n", d.failure.toRawUTF8());
        check (d.next == Next::GiveUp && d.failure.contains ("stayed unreachable for 11.5 s")
                 && d.failure.contains ("while it rebuilt its chain"),
               "(6) a rack that was busy during this slot's read is reported as having been unreachable, not as "
               "one that ignored two requests - which is the whole of why sawBusy is carried",
               d.failure);
        check (! d.failure.contains ("two requests"), "(6) ...and the misleading half is gone with it");
    }

    // ---- (7) ONE LINE PER SLOT, NOT ONE PER POLL --------------------------------------------------------
    std::printf ("== (7) a thirty-second rebuild is one log line, not thirty ==\n");
    {
        const auto said   = decide (at (5.0, false, false, true, true), true);
        const auto quiet  = decide (at (6.0, false, true,  true, true), false);
        check (said.logLine.isNotEmpty() && quiet.logLine.isEmpty(),
               "(7) the waiting line is offered once per slot and then withheld - the caller passes false once "
               "it has said it, because a line repeated ten times a second destroys the evidence around it",
               quiet.logLine.isEmpty() ? juce::String ("second poll: silent") : quiet.logLine);
        check (quiet.next == Next::WaitOnBusy, "(7) ...and it is still waiting, line or no line");
    }

    // ---- (9) THE READ'S OWN SENTENCE IS RETIRED BY THE DRAW (his 11:32, 21 Oct 2026) ------------
    //
    // *"the rack view stuck on "Reading 0 plugin(s) from 88_7even Kick_2_01..." for minutes, while
    // the kHs Compressor sat on the rack and worked. Your 8-second read deadline didn't clear it.
    // ... The line must be replaced as soon as the view has drawn the rack from the sidecar,
    // whatever path set it."*
    //
    // His 11:32, as the log tells it: read START at 11:31:52 on a valid sidecar holding 0 slots,
    // finished in the same second, nothing stranded - so the stall bound was never going to fire.
    // The sentence simply had no end condition. These legs are the end condition, in both
    // directions: what it must retire, and the lines it must never touch.
    {
        std::printf ("== (9) the read notice ==\n");
        using echojay::borrowread::ReadNotice;
        using echojay::borrowread::shouldRetireReadNotice;

        // HIS CASE. The notice was written about a rack that then published one slot; the view has
        // drawn that slot; the read is over.
        ReadNotice his;
        his.line = his.ownedLine = "Reading 0 plugin(s) from 88_7even Kick_2_01...";
        his.ownedUid = his.viewUid = "bb411b8240";
        his.viewValid = true; his.viewSlots = 1; his.drawnSlots = 1; his.readInFlight = false;
        check (shouldRetireReadNotice (his),
               "(9a) RED-FIRST: his own line, over a rack the view has drawn, is RETIRED - this is the "
               "state it sat in for minutes");

        // THE SAME LINE FROM SOME OTHER WRITER: no ownership, retired on the prose - the "whatever
        // path set it" half of his ruling.
        ReadNotice other = his;
        other.ownedLine.clear(); other.ownedUid.clear();
        check (shouldRetireReadNotice (other),
               "(9b) ...and the same sentence with no owner recorded is retired too");

        // NOT DRAWN YET: the view holds three slots, the panel is still showing none. The sentence
        // is TRUE here, and a notice removed now would leave an empty rack with nothing said about
        // it - the opposite failure.
        ReadNotice midPull = his;
        midPull.line = midPull.ownedLine = "Reading 3 plugin(s) from 88_7even Kick_2_01...";
        midPull.viewSlots = 3; midPull.drawnSlots = 0; midPull.readInFlight = true;
        check (! shouldRetireReadNotice (midPull),
               "(9c) a rack the panel has NOT drawn yet keeps its notice - the sentence is still true");

        // AN EMPTY RACK WITH THE READ STILL RUNNING: nothing drawn because there is nothing to
        // draw, and the read has not finished. The one case where 0 == 0 is not a draw.
        ReadNotice emptyBusy = his;
        emptyBusy.viewSlots = 0; emptyBusy.drawnSlots = 0; emptyBusy.readInFlight = true;
        check (! shouldRetireReadNotice (emptyBusy),
               "(9d) an empty rack whose read is still in flight keeps it");
        ReadNotice emptyDone = emptyBusy;
        emptyDone.readInFlight = false;
        check (shouldRetireReadNotice (emptyDone),
               "(9e) ...and loses it the moment that read is over: an empty rack drawn as empty is "
               "drawn, and nothing is reading any more");

        // SOMEONE ELSE'S WORDS ARE NEVER SWEPT. This is the guard that keeps the fix from eating the
        // honesty surfaces that share this line - a refusal, a lock line, an edit flow's message.
        for (auto notOurs : { "Could not finish reading that rack. Select it again.",
                              "Cannot edit this rack: its lock is held elsewhere.",
                              "Reading settings...",
                              "Released - 88_7even Kick_2_01 owns its rack again." })
        {
            ReadNotice n = his;
            n.line = notOurs;            // the read's ownedLine is still the old sentence
            check (! shouldRetireReadNotice (n),
                   juce::String ("(9f) \"") + notOurs + "\" is not the read's line and is left alone");
        }

        // AND NOT ACROSS RACKS: the notice was written about one rack, the view is on another. The
        // proxy fault in person - a line is about a rack, not about "the view".
        ReadNotice elsewhere = his;
        elsewhere.viewUid = "14377e493b";
        elsewhere.line = elsewhere.ownedLine = "Reading 0 plugin(s) from 88_7even Kick_2_01...";
        check (shouldRetireReadNotice (elsewhere),
               "(9g) a stale notice naming ANOTHER rack still goes once this view has drawn - it "
               "cannot be true of the rack on screen either");
        ReadNotice ownedElsewhere = his;
        ownedElsewhere.ownedUid = "14377e493b";
        ownedElsewhere.line = ownedElsewhere.ownedLine = "Please wait...";
        check (! shouldRetireReadNotice (ownedElsewhere),
               "(9h) ...but a line that is neither a read notice nor THIS view's owned words is not "
               "touched, whoever wrote it");

        // AN INVALID VIEW CLAIMS NOTHING. No description, no draw, no retirement.
        ReadNotice invalid = his;
        invalid.viewValid = false;
        check (! shouldRetireReadNotice (invalid),
               "(9i) an invalid view has drawn nothing, so it retires nothing");
    }

    std::printf ("\n==== borrow_read_wait_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
