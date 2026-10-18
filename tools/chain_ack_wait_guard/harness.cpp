// chain_ack_wait_guard — 18 October 2026. THE WAITING SIDE of queue items 21, 16 and 13.
//
// WHAT HAPPENED, because every leg below is one sentence of it. The 17 Oct gate's S6 round sent an add to a
// Link the row had just PROVED alive (work_on -> "ok (working on 61cc4c030f)") and the step failed:
//
//     EJLink: chain-cmd v2 -> 61cc4c030f seq=1792285348 source="agent" lease=none
//     EJAgent: step tc_tp FAILED code=not_applied
//              "no chain-ack-61cc4c030f.json with seq 1792285348 appeared in 8000 ms"
//
// and the sentence the user would have read was: *"the rack never acknowledged it within 8 s - it may be gone,
// or its track may be disabled"*. The rack was neither. It was mid-rebuild from the row before, it never said
// so (LinkProcessor::pollChainCommand returned in silence while chainBuilding), and it applied that very
// command seconds later - after its outcome had been reported as a failure.
//
// TWO THINGS ARE WRONG THERE AND THIS GUARD IS ABOUT THE SECOND ONE. The Link's silence is fixed on the Link's
// side and asserted by chain_busy_ack_guard. This is the main plugin's half: what it does with a "busy" answer,
// and what it says when its window really does expire. Both are decisions about WORDS AND WAITING, which is
// why they can be asserted in one process with a fabricated registry row - the registry is this code's only
// input about a Link, and these legs give it every shape it can have.
//
// THE LEGS
//   (1) the sender DECLARES that it understands a busy ack (wantBusyAck in the command file). Without that
//       flag a Link from today would answer "busy" to a main plugin from yesterday, which reads any status
//       that is not "ok" as a refusal - so the flag is what makes the whole change additive;
//   (2) a live, idle rack that says nothing: the sentence states the fact and NOT the two guesses. The two
//       forbidden phrases are asserted absent by name, because this is a wording fix and a wording fix with no
//       test is a comment;
//   (3) ...no row at all: "no longer listed in this session" - which is a fact the registry really has;
//   (4) ...a row that says it is BUSY: the expiry says it is rebuilding, still without speculation;
//   (5) THE FIX ITSELF: a busy ack does not end the wait. It is not a verdict, it is "I have your command",
//       and the real outcome that overwrites it is what the caller is finally told (here: ok, 3 slots);
//   (6) THE EXTENSION IS WITHDRAWN when the rack stops answering, so a Link that dies mid-build does not hold
//       the window open to its cap;
//   (7) ...and the extension is BOUNDED: a rack that stays busy for ever is given up on at the cap, with the
//       honest sentence that the edit is queued on that rack rather than refused.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EJAgentExecutorRead.h"
#include "LinkShm.h"
#include <cstdio>
#include <memory>

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }

juce::String shmDir() { int e = 0; return LinkShm::resolveDir (e); }

// THE REGISTRY, WRITTEN BY HAND. This guard plays the part a Link's process plays: it claims a row, climbs a
// heartbeat and raises or lowers the busy byte. Those three bytes are the whole of what the main plugin can
// know about a Link, so a fabricated row is not a stand-in for the input - it IS the input.
struct FakeLink
{
    void* map = nullptr; int fd = -1, idx = -1; juce::String uid;
    bool claim (const juce::String& u)
    {
        int e = 0;
        map = LinkShm::openRegistry (shmDir(), fd, e);
        if (map == nullptr) return false;
        uid = u;
        idx = LinkShm::claimSlot (map, "Guard Kick", "audio_guard.bin", u, 48000.0f, 2u);
        return idx >= 0;
    }
    void beat()            { if (map != nullptr && idx >= 0) LinkShm::bumpHeartbeat (map, idx); }
    void busy (bool b)     { if (map != nullptr && idx >= 0) LinkShm::setSlotBusy  (map, idx, b); }
    void vanish()          { if (map != nullptr && idx >= 0) LinkShm::reapSlot (map, idx); }
    ~FakeLink()            { if (map != nullptr) LinkShm::closeRegistry (map, fd); }
};

// Pump the message thread for real time, climbing the fake Link's heartbeat and refreshing the main plugin's
// view of the registry as its own 1 Hz timer would. `alive` false = the Link has stopped answering.
void pump (EchoJayProcessor& p, FakeLink& link, double ms, bool alive = true)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    double lastBeat = 0.0;
    while (juce::Time::getMillisecondCounterHiRes() - t0 < ms)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (alive && now - lastBeat > 400.0) { lastBeat = now; link.beat(); p.refreshLinkRegistry(); }
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.004, false);
    }
}

void writeAck (const juce::String& uid, int seq, const juce::String& status,
               const juce::String& reason, int slotsAfter)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("v", 1); o->setProperty ("seq", seq); o->setProperty ("status", status);
    if (reason.isNotEmpty())  o->setProperty ("reason", reason);
    if (slotsAfter >= 0)      o->setProperty ("slotsAfter", slotsAfter);
    juce::File (shmDir() + "chain-ack-" + uid + ".json")
        .replaceWithText (juce::JSON::toString (juce::var (o), true));
}

struct Outcome
{
    bool fired = false, ok = false; juce::String sentence, technical; int slots = -1; double elapsedS = 0.0;
};
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = shmDir();
    std::printf ("chain_ack_wait_guard: what the main plugin does with a busy ack, and what it says when the "
                 "window expires (shared dir %s)\n", dir.toRawUTF8());

    EchoJayProcessor bp; bp.prepareToPlay (48000.0, 512);

    FakeLink link;
    if (! link.claim ("bz1"))
    { std::printf ("HARNESS PRECONDITION FAILED: no registry row could be claimed in %s\n", dir.toRawUTF8()); return 2; }
    // Two observations with a climbing heartbeat is what makes a row LISTED (the 25 Aug liveness rule), so the
    // fixture pumps before it asserts anything.
    pump (bp, link, 1400);
    {
        const auto a = bp.linkAnswerability ("bz1");
        std::printf ("    fixture: row found=%d busy=%d answering=%d heartbeat %.1f s ago\n",
                     (int) a.found, (int) a.busy, (int) a.answering, a.ageS);
        if (! a.found)
        { std::printf ("HARNESS PRECONDITION FAILED: the fabricated row never became visible to the plugin\n"); return 2; }
    }

    auto await = [&bp] (const juce::String& uid, int seq, int maxWaitMs, std::shared_ptr<Outcome> out)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        bp.awaitChainAck (uid, seq, [out, t0] (bool ok, juce::String s, juce::String t, int slots)
        {
            out->fired = true; out->ok = ok; out->sentence = s; out->technical = t; out->slots = slots;
            out->elapsedS = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        }, maxWaitMs);
    };

    // ---- (1) THE SENDER DECLARES IT UNDERSTANDS A BUSY ACK ------------------------------------------------
    std::printf ("== (1) the command file declares that this sender understands \"busy\" ==\n");
    {
        juce::Array<juce::var> ops;
        auto* op = new juce::DynamicObject(); op->setProperty ("op", "bypass"); op->setProperty ("slot", 1);
        op->setProperty ("on", true); ops.add (juce::var (op));
        juce::File (dir + "chain-cmd-bz1.json").deleteFile();
        const int seq = bp.writeChainEditCommand ("bz1", juce::var (ops), juce::var (juce::Array<juce::var>()),
                                                  "guard", {});
        const auto body = juce::File (dir + "chain-cmd-bz1.json").loadFileAsString();
        check (seq > 0 && (bool) juce::JSON::parse (body).getProperty ("wantBusyAck", false),
               "(1) the chain command says wantBusyAck - without it a Link from today must stay silent for a "
               "main plugin from yesterday, which would read \"busy\" as a refusal",
               "seq " + juce::String (seq) + ", " + juce::String (body.length()) + " bytes");
        juce::File (dir + "chain-cmd-bz1.json").deleteFile();
    }

    const juce::String forbidden1 = "may be gone";
    const juce::String forbidden2 = "may be disabled";

    // ---- (2) A LIVE, IDLE RACK THAT SAYS NOTHING ---------------------------------------------------------
    std::printf ("== (2) a live rack that never answers: the fact, not the two guesses ==\n");
    {
        juce::File (dir + "chain-ack-bz1.json").deleteFile();
        link.busy (false);
        auto out = std::make_shared<Outcome>();
        await ("bz1", 4001, 800, out);
        pump (bp, link, 2500);
        std::printf ("    sentence: %s\n", out->sentence.toRawUTF8());
        std::printf ("    technical: %s\n", out->technical.toRawUTF8());
        check (out->fired && ! out->ok, "(2) the wait ends and reports failure", "after " + juce::String (out->elapsedS, 1) + " s");
        check (! out->sentence.contains (forbidden1) && ! out->sentence.contains (forbidden2),
               "(2) THE SENTENCE NO LONGER GUESSES: neither \"" + forbidden1 + "\" nor \"" + forbidden2
                   + "\" appears. On the night this was found both were false about a Link that answered the "
                     "rows either side of it",
               out->sentence);
        check (out->sentence.contains ("did not answer") && out->sentence.contains ("edit was not"),
               "(2) ...and it still says the two things the caller needs: no answer, and the edit was not made");
        check (out->technical.contains ("registry:") && out->technical.contains ("heartbeat"),
               "(2) the technical line carries the registry reading the sentence was built from");
    }

    // ---- (3) NO ROW AT ALL -------------------------------------------------------------------------------
    std::printf ("== (3) a uid the registry has never heard of ==\n");
    {
        juce::File (dir + "chain-ack-zz9.json").deleteFile();
        auto out = std::make_shared<Outcome>();
        await ("zz9", 4002, 600, out);
        pump (bp, link, 2000);
        std::printf ("    sentence: %s\n", out->sentence.toRawUTF8());
        check (out->fired && ! out->ok && out->sentence.contains ("no longer listed"),
               "(3) an absent row is reported as absent - which is a fact the registry has, unlike \"it may be gone\"",
               out->sentence);
    }

    // ---- (4) A ROW THAT SAYS IT IS BUSY, WITH NO ACK -----------------------------------------------------
    std::printf ("== (4) the rack's registry row says it is rebuilding, and no ack arrives ==\n");
    {
        juce::File (dir + "chain-ack-bz1.json").deleteFile();
        link.busy (true);
        auto out = std::make_shared<Outcome>();
        await ("bz1", 4003, 800, out);
        pump (bp, link, 2500);
        std::printf ("    sentence: %s\n", out->sentence.toRawUTF8());
        check (out->fired && ! out->ok && out->sentence.contains ("rebuilding")
                 && ! out->sentence.contains (forbidden1),
               "(4) a busy row is reported as rebuilding, not as possibly gone",
               out->sentence);
        link.busy (false);
    }

    // ---- (5) THE FIX: A BUSY ACK IS NOT A VERDICT --------------------------------------------------------
    std::printf ("== (5) a busy ack extends the wait, and the REAL outcome is what the caller is told ==\n");
    {
        const int seq = 4004;
        juce::File (dir + "chain-ack-bz1.json").deleteFile();
        link.busy (true);
        auto out = std::make_shared<Outcome>();
        await ("bz1", seq, 800, out);          // an 800 ms window, deliberately short
        writeAck ("bz1", seq, "busy", "a rebuild of this rack has been running for 1.2 s; the command is queued, "
                                      "not refused", -1);
        pump (bp, link, 2600);                 // more than three times the window
        check (! out->fired,
               "(5) THE HEART OF IT: the wait is STILL OPEN at 2.6 s against an 800 ms window, because the rack "
               "said it has the command and the registry agrees it is building. The old code reported a failure "
               "at the window and the rack applied the edit afterwards",
               out->fired ? ("fired after " + juce::String (out->elapsedS, 1) + " s: " + out->sentence)
                          : juce::String ("still waiting"));
        // ...and now the rack finishes and overwrites its own busy ack with the truth.
        link.busy (false);
        writeAck ("bz1", seq, "ok", {}, 3);
        pump (bp, link, 1200);
        check (out->fired && out->ok && out->slots == 3,
               "(5) ...and what the caller is finally told is the rack's real outcome",
               out->sentence + " / slots " + juce::String (out->slots));
    }

    // ---- (6) THE EXTENSION IS WITHDRAWN WHEN THE RACK STOPS ANSWERING ------------------------------------
    std::printf ("== (6) a rack that says busy and then dies does not hold the window to its cap ==\n");
    {
        const int seq = 4005;
        juce::File (dir + "chain-ack-bz1.json").deleteFile();
        link.busy (true);
        auto out = std::make_shared<Outcome>();
        await ("bz1", seq, 800, out);
        writeAck ("bz1", seq, "busy", "building", -1);
        pump (bp, link, 1000);
        check (! out->fired, "(6) fixture: the wait is open while the rack is busy");
        // The rack stops: busy down, heartbeat frozen. Past kLinkAnsweringWithinS it is not answering, and the
        // extension that its own ack bought is withdrawn.
        link.busy (false);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 200 && ! out->fired; ++i) pump (bp, link, 100, false);
        const double gaveUpS = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        std::printf ("    sentence: %s\n", out->sentence.toRawUTF8());
        check (out->fired && ! out->ok && gaveUpS < 20.0,
               "(6) the wait ends within a heartbeat or two of the rack going quiet - well inside the 30 s cap",
               juce::String (gaveUpS, 1) + " s after it went quiet");
        check (out->sentence.contains ("stopped answering") && ! out->sentence.contains (forbidden2),
               "(6) ...and it says what is actually known: the rack took the edit in hand and then stopped "
               "answering, so whether it was made is not known here",
               out->sentence);
    }

    // ---- (7) AND IT IS BOUNDED --------------------------------------------------------------------------
    std::printf ("== (7) a rack that stays busy for ever is given up on at the cap (this leg takes ~30 s) ==\n");
    {
        const int seq = 4006;
        juce::File (dir + "chain-ack-bz1.json").deleteFile();
        link.busy (true);
        auto out = std::make_shared<Outcome>();
        await ("bz1", seq, 800, out);
        writeAck ("bz1", seq, "busy", "building", -1);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 500 && ! out->fired; ++i) pump (bp, link, 100);
        const double tookS = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        std::printf ("    sentence: %s\n", out->sentence.toRawUTF8());
        std::printf ("    technical: %s\n", out->technical.toRawUTF8());
        check (out->fired && ! out->ok && tookS > 20.0 && tookS < 45.0,
               "(7) the extension has a cap (kChainAckBusyHardMaxMs, 30 s): a rack that never finishes is not "
               "waited on for ever",
               juce::String (tookS, 1) + " s");
        check (out->sentence.contains ("queued") && out->sentence.contains ("not refused"),
               "(7) ...and the sentence at the cap is the actionable truth: the edit is queued on that rack, "
               "not refused",
               out->sentence);
        link.busy (false);
    }

    // ---- (8) ITEM 13: WHAT THE AGENT IS TOLD ABOUT REACHABILITY ----------------------------------------
    //
    // The same registry, read by the other consumer. `Sources::tracks` reported `fresh` from heartbeatFresh,
    // which is `staleCycles < 6` - a count of CONSUMER POLLS with no advance - and refreshLinkRegistry runs
    // from the 1 Hz timer AND from several editor sites. An agent round IS a burst of refreshes, so the count
    // ran up against a Link that was alive and heartbeating normally, and the agent's card said: "88_7even
    // Kick_2_01 isn't reachable from this session right now (it shows as gone)". The field's own comment in
    // PluginProcessor.h has said since 4 October to use the wall clock for exactly this decision.
    //
    // THE BURST IS THE LEG. Eight refreshes with no heartbeat advance, while the last advance is well inside
    // kLinkAnsweringWithinS: the old answer is "gone", the wall clock's answer is "there".
    std::printf ("== (8) a burst of registry refreshes must not make a live Link read as gone ==\n");
    {
        link.busy (false);
        link.beat(); bp.refreshLinkRegistry();      // one real advance, now
        for (int i = 0; i < 8; ++i) bp.refreshLinkRegistry();   // ...then a burst with nothing new to see
        const auto a = bp.linkAnswerability ("bz1");
        auto tracks = echojay::agent::bindToProcessor (bp).tracks();
        const echojay::agent::TrackRead* row = nullptr;
        for (const auto& t : tracks) if (t.uid == "bz1") row = &t;
        const auto v = echojay::agent::ExecutorRead::tracksVar (tracks);
        juce::String json = juce::JSON::toString (v, true);
        std::printf ("    answerability: found=%d busy=%d answering=%d heartbeat %.2f s ago\n",
                     (int) a.found, (int) a.busy, (int) a.answering, a.ageS);
        std::printf ("    tracks: %s\n", json.toRawUTF8());
        check (row != nullptr && row->fresh && a.ageS >= 0.0f && a.ageS <= EchoJayProcessor::kLinkAnsweringWithinS,
               "(8) eight refreshes in a row do not age a Link that beat a moment ago: liveness is read in TIME, "
               "not in polls (old code: staleCycles hit 6 and the agent was told the track was gone)",
               row != nullptr ? ("fresh=" + juce::String ((int) row->fresh) + ", heartbeat "
                                 + juce::String (a.ageS, 2) + " s ago")
                              : juce::String ("no row for bz1 in the bound tracks"));
        check (! json.contains ("gone"),
               "(8) ...so nothing in what the agent reads says that track is gone", json);
    }

    // ---- (9) A REBUILDING RACK IS BUSY, NOT GONE -------------------------------------------------------
    std::printf ("== (9) a rack that is rebuilding reads as BUSY to the agent, never as gone ==\n");
    {
        link.busy (true);
        link.beat(); bp.refreshLinkRegistry(); link.beat(); bp.refreshLinkRegistry();
        auto tracks = echojay::agent::bindToProcessor (bp).tracks();
        const auto json = juce::JSON::toString (echojay::agent::ExecutorRead::tracksVar (tracks), true);
        std::printf ("    tracks: %s\n", json.toRawUTF8());
        check (json.contains ("busy") && ! json.contains ("gone"),
               "(9) the rack's own word for \"I am rebuilding\" reaches the agent as busy, and busy excludes gone - "
               "a user watching a chain being built is never told the track is missing",
               json);
        link.busy (false);
    }

    link.vanish();
    std::printf ("\n==== chain_ack_wait_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
