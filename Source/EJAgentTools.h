#pragma once
// =============================================================================
//  EJAgentTools — the EXECUTOR INTERFACE the agent client drives.
//
//  The client (EJAgentClient) owns the loop: it talks to B's /api/agent/start
//  and /step, decides when a tool call may run (approval, order, stop), and
//  posts compact results. It never touches a rack, a meter or a Link itself.
//  Everything that does is behind this interface, which Session A implements
//  over ChainHost / the Link transport / the level tallies in its own file
//  (the plan's "plugin executes" half, AGENT_MODE_PLAN.md 1.1 and 7).
//
//  Threading: every call is made on the MESSAGE THREAD and completes through
//  `done` on the message thread, synchronously or later. The client tags each
//  call with a generation and ignores a completion from a stopped session, so
//  an implementation may complete after stop() without checking anything.
//
//  Results are COMPACT (plan section 2): a look(analysis) is ~250 tokens, a
//  look(rack) one line per slot, a check(level) three numbers. The executor
//  defines the summary; the client sends it verbatim.
//
//  wait_for_playback is NOT delegated: the client runs it (the UX lines, the
//  timeout, the early stop) and reads the executor's PlaybackReading on a
//  timer. The two hooks it needs are beginPlaybackWindow() and readPlayback().
//
//  Undo: a `do` that landed returns an undoToken. The client offers Undo per
//  step (undoStep) and "undo all" back to the checkpoint it captured at the
//  session's start (captureCheckpoint / undoToCheckpoint). The tokens are
//  opaque to the client; A's implementation keys them on the plugin-wide
//  UndoHistory (EchoJayUndoHistory.h: a CHAIN entry per applied line is the
//  design in LINK_REMOTE_CONTROL_PLAN.md section 9).
// =============================================================================
#include <juce_core/juce_core.h>
#include <functional>
#include <limits>
#include "EJAgentProtocol.h"

namespace echojay::agent
{

struct ToolOutcome
{
    bool         ok = true;
    juce::var    result;                 // compact, goes to the server as `result`
    juce::String errorCode, errorMessage; // when ! ok: goes to the server as `error`
    juce::String undoToken;              // a `do` that landed and can be undone on its own; "" otherwise
    juce::String landedLine;             // optional: what the checklist row says once done ("EQ: bell 300 Hz -2 dB")

    static ToolOutcome success (juce::var result, const juce::String& undoToken = {}, const juce::String& landedLine = {})
    { ToolOutcome o; o.ok = true; o.result = std::move (result); o.undoToken = undoToken; o.landedLine = landedLine; return o; }
    static ToolOutcome failure (const juce::String& code, const juce::String& message)
    { ToolOutcome o; o.ok = false; o.errorCode = code; o.errorMessage = message; return o; }
};

struct PlaybackReading
{
    bool  transportKnown = false;    // the host publishes a transport at all (LoudnessLoop's rule: unknown never blocks)
    bool  playing        = false;    // the transport says it is running (meaningful only when transportKnown)
    float heardAboveSeconds = 0.0f;  // gated audio counted SINCE beginPlaybackWindow()
    float integratedLufs    = std::numeric_limits<float>::quiet_NaN();   // over the window
    float loudestShortTermLufs = std::numeric_limits<float>::quiet_NaN(); // the loudest 3 s in the window
    float truePeakDbtp      = std::numeric_limits<float>::quiet_NaN();
};

class ToolExecutor
{
public:
    virtual ~ToolExecutor() = default;
    using Done = std::function<void (ToolOutcome)>;

    // The opening read sent as `context` on /api/agent/start: channel (role,
    // name, uid), the rack in one line per slot, the inventory shortlist.
    virtual juce::var startContext() = 0;

    virtual void look  (const ToolCall& call, Done done) = 0;
    virtual void doOp  (const ToolCall& call, Done done) = 0;   // "do": the only mutating tool
    virtual void check (const ToolCall& call, Done done) = 0;   // never called for what:"playback"

    // wait_for_playback support
    virtual void beginPlaybackWindow() = 0;          // "heard" counts from now (the loop-owned tally, not the song's)
    virtual PlaybackReading readPlayback() = 0;

    // undo
    virtual juce::String captureCheckpoint (const juce::String& label) = 0;   // "" = no checkpoint available
    virtual void undoStep (const juce::String& undoToken, Done done) = 0;
    virtual void undoToCheckpoint (const juce::String& checkpoint, Done done) = 0;
};

// -----------------------------------------------------------------------------
//  The stand-in until A's executor lands. HONEST, not helpful: every tool answers
//  not_in_phase so the model is told the truth, playback never counts a second
//  (so the wait TIMES OUT instead of pretending), and there is no checkpoint.
//  The client's loop, cards and stop path are fully exercisable against it.
// -----------------------------------------------------------------------------
class StubExecutor : public ToolExecutor
{
public:
    juce::var startContext() override
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("executor", "stub");
        return juce::var (o);
    }
    void look  (const ToolCall&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "this build has no agent executor wired (stub)")); }
    void doOp  (const ToolCall&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "this build has no agent executor wired (stub)")); }
    void check (const ToolCall&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "this build has no agent executor wired (stub)")); }
    void beginPlaybackWindow() override {}
    PlaybackReading readPlayback() override { return {}; }
    juce::String captureCheckpoint (const juce::String&) override { return {}; }
    void undoStep (const juce::String&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "no executor: nothing was applied, so nothing can be undone")); }
    void undoToCheckpoint (const juce::String&, Done done) override { done (ToolOutcome::failure ("not_in_phase", "no executor: nothing was applied, so nothing can be undone")); }
};

} // namespace echojay::agent
