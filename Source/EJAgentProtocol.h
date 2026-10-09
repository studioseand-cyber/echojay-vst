#pragma once
// =============================================================================
//  EJAgentProtocol — the agent loop's WIRE VOCABULARY, as pure functions.
//
//  Source of truth: ~/echojay-saas/docs/AGENT_MODE_PLAN.md section 1.3 (9 Oct
//  2026). docs/CONTRACT_AGENT_TOOLS.md, which the brief names, does not exist
//  yet in that repo; everything here follows the plan's examples and is written
//  to be corrected in ONE place when the contract lands.
//
//  Server -> client frames (SSE):
//     round      {"sessionId":"ag_...","round":3}
//     delta      {"text":"..."}                                   talk, streamed
//     tool_call  {"id","name","args","approval","summary","why"}
//     await      {"ids":[...]}                                    the server waits for ONE step
//     done       {"summary","rounds","usage","costUsd"}
//     error      {"code","message","retryable"}
//  The frame TYPE is read from the SSE event name first and from a "type"
//  property inside the JSON second, so the client is right whichever shape B
//  ships (handoff open question 1).
//
//  Client -> server: POST /api/agent/step
//     {"sessionId","round","results":[{"id","ok":true,"result":{...}} | {"id","ok":false,"error":{"code","message"}}]}
//
//  Header-only on purpose, juce_core only: the guard and the client include
//  it, and Source/*.cpp is an explicit CMake list.
// =============================================================================
#include <juce_core/juce_core.h>

namespace echojay::agent
{

enum class FrameKind { Round, Delta, ToolCall, Await, Done, Error, Unknown };

struct ToolCall
{
    juce::String id, name, approval, summary, why;
    juce::var    args;
    bool askFirst() const noexcept { return approval == "ask_first"; }
};

struct Frame
{
    FrameKind    kind = FrameKind::Unknown;
    juce::String eventName;          // as received ("" when the type rode inside the JSON)
    // round
    juce::String sessionId; int round = 0;
    // delta
    juce::String text;
    // tool_call
    ToolCall call;
    // await
    juce::StringArray awaitIds;
    // done
    juce::String summary; int rounds = 0; double costUsd = 0.0; juce::var usage;
    // error
    juce::String errorCode, errorMessage; bool retryable = false;
    juce::var raw;
};

inline FrameKind frameKindOf (const juce::String& name) noexcept
{
    if (name == "round")     return FrameKind::Round;
    if (name == "delta")     return FrameKind::Delta;
    if (name == "tool_call") return FrameKind::ToolCall;
    if (name == "await")     return FrameKind::Await;
    if (name == "done")      return FrameKind::Done;
    if (name == "error")     return FrameKind::Error;
    return FrameKind::Unknown;
}

inline const char* frameKindName (FrameKind k) noexcept
{
    switch (k)
    {
        case FrameKind::Round:    return "round";
        case FrameKind::Delta:    return "delta";
        case FrameKind::ToolCall: return "tool_call";
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
    // The approval class is the SERVER's decision (plan section 2). When a frame
    // carries none, the SAFE default applies: a mutation asks, everything else
    // is free. Never the other way round.
    if (c.approval.isEmpty())
        c.approval = (c.name == "do") ? "ask_first" : "free";
    return c;
}

// eventName may be empty (type inside the JSON) and dataJson is the SSE data payload.
inline Frame parseFrame (const juce::String& eventName, const juce::String& dataJson)
{
    Frame f;
    f.eventName = eventName;
    f.raw = juce::JSON::parse (dataJson);
    auto* o = f.raw.getDynamicObject();
    juce::String typeName = eventName;
    if (typeName.isEmpty() && o != nullptr)
        typeName = o->getProperty ("type").toString();
    f.kind = frameKindOf (typeName);
    if (o == nullptr)
    {
        // A delta whose data is a bare string is tolerated; anything else
        // malformed is Unknown and the client skips it (the chat-stream rule:
        // a malformed frame is skipped, never fatal).
        if (f.kind == FrameKind::Delta && f.raw.isString()) f.text = f.raw.toString();
        else if (f.kind != FrameKind::Unknown && ! f.raw.isString()) f.kind = FrameKind::Unknown;
        return f;
    }
    switch (f.kind)
    {
        case FrameKind::Round:
            f.sessionId = o->getProperty ("sessionId").toString();
            f.round     = (int) o->getProperty ("round");
            break;
        case FrameKind::Delta:
            f.text = o->getProperty ("text").toString();
            break;
        case FrameKind::ToolCall:
            f.call = parseToolCall (f.raw);
            break;
        case FrameKind::Await:
            if (auto* a = o->getProperty ("ids").getArray())
                for (auto& id : *a) f.awaitIds.add (id.toString());
            break;
        case FrameKind::Done:
            f.summary = o->getProperty ("summary").toString();
            f.rounds  = (int) o->getProperty ("rounds");
            f.costUsd = (double) o->getProperty ("costUsd");
            f.usage   = o->getProperty ("usage");
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
    juce::var    result;                      // COMPACT, the executor's summary (never meter arrays)
    juce::String errorCode, errorMessage;
};

inline ToolResult okResult (const juce::String& id, juce::var result = juce::var())
{ ToolResult r; r.id = id; r.ok = true; r.result = std::move (result); return r; }

inline ToolResult failedResult (const juce::String& id, const juce::String& code, const juce::String& message)
{ ToolResult r; r.id = id; r.ok = false; r.errorCode = code; r.errorMessage = message; return r; }

inline ToolResult declinedResult (const juce::String& id)             { return failedResult (id, "declined", "user declined"); }
inline ToolResult stoppedResult (const juce::String& id)              { return failedResult (id, "stopped", "user stopped the agent"); }
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

// The start body. `context` is the executor's opening read (channel, rack
// summary, inventory shortlist) and is sent as given; `agentMode: true` is the
// phase-1 dev switch the plan names (section 7) and goes on every start.
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

// wait_for_playback rides as check(what:"playback") per plan 1.4; the bare
// name is accepted too so a contract that promotes it to a tool needs no client change.
inline bool isPlaybackWait (const ToolCall& c)
{
    return c.name == "wait_for_playback" || (c.name == "check" && argString (c, "what") == "playback");
}
inline double playbackMinSeconds (const ToolCall& c, double fallback = 8.0)
{
    return juce::jlimit (1.0, 120.0, argNumber (c, "min_seconds", fallback));
}

// talk(text, ask?) - the ask carries {question, choices:[ "label" | {label, intent?} ]}
struct TalkAsk { juce::String question; juce::StringArray labels; bool present = false; };
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
    if (auto* ch = ao->getProperty ("choices").getArray())
        for (auto& cv : *ch)
        {
            if (cv.isString()) a.labels.add (cv.toString().trim());
            else if (auto* co = cv.getDynamicObject()) a.labels.add (co->getProperty ("label").toString().trim());
        }
    a.labels.removeEmptyStrings();
    return a;
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
        juce::String s = op.isNotEmpty() ? op : juce::String ("change");
        if (slot.isNotEmpty()) s += " slot " + slot;
        const auto plugin = argString (c, "plugin");
        if (plugin.isNotEmpty()) s += " " + plugin;
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
