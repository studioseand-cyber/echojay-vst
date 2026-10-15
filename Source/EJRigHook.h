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
namespace cmd
{
    inline constexpr const char* sendChat     = "send_chat";       // { text }
    inline constexpr const char* agentChip    = "agent_chip";      // { on: bool }
    inline constexpr const char* press        = "press";           // { button: apply|apply_all|skip_all|skip_line|details|stop }
    inline constexpr const char* selectLink   = "select_link";     // { uid }  ("" = the own rack)
    inline constexpr const char* reportState  = "report_state";    // {}
    inline constexpr const char* setRackOut   = "set_rack_out";    // { db }   - the user's own knob, not the agent op
    // Sean's ruling 3 (13 Oct 2026), so the rig covers the three callbacks the sweep found one-ended:
    inline constexpr const char* openSlot     = "open_slot";       // { n }    - a slot DOUBLE-CLICK (1-based)
    inline constexpr const char* preGainReset = "pre_gain_reset";  // {}       - the pre-gain knob's double-click
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
    // press gains "undo" (fix 4) alongside apply / apply_all / skip_all / skip_line / details / stop.
}

/** One parsed command. `seq` 0 means "nothing to do". */
struct Command
{
    int seq = 0;
    juce::String name;
    juce::var args;

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
    if (c.seq <= lastSeq || c.name.isEmpty()) return {};
    return c;
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
