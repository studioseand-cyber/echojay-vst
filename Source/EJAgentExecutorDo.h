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

    DoSinks sinks_;
};

} // namespace echojay::agent
