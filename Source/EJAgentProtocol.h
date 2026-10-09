#pragma once
// =============================================================================
//  EJAgentProtocol — the agent loop's WIRE VOCABULARY, as pure functions.
//
//  Source of truth: ~/echojay-saas/docs/CONTRACT_AGENT_TOOLS.md (B, 9 Oct 2026,
//  branch hold/agent-contract), sections 2 (tools), 3 (approvals / the plan
//  card) and 4 (wire format). Where this file and that one disagree, that one
//  wins and this one is the single place to fix.
//
//  Server -> client frames: SSE, typed by the `event:` line ONLY (no "type"
//  field in the JSON). await, done and error are TERMINAL: the server closes
//  the response after one.
//     round      {"sessionId":"ag_...","round":3,"ofSoftCap":20}
//     delta      {"text":"..."}                                          talk, streamed
//     tool_call  {"id","name","args","approval":"free"|"ask_first","summary","why"?}
//     plan       {"round","heading","items":[{"id","summary","why"?,"approval":"ask_first"}]}
//     await      {"ids":[...],"timeoutMs":120000}
//     done       {"reason":"complete"|"stopped"|"hard_cap"|"error","summary","rounds","usage","costUsd"}
//     error      {"code","message","retryable"}
//
//  Client -> server:
//     POST /api/agent/start  {goal, context, agentMode:true, appVersion, chatId?}
//     POST /api/agent/step   {sessionId, round, results:[{id, ok:true, result} | {id, ok:false, error:{code,message}}]}
//     POST /api/agent/stop   {sessionId}
//
//  Header-only on purpose, juce_core only: the guard and the client include
//  it, and Source/*.cpp is an explicit CMake list.
// =============================================================================
#include <juce_core/juce_core.h>
#include <vector>

namespace echojay::agent
{

enum class FrameKind { Round, Delta, ToolCall, Plan, Await, Done, Error, Unknown };

struct ToolCall
{
    juce::String id, name, approval, summary, why;
    juce::var    args;
    bool askFirst() const noexcept { return approval == "ask_first"; }
};

struct PlanItem { juce::String id, summary, why, approval; };

struct Frame
{
    FrameKind    kind = FrameKind::Unknown;
    juce::String eventName;          // as received
    // round
    juce::String sessionId; int round = 0; int ofSoftCap = 0;
    // delta
    juce::String text;
    // tool_call
    ToolCall call;
    // plan
    juce::String planHeading; int planRound = 0; std::vector<PlanItem> planItems;
    // await
    juce::StringArray awaitIds; int awaitTimeoutMs = 0;
    // done
    juce::String doneReason, summary; int rounds = 0; double costUsd = 0.0; juce::var usage;
    // error
    juce::String errorCode, errorMessage; bool retryable = false;
    juce::var raw;
};

inline FrameKind frameKindOf (const juce::String& name) noexcept
{
    if (name == "round")     return FrameKind::Round;
    if (name == "delta")     return FrameKind::Delta;
    if (name == "tool_call") return FrameKind::ToolCall;
    if (name == "plan")      return FrameKind::Plan;
    if (name == "await")     return FrameKind::Await;
    if (name == "done")      return FrameKind::Done;
    if (name == "error")     return FrameKind::Error;
    return FrameKind::Unknown;
}

inline bool isTerminalFrame (FrameKind k) noexcept
{
    return k == FrameKind::Await || k == FrameKind::Done || k == FrameKind::Error;
}

inline const char* frameKindName (FrameKind k) noexcept
{
    switch (k)
    {
        case FrameKind::Round:    return "round";
        case FrameKind::Delta:    return "delta";
        case FrameKind::ToolCall: return "tool_call";
        case FrameKind::Plan:     return "plan";
        case FrameKind::Await:    return "await";
        case FrameKind::Done:     return "done";
        case FrameKind::Error:    return "error";
        default:                  return "unknown";
    }
}

inline ToolCall parseToolCall (const juce::var& v)
{
    ToolCall c;
    if (auto* o = v.getDynamicObject())
    {
        c.id       = o->getProperty ("id").toString();
        c.name     = o->getProperty ("name").toString();
        c.args     = o->getProperty ("args");
        c.approval = o->getProperty ("approval").toString();
        c.summary  = o->getProperty ("summary").toString();
        c.why      = o->getProperty ("why").toString();
    }
    // The approval class is the SERVER's decision (contract section 3). When a
    // frame carries none, the SAFE default applies: a mutation asks, everything
    // else is free. Never the other way round.
    if (c.approval.isEmpty())
        c.approval = (c.name == "do") ? "ask_first" : "free";
    return c;
}

// The frame type comes from the SSE event name ONLY (contract section 4). An
// event with no name, or JSON that does not parse to an object, is Unknown and
// the client skips it - the chat-stream rule: a malformed frame is never fatal.
inline Frame parseFrame (const juce::String& eventName, const juce::String& dataJson)
{
    Frame f;
    f.eventName = eventName;
    f.kind = frameKindOf (eventName);
    f.raw = juce::JSON::parse (dataJson);
    auto* o = f.raw.getDynamicObject();
    if (o == nullptr) { f.kind = FrameKind::Unknown; return f; }
    switch (f.kind)
    {
        case FrameKind::Round:
            f.sessionId = o->getProperty ("sessionId").toString();
            f.round     = (int) o->getProperty ("round");
            f.ofSoftCap = (int) o->getProperty ("ofSoftCap");
            break;
        case FrameKind::Delta:
            f.text = o->getProperty ("text").toString();
            break;
        case FrameKind::ToolCall:
            f.call = parseToolCall (f.raw);
            break;
        case FrameKind::Plan:
            f.planHeading = o->getProperty ("heading").toString();
            f.planRound   = (int) o->getProperty ("round");
            if (auto* items = o->getProperty ("items").getArray())
                for (auto& iv : *items)
                    if (auto* io = iv.getDynamicObject())
                    {
                        PlanItem p;
                        p.id       = io->getProperty ("id").toString();
                        p.summary  = io->getProperty ("summary").toString();
                        p.why      = io->getProperty ("why").toString();
                        p.approval = io->getProperty ("approval").toString();
                        if (p.approval.isEmpty()) p.approval = "ask_first";
                        if (p.id.isNotEmpty()) f.planItems.push_back (p);
                    }
            break;
        case FrameKind::Await:
            if (auto* a = o->getProperty ("ids").getArray())
                for (auto& id : *a) f.awaitIds.add (id.toString());
            f.awaitTimeoutMs = (int) o->getProperty ("timeoutMs");
            break;
        case FrameKind::Done:
            f.doneReason = o->getProperty ("reason").toString();
            f.summary    = o->getProperty ("summary").toString();
            f.rounds     = (int) o->getProperty ("rounds");
            f.costUsd    = (double) o->getProperty ("costUsd");
            f.usage      = o->getProperty ("usage");
            break;
        case FrameKind::Error:
            f.errorCode    = o->getProperty ("code").toString();
            f.errorMessage = o->getProperty ("message").toString();
            f.retryable    = (bool) o->getProperty ("retryable");
            break;
        default: break;
    }
    return f;
}

// ---- results ------------------------------------------------------------------
struct ToolResult
{
    juce::String id;
    bool         ok = true;
    juce::var    result;                      // COMPACT, the executor's summary (never meter arrays; < 8 KB)
    juce::String errorCode, errorMessage;
};

inline ToolResult okResult (const juce::String& id, juce::var result = juce::var())
{ ToolResult r; r.id = id; r.ok = true; r.result = std::move (result); return r; }

inline ToolResult failedResult (const juce::String& id, const juce::String& code, const juce::String& message)
{ ToolResult r; r.id = id; r.ok = false; r.errorCode = code; r.errorMessage = message; return r; }

// contract 2.2 / 3: Skip on the plan card is approval_declined (from the plugin, not the validator)
inline ToolResult declinedResult (const juce::String& id)             { return failedResult (id, "approval_declined", "the user tapped Skip"); }
inline ToolResult stoppedResult (const juce::String& id)              { return failedResult (id, "stopped", "the user stopped the agent"); }
inline ToolResult notRunResult (const juce::String& id)               { return failedResult (id, "not_run", "an earlier do in this round failed, so this one did not run"); }
inline ToolResult notInPhaseResult (const juce::String& id)           { return failedResult (id, "not_in_phase", "this build does not execute that tool yet"); }

inline juce::var toolResultVar (const ToolResult& r)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", r.id);
    o->setProperty ("ok", r.ok);
    if (r.ok)
        o->setProperty ("result", r.result.isVoid() ? juce::var (new juce::DynamicObject()) : r.result);
    else
    {
        auto* e = new juce::DynamicObject();
        e->setProperty ("code", r.errorCode);
        e->setProperty ("message", r.errorMessage);
        o->setProperty ("error", juce::var (e));
    }
    return juce::var (o);
}

inline juce::String buildStepBody (const juce::String& sessionId, int round, const std::vector<ToolResult>& results)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("sessionId", sessionId);
    o->setProperty ("round", round);
    juce::Array<juce::var> arr;
    for (const auto& r : results) arr.add (toolResultVar (r));
    o->setProperty ("results", juce::var (arr));
    return juce::JSON::toString (juce::var (o), true);
}

// {goal, context, agentMode:true, appVersion, chatId?}. `context` is the executor's opening read (contract 1:
// channel {uid, name, kind, links}, capabilities) and is sent as given; `agentMode: true` is the dev switch both
// sides gate on and goes before beta.
inline juce::String buildStartBody (const juce::String& goal, const juce::var& context,
                                    const juce::String& appVersion, const juce::String& chatId)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("goal", goal);
    o->setProperty ("context", context.isVoid() ? juce::var (new juce::DynamicObject()) : context);
    o->setProperty ("agentMode", true);
    if (appVersion.isNotEmpty()) o->setProperty ("appVersion", appVersion);
    if (chatId.isNotEmpty())     o->setProperty ("chatId", chatId);
    return juce::JSON::toString (juce::var (o), true);
}

inline juce::String buildStopBody (const juce::String& sessionId)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("sessionId", sessionId);
    return juce::JSON::toString (juce::var (o), true);
}

// ---- reading a tool call ---------------------------------------------------------
inline juce::String argString (const ToolCall& c, const char* key)
{
    if (auto* o = c.args.getDynamicObject()) return o->getProperty (key).toString();
    return {};
}
inline double argNumber (const ToolCall& c, const char* key, double fallback)
{
    if (auto* o = c.args.getDynamicObject())
    {
        const auto v = o->getProperty (key);
        if (v.isDouble() || v.isInt() || v.isInt64()) return (double) v;
    }
    return fallback;
}

// wait_for_playback rides as check(what:"playback") (contract 2.3); the bare
// name is accepted too so a contract that promotes it to a tool needs no client change.
inline bool isPlaybackWait (const ToolCall& c)
{
    return c.name == "wait_for_playback" || (c.name == "check" && argString (c, "what") == "playback");
}
inline double playbackMinSeconds (const ToolCall& c, double fallback = 8.0)
{
    return juce::jlimit (1.0, 120.0, argNumber (c, "min_seconds", fallback));
}

// The playback wait's TIMEOUT answer, verbatim from the contract (one author for the sentence).
inline constexpr int kPlaybackTimeoutSeconds = 60;
inline juce::var playbackTimeoutResult (int waitedSeconds)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("played", false);
    o->setProperty ("waited", waitedSeconds);
    o->setProperty ("sentence", "Nothing played in 60 seconds - play the loudest section and ask me again.");
    return juce::var (o);
}

// talk(text, ask?) - ask = {question, choices:[{label, detail?, intent?}], allowFreeText}
// The soft cap is an ordinary ask with id tc_keep_going; nothing here treats it specially.
struct TalkAsk
{
    juce::String question;
    juce::StringArray labels, details, intents;
    bool allowFreeText = true;
    bool present = false;
};
inline TalkAsk talkAskOf (const ToolCall& c)
{
    TalkAsk a;
    if (c.name != "talk") return a;
    auto* o = c.args.getDynamicObject();
    if (o == nullptr) return a;
    const auto askV = o->getProperty ("ask");
    auto* ao = askV.getDynamicObject();
    if (ao == nullptr) return a;
    a.present  = true;
    a.question = ao->getProperty ("question").toString().trim();
    if (ao->hasProperty ("allowFreeText")) a.allowFreeText = (bool) ao->getProperty ("allowFreeText");
    if (auto* ch = ao->getProperty ("choices").getArray())
        for (auto& cv : *ch)
        {
            juce::String label, detail, intent;
            if (cv.isString()) label = cv.toString().trim();
            else if (auto* co = cv.getDynamicObject())
            {
                label  = co->getProperty ("label").toString().trim();
                detail = co->getProperty ("detail").toString().trim();
                intent = co->getProperty ("intent").toString().trim();
            }
            if (label.isEmpty()) continue;
            a.labels.add (label); a.details.add (detail); a.intents.add (intent);
        }
    return a;
}

// The ask's answer: {tapped:"<label>"} for a chip, {typed:"<text>"} for free text (contract 2.4).
inline juce::var askAnswerVar (const juce::String& text, bool typed)
{
    auto* o = new juce::DynamicObject();
    o->setProperty (typed ? "typed" : "tapped", text);
    return juce::var (o);
}

// The checklist line for a call: the SERVER's summary when it sent one (one
// author for the words), else a short derived line so no row is ever blank.
inline juce::String describeCall (const ToolCall& c)
{
    if (c.summary.isNotEmpty()) return c.summary;
    if (isPlaybackWait (c))
        return "Wait for playback (" + juce::String ((int) playbackMinSeconds (c)) + " s)";
    if (c.name == "look")  return "Look at " + (argString (c, "what").isNotEmpty() ? argString (c, "what") : juce::String ("the state"));
    if (c.name == "check") return "Check " + (argString (c, "what").isNotEmpty() ? argString (c, "what") : juce::String ("the result"));
    if (c.name == "do")
    {
        const auto op = argString (c, "op");
        const auto slot = argString (c, "slot");
        if (op == "open_editor") return "Open the editor" + (slot.isNotEmpty() ? " for slot " + slot : juce::String());
        juce::String s = op.isNotEmpty() ? op : juce::String ("change");
        if (slot.isNotEmpty()) s += " slot " + slot;
        const auto name = argString (c, "name").isNotEmpty() ? argString (c, "name") : argString (c, "plugin");
        if (name.isNotEmpty()) s += " " + name;
        return s;
    }
    if (c.name == "talk")
    {
        const auto ask = talkAskOf (c);
        if (ask.present) return ask.question;
        return argString (c, "text");
    }
    return c.name.isNotEmpty() ? c.name : juce::String ("step");
}

} // namespace echojay::agent
