#pragma once
// =============================================================================
//  EJRigHook.h - THE HOST RIG'S AUTOMATION CHANNEL (Sean's ask A, 13 Oct 2026)
// =============================================================================
//
//  WHY THIS EXISTS. The gate cannot open an editor. Every window-lifecycle bug
//  therefore reaches Sean untested, and "closing mid-reply" has now failed three
//  rounds running - each time fixed against a reading of the code rather than a
//  run of it. The rig loads the real built bundles in a real host process, plays
//  audio through them, opens and closes editors on a script, and asserts on what
//  the plugin actually holds afterwards. This header is the only part of that
//  which lives in the shipped plugin: the door the rig knocks on.
//
//  THE TWO LOCKS. It is inert unless BOTH are open:
//
//    1. ECHOJAY_STATE_HOME is set (echojay::stateIsIsolated()). The rig may never
//       touch ~/Library/EchoJay - Sean's ruling of 6 Sep, and the reason every
//       harness in this tree runs in a private root.
//    2. EJ_RIG_HOOK=1 is in the environment.
//
//  With either absent, enabled() is false, nothing is polled, nothing is written,
//  and no file is created. A normal run of the plugin in Logic cannot open this
//  door: Logic sets neither variable, and a plugin cannot set them for itself.
//  The isolation check is first and is not optional - a hook that could drive the
//  UI against live state would be a worse bug than any it was built to catch.
//
//  THE CHANNEL. The same shape as every other command channel in this codebase
//  (chain-cmd / chain-ack), for the same reason: it is file-based, so it crosses
//  the process boundary between the rig's host and the plugin's bundle without
//  an ABI, it survives an editor being destroyed mid-command, and it can be read
//  after the fact when something goes wrong.
//
//      <state home>/rig/rig-cmd.json     the rig writes, the plugin consumes
//      <state home>/rig/rig-ack.json     the plugin writes when it has acted
//      <state home>/rig/rig-state.json   the plugin's state, on demand
//
//  A command carries a monotonic `seq`; the plugin ignores a seq it has already
//  run, so a command file left on disk is not re-executed on the next editor.
//
//  WHAT IT MAY DO. Only what a user can do with the mouse and keyboard: type a
//  message and send it, toggle the Agent chip, press the buttons on the agent
//  card, choose a working Link. It exposes no path a user does not have, so a
//  rig pass cannot prove something the product cannot do.
// =============================================================================

#include <juce_core/juce_core.h>
#include "EJStateRoot.h"

namespace echojay::rig
{

/** Both locks. Read live rather than cached: a cached answer taken at static-init
    time would be read before the host had set up the environment on some hosts. */
inline bool enabled() noexcept
{
    if (! echojay::stateIsIsolated()) return false;          // never against live state
    const char* e = ::getenv ("EJ_RIG_HOOK");
    return e != nullptr && juce::String (e).trim() == "1";
}

/** <state home>/rig, created on first use. Only ever called when enabled(). */
inline juce::File dir()
{
    auto d = echojay::userStateHome().getChildFile ("rig");
    d.createDirectory();
    return d;
}

inline juce::File cmdFile()   { return dir().getChildFile ("rig-cmd.json"); }
inline juce::File ackFile()   { return dir().getChildFile ("rig-ack.json"); }
inline juce::File stateFile() { return dir().getChildFile ("rig-state.json"); }

/** The command names the rig may send. Kept as one list in one place so the rig
    and the plugin cannot drift: tools/host_rig/host_rig.mm sends these strings. */
/** `ask_chip3` -> 3, `ask_chip` -> 0, anything else -> -1. PURE, so the guard holds the vocabulary
    without an editor: the whole fault this closes was a name routed to the wrong surface, and a name
    is exactly the kind of thing a guard can hold for ever. `chip0` deliberately returns -1 here - it
    is the agent run row's, and that is the point. */
inline int askChipPressIndex (const juce::String& button)
{
    const auto b = button.trim().toLowerCase();
    if (! b.startsWith ("ask_chip")) return -1;
    const auto tail = b.substring (8);
    if (tail.isEmpty()) return 0;
    if (! tail.containsOnly ("0123456789")) return -1;
    return tail.getIntValue();
}

namespace cmd
{
    inline constexpr const char* sendChat     = "send_chat";       // { text }
    // { on: bool } still works and means Off/Ask; { mode: "off"|"ask"|"auto" } is the three-way chip
    // (Sean's Auto ruling, 14 Oct). Both spellings, because every existing row sends `on` and a rig that
    // had to be migrated in lockstep with the plugin is a rig that stops being run.
    inline constexpr const char* agentChip    = "agent_chip";      // { on: bool } | { mode: "off|ask|auto" }
    // ITEM 0 (15 Oct): with the run rendered as a transcript MESSAGE, these presses land on the
    // message's own buttons (EchoJayEditor::rigPressAgentRow), falling back to the old card only when
    // the row has no such control. `apply` keeps its old meaning - apply the plan - because twenty rows
    // already send it; `apply_line` is the LINE's own Apply, which is item 2's actual fix, and
    // `undo_step`, `undo_all` and `changes` are the row's other controls. `undo` is unchanged and still
    // means the CHAIN BAR's Undo (ship 1, fix 4), not a step's.
    // `chat_retry` (16 Oct 2026) is the CHAT row's own pill - the interrupted row's Retry, Sean's #7 -
    // and it is deliberately NOT spelled `retry`, which in this vocabulary is the agent RUN row's retry.
    // Two controls on two surfaces with one name is how a row ends up proving the wrong button.
    // `tab_chat` / `tab_chain` press the TAB BUTTONS (16 Oct 2026). Row H1's subject is the mode hint,
    // which is gated on the composer being on screen - *"CHAT tab, composer visible"*, Sean's own words -
    // so a row about it has to be able to get there the way he does.
    // `ask_chip<N>` (S6 B6, 25 Oct 2026) presses the ASK SHELF's pills - the supersede ask's "Start
    // anyway", a model brief's options, the classify chips. `chip<N>` keeps its ONE meaning, the
    // agent run row's chip. The B6 row sent chip0 at the supersede ask, it was routed to the run row,
    // found no zone, and the plugin answered "unknown button" - which was correct, and the row then
    // measured an interaction it had never performed. Two surfaces, two names, and `askChipsUp` on
    // the state so a row can WAIT for the chips instead of pressing blind.
    inline constexpr const char* press        = "press";           // { button: apply|apply_line|apply_all|skip_all|skip_line|details|stop|undo|undo_step|undo_all|changes|chip|ask_chip|got_it|retry|chat_retry|tab_chat|tab_chain|tab_match|real_editor_on|real_editor_off }
    inline constexpr const char* selectLink   = "select_link";     // { uid }  ("" = the own rack)
    inline constexpr const char* reportState  = "report_state";    // {}
    inline constexpr const char* setRackOut   = "set_rack_out";    // { db }   - the user's own knob, not the agent op
    // Sean's ruling 3 (13 Oct 2026), so the rig covers the three callbacks the sweep found one-ended:
    inline constexpr const char* openSlot     = "open_slot";       // { n }    - a slot DOUBLE-CLICK (1-based)
    inline constexpr const char* preGainReset = "pre_gain_reset";  // {}       - the pre-gain knob's double-click
    // ---- THE MATCH PAGE, WHICH NO ROW COULD REACH (his item 2a, 30 Oct 2026) ----------------
    //
    // *"AI MATCH on the Match tab is silent"* - and there was no way to press it from here, so the
    // fault had no row and no reproduction. The page is a Reference SUB-TAB and its mix side is
    // picked from an async popup menu, which a rig cannot drive; so these three drive the same
    // state the user's own pick produces, through the product's one writer for it
    // (EchoJayEditor::matchApplyMixSlot), not a copy of it.
    inline constexpr const char* matchSlot    = "match_slot";      // { kind: live|snapshot|reference, index? }
    inline constexpr const char* aiMatch      = "ai_match";        // {}       - the AI MATCH button itself
    inline constexpr const char* matchAxis    = "match_axis";      // { axis: spectrum|loudness|dynamics|stereo }
    // THE CHAT'S working channel, which is a DIFFERENT thing from the rack view (the 25 Sep rule: "a rack
    // selection moves the VIEW and nothing else... and leaves the chat, the Working-on banner and the build
    // target exactly where they were"). Sean's item 7 is about this one - the header that said "Working on
    // Link: Aitch_4_01" - so the rig has to be able to press both doors and tell them apart.
    inline constexpr const char* workOn       = "work_on";         // { uid }  - open that channel's chat
    // A user ANSWERS a question the card asks. Without this the rig stalled on every turn that asked one
    // (the undo row sat in awaiting_ask until its bound ran out), which is the rig failing to be a user
    // rather than the agent failing to work.
    inline constexpr const char* answerAsk    = "answer_ask";      // { label } - "" = the first chip offered
    // ---- SHIP 1 (15 Oct): the doors Sean's seven fixes need proving through -----------------------------
    // setPreGain is the KNOB'S OWN gesture, not the agent's op: fix 4 is about a HAND move being undoable,
    // so a row that used the agent op would prove the wrong thing. `press: undo` is the chain bar's Undo.
    inline constexpr const char* setPreGain   = "set_pre_gain";    // { db }  - as if dragged by hand
    // ---- A SLOT'S OWN BYPASS, BY HAND (C's point 81, 29 Oct 2026) ------------------------------------
    // There was no per-SLOT gesture in this vocabulary at all, which is why the session/borrow road had
    // no row: every slot-level command here drives the live view's COPY (liveParam) or the rack as a
    // whole. A bypass is the cheapest gesture that addresses the borrowed MIRROR BY INDEX, which is the
    // thing that goes wrong when the Link's rack moves under a session - so LV9g presses this, and it
    // runs onBypassSlot, the panel's own callback, rather than a copy of it.
    inline constexpr const char* slotBypass   = "slot_bypass";     // { n }   - a slot's bypass button (1-based)
    // ---- AN EDIT TO A LINK, DOWN THE ROAD THE AGENT USES (his Test 4, 29 Oct 2026) ------------------
    // His replace was refused on BOTH Links for three weeks and no row could have found it: every
    // slot-level command here either drives the live view's copy or adds through the picker, and the
    // AGENT's edit road - editOps over chain-cmd v2, where the missing base list was - had none at all.
    // A row sends real ops down it now, so a replace, an add, a remove and a bypass are all testable
    // without a server (there is no agent in a rig run: "CAPABILITIES: skipped - no live server").
    //   { uid, ops: "[{\"op\":\"replace\",\"slot\":1,\"name\":\"EchoJay Gain\"}]" }
    inline constexpr const char* editOps      = "edit_ops";        // { uid, ops (JSON array as text) }
    // ---- EVIDENCE (16 Oct 2026, the matrix's section 6) ------------------------------------------
    // *"Evidence per scenario: a PNG of the editor's final state (chat + rack)."* The host cannot take
    // it: an AU's editor is a native view the host wraps, so a snapshot taken out there is a blank
    // rectangle (522x100 of white, which is what the first run produced). Taken from INSIDE, by the
    // editor, on itself, it is the real window.
    //
    // It is the one command here that is not a thing a finger does - and it CHANGES NOTHING, which is
    // the line that matters: it reads the component tree and writes a file the rig named. Gated by the
    // same two locks as everything else, so it cannot exist in a shipped plugin.
    inline constexpr const char* screenshot   = "screenshot";      // { path } - the editor, as it looks
    // ---- A KNOWN RACK TO START FROM (16 Oct 2026, pass 12) ---------------------------------------
    //
    // Three matrix rows failed on pass 11 for the SAME reason and none of them was a product fault:
    // the scenarios run one after another on one rack, so by row 40 the own rack held seven slots
    // left by earlier rows and "the third-party slot" / "slot 6" / "the slot the add lands in" all
    // meant something different from what the row was written against. A row whose subject depends
    // on what ran before it is not a test of the product, it is a test of the running order.
    //
    // So a scenario can ask for the rack it was written for. This is SCAFFOLDING, gated like every
    // other command here (EJ_RIG_HOOK and the build-time lock), and it goes through removeSlot - the
    // same door the user's delete uses - so a cleared rack is a rack the product itself produced.
    inline constexpr const char* clearRack   = "clear_rack";       // {} - the own rack, emptied
    // ---- A NAMED PLUGIN ON THE OWN RACK, WITHOUT THE AGENT (26 Oct 2026) ------------------------
    // The live-view rows (LV1-LV3) are about a hosted plugin's own VIEW, and they need a known
    // plugin on a known slot in two seconds, not a 120 s agent turn that depends on a model
    // answering. It goes through ChainHost::loadByRecommendedName with LoadOrigin::User - the same
    // road the picker's choice takes - so the rack it produces is one the product itself built.
    inline constexpr const char* addSlot     = "add_slot";         // { name, uid? } - uid = a Link, added over the wire (the picker's road); no uid = the own rack
    // ---- "+ NEW CHAT", THROUGH THE PRODUCT'S OWN DOOR (17 Oct 2026) ------------------------------
    // Sean's item 1 this round: "New chat" aimed at the Mix Bus brought back the previous Kick-Link
    // run, drew its closing and failed step, appended his answer to it and answered about the old
    // target. A row cannot test that without being able to press the button, so this calls
    // createNewChat - the same function the sidebar's "+ New chat" calls - and nothing else.
    inline constexpr const char* newChat     = "new_chat";         // { uid } - "" = the main chat
    // ---- FOLLOWING A REPORT'S LINK (31 Oct 2026, Sean's 13:45 test; Kathy's condition 2) -------
    //
    // A successful AI MATCH press posts a card and the page keeps its message id. Her condition is
    // that following that link opens the card wherever it is and NEVER posts or duplicates, however
    // many chats he has moved through since. A row cannot test that without being able to follow
    // the link, so this calls openSessionChatAtMessage - the same call her line will make - and
    // nothing else.
    inline constexpr const char* openMessage = "open_message";     // { id } - a chat message's id
    // ---- THE AUDITION SWITCH AND THE REFERENCE'S GAIN (R9-1 and 2c, 31 Oct 2026) --------------
    //
    // R9-1's claim is about the OUTPUT while the reference is audible: the taps must see the mix,
    // the reference must arrive with nothing of ours on it, and the flip must not click. A row can
    // only check that against a reference it can predict to the sample, so `ref_audition` puts a
    // tone of a stated frequency and amplitude in the reference slot and flips it, through the
    // product's own fields (EchoJayProcessor::testAuditionTone).
    //
    // `ref_gain` is 2c's setter and the ack carries the gain that was APPLIED, which is the figure
    // Kathy's page is told - so a row checks the clamp through the same road her toggle uses.
    inline constexpr const char* refAudition = "ref_audition";     // { on, hz?, amp? }
    inline constexpr const char* refGain     = "ref_gain";         // { db } - ack: applied
    // ---- THE REAL EDITOR'S DIRECTORY AND LEASE (26 Oct 2026, round 1) ---------------------------
    //
    // Round 1 of docs/LIVE_VIEW_REAL_EDITOR_2026-10-26.md is lifetime, lease and ordering, and his
    // condition on it is that it is *"green in the rig before the V2 ever draws a lent editor"*. So
    // the rig drives the lease itself, from inside the V2, with a borrower that records what it was
    // told and shows nothing: that proves the two BUNDLES meet in one table (they are separate
    // Mach-O images - see EJProcessGlobal.h), that the lease is exclusive, and that every teardown
    // order reaches the holder - without a lent editor on screen.
    //
    //   { op: "table" }                  is there one shared table in this process, and who made it
    //   { op: "find",      uid }         is that Link findable from the V2's image
    //   { op: "who",       uid, n }      who holds slot n's editor
    //   { op: "take",      uid, n }      take it (the real editor is created and held, never shown)
    //   { op: "give_back", uid, n }      return it (its owner destroys the editor)
    //   { op: "revokes" }                how many times the rig's borrower has been told to let go
    //   { op: "road",      uid }         which live view that Link would get, and why (the switch)
    //   { op: "link_window", uid, n }    ask that Link to open slot n in ITS OWN window (the
    //                                    hand-back, from the Link's side - his condition of 26 Oct)
    //   { op: "arm_self" }               put the V2's OWN rack in the directory, so its structural
    //                                    doors can be driven exactly (a Link's need an agent turn)
    inline constexpr const char* lease       = "lease";            // { op, uid, n }
    // ---- A KNOB ON THE LIVE VIEW, THROUGH THE KNOB'S OWN ROAD (26 Oct 2026) ---------------------
    //
    // His rule after the 16:35 regression: *"a guard that a live-view knob change reaches the Link
    // and stays there with the switch off"*, and *"if any row that covers the live view, control or
    // settings carrying over skips or doesn't run, that is a stop"*.
    //
    // A synthetic mouse drag on a hosted AU's own view could not carry that row: nine of them moved
    // no parameter at all (25 Oct), which is why those rows skip rather than pass. This moves the
    // COPY'S parameter the way its knob does - beginChangeGesture, setValueNotifyingHost,
    // endChangeGesture - so the product's own listener, ring and staging carry it from there, with
    // nothing of the control road bypassed.
    inline constexpr const char* liveParam   = "live_param";       // { i, v } - the copy's parameter i to v
    // press gains "undo" (fix 4) alongside apply / apply_all / skip_all / skip_line / details / stop.
}

/** One parsed command. `seq` 0 means "nothing to do". */
struct Command
{
    int seq = 0;
    juce::String name;
    juce::var args;
    /** ---- WHICH INSTANCE THIS IS FOR (16 Oct 2026, the matrix's T3) ---------------------------
        One command file, and from today TWO V2 editors can be open at once - the AU and the VST3 -
        which is how the matrix produces the rack-selection lock ("this rack is selected on X -
        deselect there to edit here"). Without addressing, both would obey every command and the
        lock could never be made to happen. `for` is the wrapper name ("AU" or "VST3"); absent means
        every instance, which is what every row written before today sends. */
    juce::String forWrapper;

    juce::String str (const char* key) const { return args.getProperty (key, juce::var()).toString(); }
    bool flag (const char* key) const
    {
        const auto v = args.getProperty (key, juce::var());
        if (v.isBool()) return (bool) v;
        const auto s = v.toString().trim().toLowerCase();
        return s == "1" || s == "true" || s == "yes" || s == "on";
    }
    double num (const char* key, double fallback = 0.0) const
    {
        const auto v = args.getProperty (key, juce::var());
        return (v.isDouble() || v.isInt() || v.isInt64()) ? (double) v : fallback;
    }
};

/** Read the pending command, or {} when there is none or it has already run.
    `lastSeq` is the caller's memory of what it has executed - the editor keeps
    one, so a command file that outlives an editor is not replayed by the next. */
inline Command poll (int lastSeq)
{
    if (! enabled()) return {};
    const auto f = cmdFile();
    if (! f.existsAsFile()) return {};
    const auto v = juce::JSON::parse (f.loadFileAsString());
    auto* o = v.getDynamicObject();
    if (o == nullptr) return {};
    Command c;
    c.seq  = (int) v.getProperty ("seq", 0);
    c.name = v.getProperty ("cmd", juce::var()).toString().trim();
    c.args = v.getProperty ("args", juce::var());
    c.forWrapper = v.getProperty ("for", juce::var()).toString().trim();
    if (c.seq <= lastSeq || c.name.isEmpty()) return {};
    return c;
}

/** Whether a command addressed to `forWrapper` is this instance's to run. An unaddressed
    command belongs to everyone (and in a one-editor run, which is every run before today,
    that is every command). The comparison is loose on purpose: JUCE spells the wrapper
    "AudioUnit" and "VST3", and the rig says "AU" and "VST3". */
inline bool addressedTo (const Command& c, const juce::String& myWrapper)
{
    if (c.forWrapper.isEmpty()) return true;
    const auto want = c.forWrapper.toLowerCase(), mine = myWrapper.toLowerCase();
    if (want == mine) return true;
    if (want == "au" && mine.startsWith ("audiounit")) return true;
    if (want == "audiounit" && mine == "au") return true;
    return false;
}

/** Say what happened, so the rig waits on a fact rather than on a sleep. A rig
    that slept would be the "decisions on uncontrolled timing" fault in a new
    costume, so every command is acknowledged and the rig blocks on the ack. */
inline void ack (int seq, const juce::String& name, bool ok, const juce::String& note = {})
{
    if (! enabled()) return;
    auto* o = new juce::DynamicObject();
    o->setProperty ("seq", seq);
    o->setProperty ("cmd", name);
    o->setProperty ("ok", ok);
    o->setProperty ("note", note);
    o->setProperty ("atMs", (double) juce::Time::currentTimeMillis());
    ackFile().replaceWithText (juce::JSON::toString (juce::var (o), true));
}

/** Write the state the rig asserts on. The caller fills the object; this only
    owns where it goes and that it is written atomically enough to read. */
inline void publishState (juce::var state)
{
    if (! enabled()) return;
    // ATOMIC, VIA A RENAME. replaceWithText truncates and rewrites in place, so a reader can catch the file
    // half-written - and a half-written JSON that happens to parse is worse than one that does not. The rig
    // read exactly that twice: an ownRack with count 2 and an EMPTY slots array, and a transcript missing its
    // user rows, both from snapshots the plugin was in the middle of writing. A rename cannot be observed
    // half-done on the same filesystem.
    const auto text = juce::JSON::toString (state, true);
    auto tmp = dir().getChildFile ("rig-state.json.tmp");
    tmp.deleteFile();
    if (! tmp.replaceWithText (text)) return;
    // replaceFileIn, NOT deleteFile + moveFileTo. The first cut did the latter and it worked exactly once:
    // after that the move failed every time and the state file FROZE at the first snapshot, so eight rig
    // rows argued with the plugin about a file that had not changed since command 1 (the row detail said
    // forSeq=1, which is what finally named it). replaceFileIn is the one call that means "this file becomes
    // that file", and its result is checked rather than assumed.
    if (! tmp.replaceFileIn (stateFile()))
    {
        // Falling back to a plain write is better than leaving the reader with a stale snapshot: a torn read
        // is detectable (the JSON will not parse and the reader retries), a stale one is not.
        stateFile().replaceWithText (text);
        tmp.deleteFile();
    }
}

} // namespace echojay::rig
