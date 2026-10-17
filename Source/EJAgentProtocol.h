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
        default:
            // AN UNKNOWN FRAME STILL CARRIES ITS WORDS (17 Oct 2026). `message` frames are not the
            // agent parser's business - the chat stream draws them - but their TEXT is what Sean
            // reads, and two of this round's items are about those exact sentences. Carrying the
            // text here costs nothing and lets the client log it, which is what makes a session
            // replayable instead of merely describable. "text" is the field the server sends;
            // "content" is accepted because an older shape used it.
            f.text = o->getProperty ("text").toString();
            if (f.text.isEmpty()) f.text = o->getProperty ("content").toString();
            break;
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
                                    const juce::String& appVersion, const juce::String& chatId,
                                    const juce::String& approvalMode = {})
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("goal", goal);
    o->setProperty ("context", context.isVoid() ? juce::var (new juce::DynamicObject()) : context);
    o->setProperty ("agentMode", true);
    // AUTO MODE (Sean, 14 Oct). The SERVER owns the approval policy, so the mode has to be in the request
    // and honoured there: in "auto" B may mark free what it would otherwise mark ask_first, EXCEPT anything
    // irreversible or outside the rack, which stays ask_first whatever the mode says. The plugin enforces
    // its own undoability rule on top (ExecutorDo::undoabilityOf), so a disagreement between us is safe in
    // the direction that ASKS. Server limits - the -0.1 ceiling and the rest - do not move in Auto.
    // Defaulted and only sent when set, so an older caller's body is byte-for-byte what it was.
    if (approvalMode.isNotEmpty()) o->setProperty ("approvalMode", approvalMode);
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
// A FLAG, read the way a model actually sends one: true/false, 1/0, or the words. Item 6(b) needs it for the
// cross-channel confirmation, and a flag that only understood JSON true would read a model's "true" as absent -
// which on a confirmation gate fails in the UNSAFE direction (acting unasked), so all three spellings count.
inline bool argFlag (const ToolCall& c, const char* key)
{
    if (auto* o = c.args.getDynamicObject())
    {
        const auto v = o->getProperty (key);
        if (v.isBool()) return (bool) v;
        if (v.isInt() || v.isInt64() || v.isDouble()) return ((double) v) != 0.0;
        const auto t = v.toString().trim().toLowerCase();
        return t == "true" || t == "yes" || t == "1";
    }
    return false;
}
// Was the flag SENT at all? argFlag answers "is it true", which cannot distinguish "absolute: false" from
// "no absolute argument" - and item 10 needs that distinction to pick the right default per argument.
inline bool argFlagPresent (const ToolCall& c, const char* key)
{
    if (auto* o = c.args.getDynamicObject()) return o->hasProperty (key);
    return false;
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
    /** TRUE when the server sent no choices AND allowFreeText false - an ask nobody could answer.
        The plugin takes typing anyway and this records that it had to (FOR_B, 16 Oct 2026). */
    bool freeTextForced = false;
    /** Which key the choices arrived under, and which key carried a label - "" when none did.
        Evidence for B rather than a guess about B. */
    juce::String choiceKey, labelKey;
};
/** The label of one choice. B's contract says `label`; a choice that arrived
    under another name was dropped silently, and a dropped choice is an ask with
    nothing to tap (16 Oct 2026, Sean's blocker). Read the alternatives too, and
    SAY which key answered so the log names the shape rather than the symptom. */
inline juce::String choiceLabelOf (juce::DynamicObject& co, juce::String& keyUsed)
{
    for (const char* k : { "label", "name", "title", "text", "value", "plugin", "option" })
        if (co.hasProperty (k))
        {
            const auto v = co.getProperty (k).toString().trim();
            if (v.isNotEmpty()) { keyUsed = k; return v; }
        }
    return {};
}

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
    // ---- THE CHOICES, UNDER ANY OF THEIR NAMES (16 Oct 2026) -----------------------------------
    // "an ask always shows its choices AND accepts a typed answer" (Sean, 16 Oct). The 09:19 ask
    // logged `choices=` empty with allowFreeText false - a dead end. Whether B sent nothing or sent
    // it under another key, the plugin reads every plausible key before it concludes there are none.
    juce::String arrayKey, labelKey;
    const juce::Array<juce::var>* ch = nullptr;
    for (const char* k : { "choices", "options", "answers", "buttons", "chips" })
        if (auto* arr = ao->getProperty (k).getArray()) { ch = arr; arrayKey = k; break; }
    if (ch != nullptr)
        for (auto& cv : *ch)
        {
            juce::String label, detail, intent;
            if (cv.isString()) { label = cv.toString().trim(); labelKey = "(bare string)"; }
            else if (auto* co = cv.getDynamicObject())
            {
                label  = choiceLabelOf (*co, labelKey);
                detail = co->getProperty ("detail").toString().trim();
                intent = co->getProperty ("intent").toString().trim();
            }
            if (label.isEmpty()) continue;
            a.labels.add (label); a.details.add (detail); a.intents.add (intent);
        }
    a.choiceKey = arrayKey;
    a.labelKey  = labelKey;
    // ---- AN ASK WITH NO CHOICES ALWAYS TAKES TYPING ---------------------------------------------
    // allowFreeText=false means "tap, do not type". With nothing to tap it means "you cannot
    // answer", which is not a state the server is allowed to put the user in: the run hung until
    // Sean pressed Stop. The flag is overridden here, at the parse, so every reader downstream
    // (the composer, the row, the rig) sees one truth.
    if (a.labels.isEmpty() && ! a.allowFreeText)
    {
        a.allowFreeText  = true;
        a.freeTextForced = true;
    }
    return a;
}

/** WHETHER TWO PLUGIN NAMES ARE THE SAME PLUGIN (16 Oct 2026, Sean's #9).
    A rack slot's name is the plugin's REGISTRATION name and the model asks by the name a person
    uses: "Renaissance Axx" against "Renaissance Axx (s)" (Waves' stereo variant, which the scan
    renames to - EJScan logs it as [feed-variant]), "MTransient" against "MTransient (m)". So the
    comparison drops case, spaces, punctuation and a trailing one-or-two-letter variant tag in
    brackets, and then requires one to CONTAIN the other - never a fuzzy score, because the only use
    is deciding which slot a read-back belongs to and a wrong answer there reports a value from
    another plugin. */
inline juce::String flattenPluginName (const juce::String& raw)
{
    auto t = raw.trim();
    // a trailing variant tag: "(s)", "(m)", "(mono)", "(stereo)" - the scan's own suffixes
    if (t.endsWithChar (')'))
    {
        const int open = t.lastIndexOfChar ('(');
        if (open > 0)
        {
            const auto tag = t.substring (open + 1, t.length() - 1).trim().toLowerCase();
            if (tag.length() <= 6 && (tag == "s" || tag == "m" || tag == "mono" || tag == "stereo"
                                      || tag == "st" || tag == "5.1"))
                t = t.substring (0, open).trim();
        }
    }
    return t.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789");
}
inline bool namesMatchLoosely (const juce::String& a, const juce::String& b)
{
    const auto x = flattenPluginName (a), y = flattenPluginName (b);
    if (x.isEmpty() || y.isEmpty()) return false;
    return x == y || x.contains (y) || y.contains (x);
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
