#pragma once

#include "EJAgentExecutorRead.h"
#include <functional>

// ===========================================================================
// THE AGENT'S MUTATING HALF (agent mode, 10 Oct 2026)
// ===========================================================================
//
// A2 built the READ-ONLY executor and left doOp / undoStep / undoToCheckpoint
// answering `not_in_phase` with a sentence saying Session A wires them. This is
// that wiring, as a SUBCLASS rather than a replacement: every read path, every
// checkpoint and the whole start context stay exactly as A2 wrote them, and only
// the three mutating entry points are overridden. A fork would have left two
// copies of the read half to drift.
//
// THE RULES IT OBEYS, all from docs/CONTRACT_LINK_COMMANDS.md section 8, which was
// reconciled with B:
//
//   ONE `do` = ONE COMMAND = ONE ACK = ONE UNDO STEP. A round may carry several and
//   the plugin runs them in order. `set_io` may carry BOTH inDb and outDb - two ring
//   frames, still one ack and still one undo step, because the user asked for one
//   thing.
//
//   DEVICE UNITS ONLY, NEVER NORMALISED. `params` in the built-in's own units,
//   `controls` in the map's display units as the registry prints them. Normalised
//   values are what the fingerprint/map era got wrong.
//
//   A REFUSAL'S REASON REACHES THE MODEL AS A SENTENCE, unparaphrased. So every
//   failure here carries the sentence the plugin itself would say, and nothing
//   rewrites it on the way out.
//
// WHAT IT WILL NOT DO. `set_level` is NOT a write to an EchoJay Level slot any more:
// the 10 Oct ruling dropped that slot, levelling lives on the rack-level record, and
// an executor that still wrote a Level slot would be addressing a plugin that is not
// there. It writes the record, which is the door findTarget reads.

namespace echojay::agent
{

/** What the mutating executor needs beyond the read half's Sources. Supplied by the
    editor, which is the only thing that knows which rack a chat is addressing and
    how to send a command to it. */
struct DoSinks
{
    /** Apply a chain-channel op batch to the session's target ("" = our own rack).
        The var is the `edit` array the chain channel already takes, so there is ONE
        op vocabulary and this executor invents none of its own. Calls back with
        (ok, sentence) - the sentence is what the model is shown on a refusal. */
    std::function<void (const juce::String& targetUid, const juce::var& editOps,
                        std::function<void (bool, juce::String)>)> applyChainOps;

    /** ---- ITEM 1 (Sean, 14 Oct 2026): A BUILD IS NOT AN EDIT OP ------------------------------------
        A build replaces the whole rack and has its own road on each side: the V2's own build path for the
        open rack, and the chain-build command for a Link. It was being emitted as an `edit` op called
        "build", which the edit-op vocabulary has never had - so the Link answered `op 1 invalid: unknown
        operation "build"` and the own rack answered "that change carried no operation this rack
        understands". Two different sentences for one missing road.

        The var is the chain ARRAY (the block's own schema, passed through whole). The callback says only
        whether the build was DISPATCHED; whether it LANDED is decided by a read-back of the rack, because
        a build takes seconds and a dispatch is not a landing. */
    std::function<void (const juce::String& targetUid, const juce::var& chainArray,
                        std::function<void (bool, juce::String)>)> buildChain;

    /** Run something after a delay, on the message thread. The executor uses it to re-read a rack after a
        structural change rather than reading once and calling an empty rack a refusal - a build's slots
        arrive over seconds. A hook rather than a juce::Timer so a guard can drive the clock itself and
        prove the poll, which a real timer inside the executor would make untestable. */
    std::function<void (int delayMs, std::function<void()>)> afterMs;

    /** Is agent mode ON? Checked on every mutating call and not cached: a user who
        turns it off mid-round means it, and a cached answer would let the rest of a
        round through. */
    std::function<bool()> agentModeOn;

    /** The EchoJay-only setting, for the start context. Separate from Dial only;
        both can be on (Sean's 10 Oct ruling). */
    std::function<bool()> echoJayOnly;

    /** Undo one step / back to a checkpoint, on whichever rack the step belongs to.
        Returns (ok, sentence). */
    std::function<void (const juce::String& token, std::function<void (bool, juce::String)>)> undoOne;
    // RULING 4 (Sean, 14 Oct 2026): {"op":"undo"} walks the PLUGIN'S undo history one step - not an agent
    // step keyed by a token, which is what undoOne above is. The callback carries what was undone and the
    // history's depth BEFORE and AFTER, because the depth moving is the only read-back an undo has: the
    // rig watched the agent answer "I've undone the pre-gain change" having removed a slot (row C10), and a
    // sink that reported its own return value would have called that a success too.
    std::function<void (std::function<void (bool ok, juce::String label, int depthBefore, int depthAfter)>)> undoHistoryStep;

    /** ---- ITEM 10 (Sean, 13 Oct 2026): THE RACK'S OUT GAIN, AS ITS OWN OP ----------------------------
        Sean asked to turn the vocal rack output down 2 dB and the server dialled the EchoJay EQ's
        output_db - a plugin's output inside the rack, not the rack's. It had no choice: there was no op
        that meant "the rack's OUT", so the nearest thing with the word "output" in it won.

        It is NOT a chain-channel edit op, which is why it needs a sink of its own. The rack OUT is the
        gain the levelling loop's `rack_out` stage writes, and that is a different control on each side:
        the V2's own bus gain (setBusGainDb) on this instance, and the Link's mixer-strip GAIN
        (writeLinkCtrlCommand "gainDb") on a Link. Sending it as an `edit` op would reach neither - the
        rack editor's op list has no name for it, and the one place a name was invented ("link_gain", in
        the ring's fallback table) is not in the Link's accepted list either.

        absolute: true  -> db IS the new gain.      absolute: false -> db is a DELTA on the current gain.
        A delta is what a user asking for "down 2 dB" means, so that is what the op defaults to. */
    std::function<void (const juce::String& targetUid, double db, bool absolute,
                        std::function<void (bool, juce::String)>)> setRackOutDb;
};

class ExecutorDo : public ExecutorRead
{
public:
    ExecutorDo (Sources s, DoSinks sinks);

    /** A2's start context, plus the two facts an agent must know before it plans:
        whether it may act at all, and whether it may only pick EchoJay's own
        devices. Both are read live. */
    juce::var startContext() override;

    void doOp (const ToolCall& call, Done done) override;
    void undoStep (const juce::String& token, Done done) override;
    void undoToCheckpoint (const juce::String& token, Done done) override;

private:
    /** Translate one `do` op into the chain channel's `edit` array, or fail with the
        sentence the model should see. Returns an empty var on refusal. */
    juce::var translate (const ToolCall& call, juce::String& refusal) const;
    // ITEM 1 (13 Oct 2026): the slot floor a structural op must read back, or -1 when the op does not change
    // the rack's shape. Public on purpose - the guard drives it directly, so the rule is checkable without
    // standing up a rack.
public:
    int structuralWantSlots (const ToolCall& call, const juce::String& opName) const;

    // ITEM 1 (14 Oct 2026): how long a structural change is given to appear on the target rack, and how
    // often the read-back asks. A build instantiates plugins, so the first read can honestly show nothing;
    // reading once and calling that a refusal would be the same mistake as calling a file write a landing,
    // pointed the other way. Public so a guard can compute the number of polls it must drive.
    static constexpr int kLandPollMs    = 250;
    static constexpr int kLandWaitMaxMs = 20000;
private:
    /** Resolve the call's `channel` into the uid to act on, applying the cross-channel confirmation gate.
        Returns false when it has already answered `done` (unknown channel, or confirmation needed), so
        every caller shares one gate instead of re-implementing it per op. */
    bool resolveTargetWithConfirm (const ToolCall& call, Done& done, juce::String& applyTo);

    /** Re-read the target rack until it holds `wantSlots` or the wait runs out, then answer. The one place
        that decides a structural change LANDED. */
    void confirmLanded (const juce::String& applyTo, int wantSlots, const juce::String& opName,
                        juce::String sentence, Done done, int waitedMs);

    DoSinks sinks_;
};

} // namespace echojay::agent
