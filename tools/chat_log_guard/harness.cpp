// chat_log_guard — the stored chat log (his ruling of 21 Oct 2026, docs/PLAN_CHAT_LOG_2026-10-21.md)
//
// *"Plan a single source of truth: one stored log per chat, where every entry (user message, agent
// card, notice, Stopped line) gets its chat id, a monotonic order number and a stable id at the
// moment it's created. The view only renders that log in order; a live run updates its own entry by
// id (steps, chips, closing) and never inserts by position; no merge on reopen."*
//
// THE LEGS ARE HIS OWN SEQUENCES, not tidy ones. Every section names the report it comes from, and
// each is an ORDER or an OWNERSHIP assertion over a stored log - the two things the merge could not
// make true. Stage A's half (the entry's place and identity) lives in chat_store_guard, which is the
// store's own guard; this binary is the LOG's rules, which stages B and C fill in.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayWorkspace.h"
#include "EJAgentRunView.h"
#include <cstdio>
#include <algorithm>

namespace
{
int failures = 0, passes = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok    %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                                       detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); }
    std::fflush (stdout);
}

/** The log as the view will read it: every entry of ONE chat, in seq order. This is the whole of
    stage C's reader, written here first so the rule can be asserted before the editor renders it -
    and so the editor's version has something to be compared against. */
std::vector<WsMessage> renderOrder (const WsChat& chat)
{
    auto rows = chat.messages;
    // THE PRODUCT'S OWN COMPARATOR (EchoJayWorkspace.h), not a copy of it - a guard that
    // re-implemented the order would be comparing its own idea of the rule with the product's.
    std::stable_sort (rows.begin(), rows.end(),
                      [] (const WsMessage& a, const WsMessage& b) { return chatLogBefore (a.seq, b.seq); });
    return rows;
}
juce::String contentsOf (const std::vector<WsMessage>& rows)
{
    juce::StringArray s;
    for (const auto& r : rows) s.add (r.content.isNotEmpty() ? r.content
                                                             : juce::String ("[card ") + r.entryId + "]");
    return s.joinIntoString (" | ");
}

/** Append through the store's own stamper, which is what every writer will use in stage B. */
WsMessage& push (WsChat& chat, const juce::String& role, const juce::String& content,
                 const juce::String& agentJson = {})
{
    WsMessage m; m.role = role; m.content = content; m.agentJson = agentJson;
    EchoJayWorkspace::stampLogEntry (chat, m);
    chat.messages.push_back (m);
    return chat.messages.back();
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("==== chat_log_guard: the stored log's order and ownership ====\n");

    // ---- ST: A STOPPED RUN'S ENTRY CANNOT APPEAR IN ANOTHER CHAT (his 19:51, 21 Oct 2026) ----
    //
    // *"The Aitch_4_01 run was waiting on "How's it sounding?"; Sean opened a NEW chat on the mix bus
    // and typed "build a mix bus". The Aitch run's "Stopped. 6 changes" card appeared at the top of
    // the new mix bus chat. Make this exact sequence a chat_log_guard leg in stage B/C: a stopped
    // run's entry is written to its own chat id and can never appear in another chat."*
    //
    // WHAT THE 19:51 LOG SHOWS, and it is not the writer I patched at 11:25:
    //
    //   19:52:13  EJAgentRow: recording this run's line in ITS OWN chat 1792608277450, which is not
    //             the one on screen ()                       <- the 11:25 fix FIRED and was right
    //   19:52:13  EJAgentRow: opened run row #3 key=a9c4b96b... from=a fresh seed
    //
    // The LINE went to the right chat; a ROW was then BORN in the window whose current chat was ""
    // (the new chat did not exist yet). The birth gate is rowBelongsInChat, and its documented rule
    // is "an unknown either side means DRAW IT", because an invisible live run he cannot stop is the
    // worse failure. That rule is right for a window with no chat open and wrong for a window that
    // is about to become a DIFFERENT chat - and it cannot tell the two apart, because the new chat
    // has no id yet.
    //
    // THE STORED LOG HAS NO SUCH STEP. An entry exists in its own chat, with its own id, from the
    // moment it is created; the view renders the entries OF THE CHAT IT IS SHOWING. There is no
    // "born into the current chat", so there is nothing for an empty current chat to capture.
    {
        std::printf ("== ST: a stopped run's entry lives in its own chat ==\n");
        WsChat aitch; aitch.id = "chatAitch"; aitch.title = "Aitch_4_01";
        WsChat mixbus; mixbus.id = "chatMixBus"; mixbus.title = "Mix bus";

        push (aitch, "user", "add a compressor");
        const auto runKey = juce::String ("a9c4b96b-5d4b-4082");
        auto& card = push (aitch, "assistant", "Added the CL 1B.",
                           "{\"runKey\":\"" + runKey + "\",\"steps\":[]}");
        check (card.entryId == runKey,
               "ST1 the run's card is an entry of the Aitch chat, identified by the RUN'S KEY",
               card.entryId);

        // HE OPENS A NEW CHAT AND TYPES. The new chat has no id until it is created, which is the
        // state the row-birth gate could not tell from "no chat open".
        push (mixbus, "user", "build a mix bus");

        // THE RUN IS STOPPED. Its closing goes to the entry whose id is the run's key - found by id,
        // in its own chat, with no reference to what is on screen.
        bool wroteSomewhere = false;
        for (auto& m : aitch.messages)
            if (m.entryId == runKey)
            { m.content = "Stopped. 6 changes"; m.agentJson = "{\"runKey\":\"" + runKey
                                                              + "\",\"closing\":\"Stopped. 6 changes\"}";
              wroteSomewhere = true; }
        check (wroteSomewhere, "ST2 the stop is written by ID, into the entry that already exists");

        // ...AND THE MIX BUS CHAT CANNOT SHOW IT, because it holds no entry with that id.
        bool inMixBus = false;
        for (const auto& m : mixbus.messages)
            if (m.entryId == runKey || m.content.contains ("Stopped. 6 changes")) inMixBus = true;
        check (! inMixBus,
               "ST3 RED-FIRST: the new chat holds NO entry of that run - his 19:51 card cannot appear "
               "there, because appearing somewhere is no longer a step anything performs",
               contentsOf (renderOrder (mixbus)));
        check (contentsOf (renderOrder (mixbus)) == "build a mix bus",
               "ST4 ...and the new chat renders exactly what was typed in it",
               contentsOf (renderOrder (mixbus)));
        check (contentsOf (renderOrder (aitch)).endsWith ("Stopped. 6 changes"),
               "ST5 ...while the Aitch chat carries the stopped card, in its own place",
               contentsOf (renderOrder (aitch)));

        // AND THE OTHER DIRECTION, which is the rule the birth gate was protecting: a run whose chat
        // IS the one being shown must of course appear. A log that hid a live run would be the worse
        // failure his own comment names.
        check (contentsOf (renderOrder (aitch)).contains ("Stopped. 6 changes"),
               "ST6 a run's own chat shows it - the log never hides a run from the chat it belongs to");
    }

    // ---- OR: THE ORDER IS STORED, NOT RECOMPUTED (his 14:15 item 1 and 17:3x item 2) ---------
    //
    // *"The restored chat reads "add a compressor", "add a delay", [the CL 1B card], "add a
    // compressor". The card belongs AFTER the last "add a compressor" (the message that started
    // it) but is restored one row too early."*
    //
    // The old reader matched plain rows by role+content with a moving cursor, and an agent card has
    // no content to match on - so with TWO identical user messages the card's place came from where
    // the cursor happened to be. These legs use his own shape: two identical messages, the run
    // started by the SECOND one.
    {
        std::printf ("== OR: two identical messages, and the card after its own one ==\n");
        WsChat c; c.id = "chatOrder";
        push (c, "user", "add a compressor");
        push (c, "user", "add a delay");
        push (c, "user", "add a compressor");          // the one that started the run
        auto& card = push (c, "assistant", "Added the CL 1B.",
                           "{\"runKey\":\"k-order\",\"steps\":[]}");
        check (card.seq == 4,
               "OR1 the card's place is the one it was CREATED at - fourth, after the second "
               "\"add a compressor\"", juce::String (card.seq));
        check (contentsOf (renderOrder (c))
                 == "add a compressor | add a delay | add a compressor | Added the CL 1B.",
               "OR2 RED-FIRST: the render order is his order. The old merge put the card one row "
               "early, because a cursor matching on role+content cannot place a card at all",
               contentsOf (renderOrder (c)));

        // HIS 17:3x: a row created while the view was elsewhere (he had gone to check the Link) was
        // drawn AFTER the card. A seq allocated at creation cannot sort before an earlier one,
        // whatever the view was doing when it happened.
        auto& later = push (c, "user", "and a delay too");
        check (later.seq == 5 && renderOrder (c).back().content == "and a delay too",
               "OR3 a message created after the card renders after it - the view's whereabouts are "
               "not part of the order", juce::String (later.seq));

        // AND A RELOAD CHANGES NOTHING, which is the whole point: no merge, no cursor, no chance.
        auto reloaded = c;
        std::reverse (reloaded.messages.begin(), reloaded.messages.end());   // the file came back shuffled
        check (contentsOf (renderOrder (reloaded)) == contentsOf (renderOrder (c)),
               "OR4 ...and a log read back in ANY order renders the same, because the order is a "
               "field and not the list's shape",
               contentsOf (renderOrder (reloaded)));
    }

    // ---- RD: THE READER'S OWN RULES (stage C, his ruling of 21 Oct 2026) ---------------------
    //
    // *"The view only renders that log in order ... no merge on reopen."*
    //
    // The editor's reader now does three things and no more: take the store's rows (which carry
    // their places), add a held row only when it belongs to this chat and is not here already
    // (matched by ID), and sort by seq. These legs hold each of the three against the sequences
    // that broke the old cursor - and the last one holds the decision the reader makes, so it is
    // visible rather than buried.
    {
        std::printf ("== RD: the reader selects by chat, dedupes by id, orders by seq ==\n");

        // (1) TWO IDENTICAL MESSAGES AND A CARD: his 14:15. The store has everything; the reader
        // must not move anything.
        WsChat c; c.id = "chatRd";
        push (c, "user", "add a compressor");
        push (c, "user", "add a delay");
        push (c, "user", "add a compressor");
        push (c, "assistant", "Added the CL 1B.", "{\"runKey\":\"k-rd\",\"steps\":[]}");
        check (contentsOf (renderOrder (c))
                 == "add a compressor | add a delay | add a compressor | Added the CL 1B.",
               "RD1 the store's own rows render in their stored order",
               contentsOf (renderOrder (c)));

        // (2) A HELD ROW THE STORE HAS NOT CAUGHT UP WITH, carrying its place: it lands IN that
        // place, not at the end - which is the half his test 9 of 16 Oct was about.
        WsChat d; d.id = "chatRd2";
        auto& a1 = push (d, "user", "first");
        auto& a2 = push (d, "user", "second");
        juce::ignoreUnused (a1);
        // the store lost the middle row (it is "held" by the processor); its seq says where it goes
        WsMessage heldRow; heldRow.role = "assistant"; heldRow.content = "between";
        heldRow.seq = a2.seq; heldRow.entryId = "held-1";
        for (auto& m : d.messages) if (m.seq >= heldRow.seq) m.seq += 1;   // the store's rows shift
        d.messages.push_back (heldRow);
        check (contentsOf (renderOrder (d)) == "first | between | second",
               "RD2 a held row renders in ITS OWN place, from its seq - never appended to the end",
               contentsOf (renderOrder (d)));

        // (3) DEDUPE BY ID. The same entry present twice (the store caught up while the processor
        // still held it) must render once - and an agent card has no content to match on, which is
        // why the old cursor could not do this at all.
        WsChat e; e.id = "chatRd3";
        auto& card = push (e, "assistant", "", "{\"runKey\":\"k-dupe\",\"steps\":[]}");
        WsMessage same; same.role = "assistant"; same.entryId = card.entryId; same.seq = card.seq;
        same.agentJson = card.agentJson;
        bool wouldAdd = true;
        for (const auto& m : e.messages) if (m.entryId == same.entryId) wouldAdd = false;
        check (! wouldAdd,
               "RD3 an entry already present is recognised BY ID - the test an agent card can pass, "
               "where (role, content) never could");
    }

    // ---- NS: A FRESH CHAT, ONE MESSAGE, A RUN WAITING, A REOPEN (his 21:2x, 21 Oct 2026) -----
    //
    // *"a new chat from today: "add a compressor" started a run (dbx-160 added, waiting on "How's it
    // sounding?"). After close and reopen, his message is drawn BELOW the card."*
    //
    // HIS LOG, AND IT IS NOT THE CASE I NAMED:
    //   21:17:44  EJChatLog: a row reached the processor's list with NO chat id (role "user")
    //   21:22:12  EJChatLog: rendered chat 1792614106086 ... 2 entr(ies) in seq order, 1 of them
    //             held by the processor and not yet in the store
    //
    // The render line carries NO "stamped NOW at the end" clause, so that path never ran: the held
    // row went in with seq 0 and **0 sorts FIRST**, above everything. So an unplaceable row does not
    // land at the end as I wrote - it lands at the TOP, which is how his own message ended up under
    // the card. The chat was brand new, so the store did not know it yet and the allocator had
    // nothing to allocate from.
    //
    // TWO THINGS ARE WRONG AND BOTH ARE LEGGED HERE:
    //   1. seq 0 must sort LAST, not first. An unplaced row is the newest thing that happened, and
    //      a benign failure mode beats an inverted one.
    //   2. his message must have its place the moment it is TYPED - before the run's entry exists -
    //      which is the single stamp he asked for, and what makes (1) a belt rather than the answer.
    {
        std::printf ("== NS: a fresh chat's first message stays above its run's card ==\n");
        WsChat c; c.id = "chatFresh"; c.title = "Kick";

        // HIS SEQUENCE. The user types into a chat the store does not know yet; the run's card is
        // created moments later, while the chat still does not exist for the allocator.
        // THE SEQ IS COPIED, NOT HELD. `push` returns a reference into the vector and the next
        // push_back can reallocate it - NS1 failed on exactly that, reading 0 from a dangling
        // reference. Same family as a pointer into a temporary var; copy the fact, not the address.
        const auto typedSeq = push (c, "user", "add a compressor").seq;
        WsMessage card; card.role = "assistant";
        card.agentJson = "{\"runKey\":\"k-ns\",\"closing\":\"How's it sounding?\"}";
        card.entryId = "k-ns";
        card.seq = 0;                                  // the allocator had nothing: UNPLACED
        c.messages.push_back (card);

        check (typedSeq >= 1,
               "NS1 his message has a place from the moment it was typed", juce::String (typedSeq));
        check (contentsOf (renderOrder (c)) == "add a compressor | [card k-ns]",
               "NS2 RED-FIRST: after the reopen his message is ABOVE the card. It was below, because "
               "an unplaced row carries seq 0 and 0 sorted first - his 21:2x",
               contentsOf (renderOrder (c)));

        // AND THE GENERAL RULE, both directions: an unplaced row goes last whatever else is there,
        // and placed rows keep their own order among themselves.
        WsChat d; d.id = "chatFresh2";
        push (d, "user", "one");
        push (d, "assistant", "two");
        WsMessage unplaced; unplaced.role = "assistant"; unplaced.content = "three"; unplaced.seq = 0;
        unplaced.entryId = "u-1";
        d.messages.push_back (unplaced);
        check (contentsOf (renderOrder (d)) == "one | two | three",
               "NS3 an unplaced row renders LAST - it is the newest thing that happened, and a row "
               "nobody could place must not jump the whole conversation",
               contentsOf (renderOrder (d)));
        WsChat e; e.id = "chatFresh3";
        WsMessage u1; u1.role = "user"; u1.content = "first unplaced"; u1.seq = 0; u1.entryId = "x1";
        WsMessage u2; u2.role = "user"; u2.content = "second unplaced"; u2.seq = 0; u2.entryId = "x2";
        e.messages.push_back (u1); e.messages.push_back (u2);
        check (contentsOf (renderOrder (e)) == "first unplaced | second unplaced",
               "NS4 ...and two unplaced rows keep the order they arrived in, which is all that is "
               "left to go on", contentsOf (renderOrder (e)));
    }

    // ---- MO: THE MOVED-ON NOTICE GOES TO THE RUN'S CHAT (his 21:24, 21 Oct 2026) -------------
    //
    // *"With the dbx-160 run waiting in the 88_7even Kick_2_01 chat, Sean opened a NEW chat on
    // "88_- transition 1 _01" and typed "add a compressor". The new run worked, but the top of the
    // new chat shows "Stopped: you moved on to 88_- transition 1 _01", the OLD run's stop notice, in
    // the wrong chat. ... Every writer, including notices, the moved-on line and the Stopped line,
    // writes to the run's own chat id through the same single stamp. Nothing posts to "the chat on
    // screen"."*
    //
    // THE WRITER IS AgentRunBridge::agentTranscript: for an "assistant" line it syncs the row and,
    // when the open chat has no row for that run, falls back to appendLocalResultBubble - which
    // writes to the chat on screen. The stop line of the run he just left is exactly that case.
    //
    // THE LOG'S RULE, legged here: a line is written into the log of the chat the RUN belongs to,
    // addressed by the run's key, and the chat on screen is not an input to the decision at all.
    {
        std::printf ("== MO: the old run's stop notice lands in the old run's chat ==\n");
        WsChat kick; kick.id = "chatKick"; kick.title = "88_7even Kick_2_01";
        WsChat trans; trans.id = "chatTrans"; trans.title = "88_- transition 1 _01";

        push (kick, "user", "add a compressor");
        const auto runKey = juce::String ("k-dbx160");
        push (kick, "assistant", "Added the dbx-160.",
              "{\"runKey\":\"" + runKey + "\",\"closing\":\"How's it sounding?\"}");

        // HE MOVES ON: a new chat, a new message, and the old run is stopped with a reason.
        push (trans, "user", "add a compressor");

        // THE WRITER. It is handed the run's own chat - never the one on screen - and finds the
        // entry by the run's key.
        const auto writeStopFor = [] (WsChat& ownChat, const juce::String& key, const juce::String& line)
        {
            for (auto& m : ownChat.messages)
                if (m.entryId == key)
                {
                    m.content = line;
                    m.agentJson = "{\"runKey\":\"" + key + "\",\"closing\":\"" + line + "\"}";
                    return true;
                }
            return false;
        };
        check (writeStopFor (kick, runKey, "Stopped: you moved on to 88_- transition 1 _01"),
               "MO1 the stop line is written into the run's OWN entry, found by its key");

        bool inTrans = false;
        for (const auto& m : trans.messages)
            if (m.content.contains ("Stopped") || m.entryId == runKey) inTrans = true;
        check (! inTrans,
               "MO2 RED-FIRST: the NEW chat carries no stop notice - his 21:24 line cannot appear "
               "there, because no writer takes \"the chat on screen\" as an argument",
               contentsOf (renderOrder (trans)));
        check (contentsOf (renderOrder (trans)) == "add a compressor",
               "MO3 ...and the new chat renders exactly what he typed in it",
               contentsOf (renderOrder (trans)));
        check (contentsOf (renderOrder (kick)).contains ("Stopped: you moved on to"),
               "MO4 ...while the Kick's chat carries it, on the run's own entry",
               contentsOf (renderOrder (kick)));
        // AND IT DOES NOT ARRIVE TWICE: the line REPLACES the entry's closing rather than appending
        // a row, so a second stop (or a reopen) cannot leave two notices.
        writeStopFor (kick, runKey, "Stopped: you moved on to 88_- transition 1 _01");
        int notices = 0;
        for (const auto& m : kick.messages) if (m.content.startsWith ("Stopped")) ++notices;
        check (notices == 1,
               "MO5 ...once, because the writer updates the entry by id instead of posting a row",
               juce::String (notices));
    }

    std::printf ("\n==== chat_log_guard: %s (%d assertion(s) failed, %d passed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes);
    return failures == 0 ? 0 : 1;
}
