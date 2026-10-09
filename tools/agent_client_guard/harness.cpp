// agent_client_guard - THE AGENT LOOP'S PLUGIN SIDE, in-process, no network, no Logic (Session A2, 9 Oct 2026).
//
// The client (EJAgentClient) is driven through a recorder in place of the socket and a fake executor in place of
// A's; frames are fed as the wire would deliver them (EJAgentProtocol::parseFrame over the plan's 1.3 examples).
// Every leg below is a sentence from AGENT_MODE_PLAN.md or from Sean's brief, and each one is RED without the
// behaviour it names:
//   P   the protocol: event-line and type-in-JSON frames parse alike; a do with no approval class asks; the step
//       body is the plan's shape; wait_for_playback is recognised in both its shapes
//   N   nextRunnable: strict wire order; an ask_first step blocks on the plan until it is submitted
//   R1  one round: free calls run, the ask_first line waits on the plan card, Approve all runs it, ONE step is
//       posted with every awaited id answered in await order, the round's talk reaches the transcript ONCE
//   R2  Decline: the declined call returns {ok:false, error:{code:"declined"}} and nothing ran
//   R3  per-line taps: a skipped line is declined, the approved one runs, order preserved
//   R4  a failed do stops the rest of the round's do calls (not_run); a look after it still runs
//   A   talk(ask): the ask card waits; a typed answer is taken as the choice and goes to the transcript
//   W1  wait_for_playback with no audio TIMES OUT and returns played:false - it never hangs
//   W2  min_seconds of audio -> "Got it, you can stop." and the reading goes back
//   W3  the user's early stop returns what was heard, marked short
//   W4  a stopped transport after some audio ends the wait early
//   S   Stop halts at once: the running step is marked Stopped, NO step is posted, a late completion and a stale
//       frame are ignored, "Stopped." reaches the transcript, Undo is reachable afterwards
//   U   Undo per step sends that step's token; Undo all sends the session's checkpoint; rows read "undone"
//   E   an error frame -> Failed (retryable keeps the body for Retry, which re-posts it byte for byte); a stream that
//       ends with no terminal frame -> Failed; no token -> "Sign in"; done -> Done, summary in the transcript; dismiss
//   T   a typed message under a running agent is refused WITH a line
//   L   the panel: at every editor width (380..1780) and height cap, no text row is shorter than its wrapped text,
//       no button overlaps text or leaves the card, chips wrap, and a body taller than its room scrolls
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJAgentFraming.h"
#include "EJAgentProtocol.h"
#include "EJAgentTools.h"
#include "EJAgentClient.h"
#include "EJAgentPanel.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace echojay::agent;

// The one named seam (declared friend in EJAgentClient.h).
struct EJAgentClientTestAccess
{
    static void setRecorder (EJAgentClient& c, std::function<void (const juce::String&, const juce::String&)> fn) { c.postOverride_ = std::move (fn); }
    static void feed (EJAgentClient& c, const juce::String& event, const juce::String& data) { c.onFrame (c.generation_, parseFrame (event, data)); }
    static void feedStale (EJAgentClient& c, const juce::String& event, const juce::String& data) { c.onFrame (c.generation_ - 1, parseFrame (event, data)); }
    static void endStream (EJAgentClient& c, bool terminal, int status, const juce::String& err) { c.onStreamEnded (c.generation_, terminal, status, err); }
};

namespace
{
int failures = 0, passes = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok  %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("FAIL  %s%s\n", what.toRawUTF8(), detail.isNotEmpty() ? (" -- " + detail).toRawUTF8() : ""); }
}
void pumpMs (double ms)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < ms)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false);
    }
}

struct FakeExecutor : ToolExecutor
{
    struct Call { juce::String name, id; };
    std::vector<Call> calls;
    std::function<ToolOutcome (const ToolCall&)> onDo;
    bool deferDo = false;
    std::vector<std::pair<ToolCall, Done>> deferred;
    PlaybackReading reading;
    int windowsBegun = 0, undoSeq = 0;
    juce::String checkpointLabel;
    std::vector<juce::String> undoneTokens;
    juce::String undoneCheckpoint;

    juce::var startContext() override { auto* o = new juce::DynamicObject(); o->setProperty ("executor", "fake"); return juce::var (o); }
    void look  (const ToolCall& c, Done done) override { calls.push_back ({ "look", c.id }); auto* o = new juce::DynamicObject(); o->setProperty ("slots", 3); done (ToolOutcome::success (juce::var (o))); }
    void check (const ToolCall& c, Done done) override { calls.push_back ({ "check", c.id }); auto* o = new juce::DynamicObject(); o->setProperty ("outLufs", -12.1); done (ToolOutcome::success (juce::var (o))); }
    void doOp  (const ToolCall& c, Done done) override
    {
        calls.push_back ({ "do", c.id });
        if (deferDo) { deferred.push_back ({ c, done }); return; }
        if (onDo) { done (onDo (c)); return; }
        auto* o = new juce::DynamicObject(); o->setProperty ("landed", true);
        done (ToolOutcome::success (juce::var (o), "u" + juce::String (++undoSeq), "landed: " + c.summary));
    }
    void beginPlaybackWindow() override { ++windowsBegun; }
    PlaybackReading readPlayback() override { return reading; }
    juce::String captureCheckpoint (const juce::String& label) override { checkpointLabel = label; return "cp_7"; }
    void undoStep (const juce::String& t, Done done) override { undoneTokens.push_back (t); done (ToolOutcome::success (juce::var())); }
    void undoToCheckpoint (const juce::String& cp, Done done) override { undoneCheckpoint = cp; done (ToolOutcome::success (juce::var())); }
};

struct Rig : EJAgentClient::Listener
{
    FakeExecutor ex;
    std::vector<std::pair<juce::String, juce::String>> posts, transcript;
    std::vector<juce::String> logs;
    juce::String token = "tok";
    EJAgentClient client;
    Rig() : client ([this] { EJAgentClient::Transport t; t.baseUrl = "http://guard.invalid/"; t.authToken = token; t.appVersion = "guard"; return t; },
                    ex, [this] (const juce::String& l) { logs.push_back (l); })
    {
        EJAgentClientTestAccess::setRecorder (client, [this] (const juce::String& p, const juce::String& b) { posts.push_back ({ p, b }); });
        client.addListener (this);
        client.playbackTickMs = 10;
    }
    ~Rig() override { client.removeListener (this); }
    void agentTranscript (const juce::String& role, const juce::String& text) override { transcript.push_back ({ role, text }); }

    void round (int n) { EJAgentClientTestAccess::feed (client, "round", "{\"sessionId\":\"ag_1\",\"round\":" + juce::String (n) + "}"); }
    void delta (const juce::String& t) { EJAgentClientTestAccess::feed (client, "delta", "{\"text\":\"" + t + "\"}"); }
    void tool (const juce::String& json) { EJAgentClientTestAccess::feed (client, "tool_call", json); }
    void await (const juce::StringArray& ids)
    {
        juce::String j = "{\"ids\":[";
        for (int i = 0; i < ids.size(); ++i) j += (i ? "," : "") + ("\"" + ids[i] + "\"");
        EJAgentClientTestAccess::feed (client, "await", j + "]}");
        EJAgentClientTestAccess::endStream (client, true, 200, {});
    }
    int assistantLines() const { int n = 0; for (auto& t : transcript) if (t.first == "assistant") ++n; return n; }
    bool logHas (const juce::String& s) const { for (auto& l : logs) if (l.contains (s)) return true; return false; }
    juce::var lastStepBody() const { return posts.empty() ? juce::var() : juce::JSON::parse (posts.back().second); }
    juce::var result (int i) const { auto b = lastStepBody(); if (auto* a = b.getProperty ("results", {}).getArray()) if (i < a->size()) return (*a)[i]; return {}; }
    const EJAgentClient::Step* step (const juce::String& id) const { for (auto& s : client.steps()) if (s.id == id) return &s; return nullptr; }
};
const char* kLook  = R"({"id":"tc_1","name":"look","args":{"what":"rack"},"approval":"free"})";
const char* kDoSet = R"({"id":"tc_2","name":"do","args":{"op":"set","slot":1,"params":{"eq_bands":[]}},"approval":"ask_first","summary":"EchoJay EQ: bell 300 Hz -2 dB Q 1.4","why":"over the 3 dB line"})";
const char* kCheck = R"({"id":"tc_3","name":"check","args":{"what":"level"},"approval":"free"})";
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_agent_client_guard");
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("agent_client_guard - the agent loop's plugin side\n\n");

    // ---- P: the protocol --------------------------------------------------------------------------------------
    {
        const auto a = parseFrame ("tool_call", kDoSet);
        const auto b = parseFrame ({}, juce::String (R"({"type":"tool_call",)") + juce::String (kDoSet).substring (1));
        check (a.kind == FrameKind::ToolCall && b.kind == FrameKind::ToolCall && a.call.id == b.call.id && a.call.summary == b.call.summary,
               "P1 a frame typed by its event line and one typed inside the JSON parse alike");
        const auto bare = parseToolCall (juce::JSON::parse (R"({"id":"x","name":"do","args":{"op":"add"}})"));
        const auto bareLook = parseToolCall (juce::JSON::parse (R"({"id":"y","name":"look","args":{}})"));
        check (bare.approval == "ask_first" && bareLook.approval == "free", "P2 a do with no approval class ASKS; a look is free (the safe default runs one way)");
        check (describeCall (a.call) == "EchoJay EQ: bell 300 Hz -2 dB Q 1.4", "P3 the checklist line is the server's summary when it sent one");
        check (describeCall (parseToolCall (juce::JSON::parse (R"({"id":"z","name":"do","args":{"op":"add","plugin":"EchoJay Compressor"}})"))) == "add EchoJay Compressor",
               "P4 ...and a derived line when it did not, so no row is blank");
        const auto w1 = parseToolCall (juce::JSON::parse (R"({"id":"w","name":"check","args":{"what":"playback","min_seconds":12}})"));
        const auto w2 = parseToolCall (juce::JSON::parse (R"({"id":"w","name":"wait_for_playback","args":{"min_seconds":12}})"));
        check (isPlaybackWait (w1) && isPlaybackWait (w2) && playbackMinSeconds (w1) == 12.0 && describeCall (w1) == "Wait for playback (12 s)",
               "P5 wait_for_playback is recognised as check(playback) and by name");
        std::vector<ToolResult> rs { okResult ("tc_1", juce::JSON::parse ("{\"slots\":3}")), declinedResult ("tc_2") };
        const auto body = juce::JSON::parse (buildStepBody ("ag_1", 3, rs));
        auto* arr = body.getProperty ("results", {}).getArray();
        check (body.getProperty ("sessionId", {}).toString() == "ag_1" && (int) body.getProperty ("round", {}) == 3 && arr != nullptr && arr->size() == 2
               && (bool) (*arr)[0].getProperty ("ok", {}) == true && (int) (*arr)[0].getProperty ("result", {}).getProperty ("slots", {}) == 3
               && (bool) (*arr)[1].getProperty ("ok", {}) == false && (*arr)[1].getProperty ("error", {}).getProperty ("code", {}).toString() == "declined",
               "P6 the step body is the plan's shape: sessionId, round, results[{id,ok,result}|{id,ok,error{code,message}}]");
        const auto start = juce::JSON::parse (buildStartBody ("make it louder", juce::var(), "1.2.3", "chat_9"));
        check (start.getProperty ("goal", {}).toString() == "make it louder" && (bool) start.getProperty ("agentMode", {}) == true
               && start.getProperty ("appVersion", {}).toString() == "1.2.3" && start.getProperty ("chatId", {}).toString() == "chat_9",
               "P7 the start body carries goal, agentMode:true (the phase-1 switch), appVersion and chatId");
        const auto err = parseFrame ("error", R"({"code":"model_unavailable","message":"busy","retryable":true})");
        check (err.kind == FrameKind::Error && err.errorCode == "model_unavailable" && err.retryable, "P8 an error frame carries code, message, retryable");
        check (parseFrame ("delta", "not json").kind == FrameKind::Unknown && parseFrame ("", "{\"x\":1}").kind == FrameKind::Unknown,
               "P9 a malformed or untyped frame is Unknown (skipped), never fatal");
    }

    // ---- N: nextRunnable ---------------------------------------------------------------------------------------
    {
        using S = EJAgentClient::Step;
        std::vector<S> st (3);
        st[0].call.name = "look"; st[0].status = S::Status::Pending; st[0].round = 1;
        st[1].call.name = "do"; st[1].call.approval = "ask_first"; st[1].status = S::Status::AwaitingApproval; st[1].round = 1;
        st[2].call.name = "check"; st[2].status = S::Status::Pending; st[2].round = 1;
        check (EJAgentClient::nextRunnable (st, 1, false) == 0, "N1 the first pending step runs first");
        st[0].status = S::Status::Done;
        check (EJAgentClient::nextRunnable (st, 1, false) == -2, "N2 an ask_first step next in line asks for the plan decision (-2), the check behind it waits");
        check (EJAgentClient::nextRunnable (st, 1, true) == 1, "N3 once the plan is submitted the same step runs");
        st[1].status = S::Status::Running;
        check (EJAgentClient::nextRunnable (st, 1, true) == -1, "N4 nothing runs while a step is running");
        st[1].status = S::Status::Done; st[2].status = S::Status::Done;
        check (EJAgentClient::nextRunnable (st, 1, true) == -1, "N5 all answered: nothing to run (the step is posted)");
    }

    // ---- R1: one round with a plan ------------------------------------------------------------------------------
    Rig r;
    {
        r.client.start ("build a mix bus chain", "chat_1");
        check (r.posts.size() == 1 && r.posts[0].first == "/api/agent/start", "R1a start POSTs /api/agent/start once", r.posts.empty() ? "no post" : r.posts[0].first);
        auto sb = juce::JSON::parse (r.posts[0].second);
        check (sb.getProperty ("goal", {}).toString() == "build a mix bus chain" && (bool) sb.getProperty ("agentMode", {})
               && sb.getProperty ("context", {}).getProperty ("executor", {}).toString() == "fake",
               "R1b ...with the goal, agentMode:true and the executor's opening context");
        check (r.ex.checkpointLabel.contains ("build a mix bus chain"), "R1c a checkpoint is captured at the session's start");
        check (r.transcript.size() == 1 && r.transcript[0].first == "user" && r.transcript[0].second == "build a mix bus chain", "R1d the goal is the user's turn in the transcript");
        r.round (1); r.delta ("Looking at the rack"); r.delta (" and the level...");
        check (r.client.liveText() == "Looking at the rack and the level...", "R1e deltas accumulate as the live text");
        r.tool (kLook); r.tool (kDoSet); r.tool (kCheck);
        check (r.client.steps().size() == 3 && r.client.steps()[1].label == "EchoJay EQ: bell 300 Hz -2 dB Q 1.4"
               && r.client.steps()[1].status == EJAgentClient::Step::Status::AwaitingApproval && r.client.steps()[0].status == EJAgentClient::Step::Status::Pending,
               "R1f three tool_call frames are three checklist rows; the ask_first one awaits approval");
        r.await ({ "tc_1", "tc_2", "tc_3" });
        pumpMs (20);
        check (r.ex.calls.size() == 1 && r.ex.calls[0].name == "look", "R1g on await the free look ran and the check behind the plan did NOT (strict wire order)",
               juce::String ((int) r.ex.calls.size()));
        check (r.client.state() == EJAgentClient::State::AwaitingApproval && r.client.planLines().size() == 1 && r.posts.size() == 1,
               "R1h the plan card is up with one line and nothing was posted");
        r.client.approveAll();
        pumpMs (20);
        check (r.ex.calls.size() == 3 && r.ex.calls[1].name == "do" && r.ex.calls[2].name == "check", "R1i Approve all runs the do, then the check");
        check (r.posts.size() == 2 && r.posts[1].first == "/api/agent/step", "R1j ONE step is posted", juce::String ((int) r.posts.size()));
        auto b = r.lastStepBody();
        auto* arr = b.getProperty ("results", {}).getArray();
        check (b.getProperty ("sessionId", {}).toString() == "ag_1" && (int) b.getProperty ("round", {}) == 1 && arr != nullptr && arr->size() == 3
               && (*arr)[0].getProperty ("id", {}).toString() == "tc_1" && (*arr)[1].getProperty ("id", {}).toString() == "tc_2" && (*arr)[2].getProperty ("id", {}).toString() == "tc_3"
               && (bool) (*arr)[0].getProperty ("ok", {}) && (bool) (*arr)[1].getProperty ("ok", {}) && (bool) (*arr)[2].getProperty ("ok", {})
               && (bool) (*arr)[1].getProperty ("result", {}).getProperty ("landed", {}),
               "R1k ...with every awaited id answered, in await order, carrying the executor's compact results");
        check (r.assistantLines() == 1 && r.transcript.back().second == "Looking at the rack and the level...", "R1l the round's talk reached the transcript exactly once");
        const auto* s2 = r.step ("tc_2");
        check (s2 != nullptr && s2->status == EJAgentClient::Step::Status::Done && s2->undoToken == "u1" && s2->detail.startsWith ("landed:"),
               "R1m the landed do carries its undo token and landed line");
        check (r.logHas ("STEP round=1 results=3") && r.logHas ("plan submitted approved=1"), "R1n the log says what was posted and what was approved");
    }
    // ---- R2: Decline ----------------------------------------------------------------------------------------------
    {
        const auto before = r.ex.calls.size();
        r.round (2);
        r.tool (R"({"id":"tc_4","name":"do","args":{"op":"add","after":2,"plugin":"EchoJay Compressor"},"approval":"ask_first","summary":"add EchoJay Compressor after slot 2"})");
        r.await ({ "tc_4" });
        pumpMs (10);
        check (r.client.state() == EJAgentClient::State::AwaitingApproval, "R2a the plan card is up");
        r.client.declineAll();
        pumpMs (10);
        auto res = r.result (0);
        check (r.ex.calls.size() == before && (bool) res.getProperty ("ok", {}) == false && res.getProperty ("error", {}).getProperty ("code", {}).toString() == "declined",
               "R2b Decline: nothing ran, the step says {ok:false, error.code:\"declined\"}");
        check (r.step ("tc_4")->status == EJAgentClient::Step::Status::Declined, "R2c the row reads declined");
    }
    // ---- R3: per-line taps ------------------------------------------------------------------------------------------
    {
        r.round (3);
        r.tool (R"({"id":"tc_5","name":"do","args":{"op":"set","slot":2},"approval":"ask_first","summary":"API-2500: ratio 4:1"})");
        r.tool (R"({"id":"tc_6","name":"do","args":{"op":"set","slot":3},"approval":"ask_first","summary":"bx_saturator: drive +2"})");
        r.await ({ "tc_5", "tc_6" });
        pumpMs (10);
        check (r.client.planLines().size() == 2, "R3a two lines on the card");
        r.client.setLineApproved ("tc_5", false);
        check (r.step ("tc_5")->approved == false && r.step ("tc_6")->approved == true, "R3b a line can be skipped on its own");
        const auto before = r.ex.calls.size();
        r.client.submitPlan();
        pumpMs (10);
        check (r.ex.calls.size() == before + 1 && r.ex.calls.back().id == "tc_6", "R3c Apply runs only the approved line");
        check (r.result (0).getProperty ("error", {}).getProperty ("code", {}).toString() == "declined" && (bool) r.result (1).getProperty ("ok", {}),
               "R3d the step keeps the wire order: declined, then ok");
    }
    // ---- R4: a failed do stops the rest of the round's do calls ------------------------------------------------------
    {
        r.ex.onDo = [] (const ToolCall& c) { return c.id == "tc_7" ? ToolOutcome::failure ("range_not_allowed", "Threshold -60 is below the map's range") : ToolOutcome::success (juce::var (new juce::DynamicObject()), "u9"); };
        r.round (4);
        r.tool (R"({"id":"tc_7","name":"do","args":{"op":"set","slot":2},"approval":"free","summary":"threshold -60"})");
        r.tool (R"({"id":"tc_8","name":"do","args":{"op":"set","slot":3},"approval":"free","summary":"drive +1"})");
        r.tool (R"({"id":"tc_9","name":"look","args":{"what":"analysis"},"approval":"free"})");
        const auto before = r.ex.calls.size();
        r.await ({ "tc_7", "tc_8", "tc_9" });
        pumpMs (10);
        check (r.ex.calls.size() == before + 2 && r.ex.calls[before].id == "tc_7" && r.ex.calls[before + 1].id == "tc_9",
               "R4a the first do ran and failed, the second do did NOT run, the look still ran");
        check (r.result (0).getProperty ("error", {}).getProperty ("code", {}).toString() == "range_not_allowed"
               && r.result (1).getProperty ("error", {}).getProperty ("code", {}).toString() == "not_run"
               && (bool) r.result (2).getProperty ("ok", {}),
               "R4b the step says which failed, which did not run, and the look's result");
        check (r.step ("tc_8")->status == EJAgentClient::Step::Status::NotRun && r.step ("tc_7")->detail.contains ("below the map"), "R4c the rows carry the error and the not-run reason");
        r.ex.onDo = nullptr;
    }
    // ---- A: talk(ask) ------------------------------------------------------------------------------------------------
    {
        r.round (5);
        r.tool (R"({"id":"tc_10","name":"talk","args":{"text":"Two EQs are on the rack.","ask":{"question":"Which one?","choices":["EchoJay EQ",{"label":"elysia museq"}]}},"approval":"free"})");
        r.await ({ "tc_10" });
        pumpMs (10);
        check (r.client.state() == EJAgentClient::State::AwaitingAsk && r.client.askPending() && r.client.pendingAsk().labels.size() == 2
               && r.client.pendingAsk().labels[1] == "elysia museq" && r.client.pendingAsk().question.contains ("Which one?"),
               "A1 the ask card waits with the question and both choices (string and object forms)");
        const auto postsBefore = r.posts.size();
        check (r.client.interceptTyped ("the museq") && r.posts.size() == postsBefore + 1, "A2 a typed answer is TAKEN as the choice and the step goes out");
        check (r.result (0).getProperty ("result", {}).getProperty ("choice", {}).toString() == "the museq" && (bool) r.result (0).getProperty ("result", {}).getProperty ("typed", {}),
               "A3 ...as {choice, typed:true}");
        check (r.transcript.back().first == "user" && r.transcript.back().second == "the museq", "A4 the answer is the user's turn in the transcript");
        r.round (6);
        r.tool (R"({"id":"tc_11","name":"talk","args":{"ask":{"question":"Louder?","choices":["Yes","No"]}},"approval":"free"})");
        r.await ({ "tc_11" });
        pumpMs (10);
        r.client.answerAsk ("No");
        check (r.result (0).getProperty ("result", {}).getProperty ("choice", {}).toString() == "No" && (bool) r.result (0).getProperty ("result", {}).getProperty ("typed", {}) == false,
               "A5 a tapped choice goes back as {choice, typed:false}");
    }
    // ---- W: wait_for_playback NEVER hangs ------------------------------------------------------------------------------
    {
        r.client.playbackTimeoutS = 1;
        r.ex.reading = {};
        r.round (7);
        r.tool (R"({"id":"tc_12","name":"check","args":{"what":"playback","min_seconds":2},"approval":"free"})");
        const auto postsBefore = r.posts.size();
        r.await ({ "tc_12" });
        pumpMs (20);
        check (r.client.state() == EJAgentClient::State::Listening && r.client.playback().active && r.client.playback().line == "Play the chorus or the loudest section."
               && r.ex.windowsBegun == 1,
               "W1a the wait begins: Listening, the prompt line, a fresh window");
        pumpMs (1400);
        check (r.posts.size() == postsBefore + 1 && (bool) r.result (0).getProperty ("result", {}).getProperty ("played", {}) == false
               && (double) r.result (0).getProperty ("result", {}).getProperty ("waited", {}) >= 1.0,
               "W1b with no audio the wait TIMES OUT and returns {played:false, waited}");
        check (r.step ("tc_12")->detail.contains ("No playback heard"), "W1c ...and says so on the row", r.step ("tc_12")->detail);
        check (r.logHas ("playback wait ends (timeout)"), "W1d the log names the ending");

        r.client.playbackTimeoutS = 10;
        r.round (8);
        r.tool (R"({"id":"tc_13","name":"check","args":{"what":"playback","min_seconds":2},"approval":"free"})");
        r.await ({ "tc_13" });
        r.ex.reading.heardAboveSeconds = 0.6f;
        pumpMs (60);
        check (r.client.playback().line.startsWith ("Listening..."), "W2a audio arriving turns the prompt into a progress line", r.client.playback().line);
        r.ex.reading.heardAboveSeconds = 2.4f; r.ex.reading.integratedLufs = -14.2f; r.ex.reading.loudestShortTermLufs = -11.9f; r.ex.reading.truePeakDbtp = -1.3f;
        pumpMs (60);
        auto res = r.result (0).getProperty ("result", {});
        check ((bool) res.getProperty ("played", {}) && std::abs ((double) res.getProperty ("seconds", {}) - 2.4) < 0.05
               && std::abs ((double) res.getProperty ("integratedLufs", {}) + 14.2) < 0.05 && std::abs ((double) res.getProperty ("loudestLufs", {}) + 11.9) < 0.05
               && std::abs ((double) res.getProperty ("peakDbtp", {}) + 1.3) < 0.05 && ! res.hasProperty ("short"),
               "W2b min_seconds heard: {played:true, seconds, integratedLufs, loudestLufs, peakDbtp}");
        check (r.step ("tc_13")->detail == "Got it, you can stop." && r.client.state() != EJAgentClient::State::Listening, "W2c the row reads \"Got it, you can stop.\" and the wait is over");

        r.round (9);
        r.tool (R"({"id":"tc_14","name":"check","args":{"what":"playback","min_seconds":8},"approval":"free"})");
        r.await ({ "tc_14" });
        r.ex.reading = {}; r.ex.reading.heardAboveSeconds = 1.5f;
        pumpMs (40);
        r.client.playbackGotIt();
        res = r.result (0).getProperty ("result", {});
        check ((bool) res.getProperty ("played", {}) && (bool) res.getProperty ("short", {}) && res.getProperty ("endedBy", {}).toString() == "user",
               "W3 the user's early stop returns what was heard, marked short");
        check (r.step ("tc_14")->detail.startsWith ("Got it - using the 1.5 s heard"), "W3b ...and the row says how much", r.step ("tc_14")->detail);

        r.round (10);
        r.tool (R"({"id":"tc_15","name":"check","args":{"what":"playback","min_seconds":8},"approval":"free"})");
        r.await ({ "tc_15" });
        r.ex.reading = {}; r.ex.reading.transportKnown = true; r.ex.reading.playing = false; r.ex.reading.heardAboveSeconds = 1.2f;
        pumpMs (2300);
        res = r.result (0).getProperty ("result", {});
        check (res.getProperty ("endedBy", {}).toString() == "transport stopped" && (bool) res.getProperty ("played", {}),
               "W4 a stopped transport after some audio ends the wait early with what was heard", res.getProperty ("endedBy", {}).toString());
        r.ex.reading = {};
    }
    // ---- S: Stop halts at once -----------------------------------------------------------------------------------------
    {
        r.ex.deferDo = true;
        r.round (11);
        r.tool (R"({"id":"tc_16","name":"do","args":{"op":"set","slot":1},"approval":"free","summary":"a slow dial"})");
        r.tool (R"({"id":"tc_17","name":"look","args":{"what":"rack"},"approval":"free"})");
        r.await ({ "tc_16", "tc_17" });
        pumpMs (10);
        check (r.step ("tc_16")->status == EJAgentClient::Step::Status::Running && r.ex.deferred.size() == 1, "S1 the do is running (the executor holds its completion)");
        const auto postsBefore = r.posts.size();
        const auto transcriptBefore = r.transcript.size();
        r.client.stop();
        check (r.client.state() == EJAgentClient::State::Stopped && ! r.client.isActive() && r.client.hasSession(), "S2 Stop -> Stopped at once (the card stays)");
        check (r.step ("tc_16")->status == EJAgentClient::Step::Status::Stopped && r.step ("tc_17")->status == EJAgentClient::Step::Status::Stopped,
               "S3 every unfinished step is marked Stopped");
        check (r.transcript.size() == transcriptBefore + 1 && r.transcript.back().second == "Stopped.", "S4 \"Stopped.\" reaches the transcript");
        auto held = r.ex.deferred.back(); r.ex.deferred.clear();
        held.second (ToolOutcome::success (juce::var (new juce::DynamicObject()), "u_late"));
        EJAgentClientTestAccess::feedStale (r.client, "round", "{\"sessionId\":\"ag_1\",\"round\":12}");
        pumpMs (30);
        check (r.posts.size() == postsBefore && r.step ("tc_16")->status == EJAgentClient::Step::Status::Stopped && r.client.round() == 11,
               "S5 a late completion and a stale frame change NOTHING: no step posted, the row stays Stopped");
        check (r.logHas ("STOP requested"), "S6 the log records the stop with the state it interrupted");
        check (r.client.undoAllowedNow() && r.client.canUndoAll(), "S7 Undo is reachable after a stop");
        r.ex.deferDo = false;
    }
    // ---- U: Undo ----------------------------------------------------------------------------------------------------------
    {
        r.client.undoStep ("tc_2");
        pumpMs (10);
        check (r.ex.undoneTokens.size() == 1 && r.ex.undoneTokens[0] == "u1" && r.step ("tc_2")->status == EJAgentClient::Step::Status::Undone && r.step ("tc_2")->detail == "undone",
               "U1 Undo on a step sends THAT step's token and the row reads undone");
        check (r.client.canUndoAll(), "U2 another landed do remains, so Undo all is still offered");
        r.client.undoAll();
        pumpMs (10);
        check (r.ex.undoneCheckpoint == "cp_7" && ! r.client.canUndoAll() && r.client.notice().contains ("undone"), "U3 Undo all sends the session's checkpoint; every landed row is undone; a notice says so");
    }
    // ---- E: errors, retry, done, dismiss -----------------------------------------------------------------------------------
    {
        r.client.start ("cut 2 dB at 300 Hz", "chat_1");
        check (r.client.steps().empty() && r.client.state() == EJAgentClient::State::Connecting && r.posts.back().first == "/api/agent/start", "E1 a new session starts clean");
        r.round (1);
        EJAgentClientTestAccess::feed (r.client, "error", R"({"code":"model_unavailable","message":"The model is busy.","retryable":true})");
        check (r.client.state() == EJAgentClient::State::Failed && r.client.errorRetryable() && r.client.errorMessage() == "The model is busy."
               && r.transcript.back().second.contains ("The model is busy."),
               "E2 an error frame -> Failed, retryable, the message in the transcript");
        const auto lastBody = r.posts.back().second; const auto lastPath = r.posts.back().first; const auto n = r.posts.size();
        r.client.retry();
        check (r.posts.size() == n + 1 && r.posts.back().first == lastPath && r.posts.back().second == lastBody, "E3 Retry re-posts the SAME request, byte for byte");
        r.round (1); r.delta ("Done: the EQ took a 2 dB cut at 300 Hz.");
        EJAgentClientTestAccess::feed (r.client, "done", R"({"summary":"Cut 2 dB at 300 Hz on the EchoJay EQ.","rounds":2,"costUsd":0.04})");
        check (r.client.state() == EJAgentClient::State::Done && r.client.round() == 2 && std::abs (r.client.costUsd() - 0.04) < 1e-9
               && r.transcript.back().second == "Cut 2 dB at 300 Hz on the EchoJay EQ." && r.transcript[r.transcript.size() - 2].second == "Done: the EQ took a 2 dB cut at 300 Hz.",
               "E4 done -> Done; the round's talk and then the summary reach the transcript");
        check (r.client.statusLine().startsWith ("Done"), "E5 the status line reads Done", r.client.statusLine());
        r.client.dismiss();
        check (r.client.state() == EJAgentClient::State::Idle && ! r.client.hasSession() && r.client.steps().empty(), "E6 dismiss -> Idle, nothing to show");

        r.client.start ("x", "chat_1");
        EJAgentClientTestAccess::endStream (r.client, false, 502, "The server answered 502.");
        check (r.client.state() == EJAgentClient::State::Failed && r.client.errorRetryable() && r.client.errorMessage().contains ("502"), "E7 a stream that ends with no terminal frame -> Failed (retryable on a 5xx)");
        r.client.dismiss();
        r.token.clear();
        r.client.start ("x", "chat_1");
        check (r.client.state() == EJAgentClient::State::Failed && r.client.errorMessage().contains ("Sign in") && ! r.client.errorRetryable(), "E8 no token -> \"Sign in\", not a request");
        r.token = "tok";
        r.client.dismiss();
    }
    // ---- T: typed under a running agent ---------------------------------------------------------------------------------------
    {
        r.client.start ("build a vocal chain", "chat_1");
        r.round (1);
        check (r.client.interceptTyped ("also add a de-esser") && r.client.notice().contains ("still working"), "T1 a typed message while the agent runs is refused WITH a line");
        r.client.stop();
        check (! r.client.interceptTyped ("hello"), "T2 ...and goes through once the agent is not running");
        r.client.dismiss();
    }
    // ---- L: the panel at every editor size ------------------------------------------------------------------------------------
    {
        EJAgentPanel panel (r.client);
        int layoutNeeded = 0;
        panel.onLayoutNeeded = [&] { ++layoutNeeded; };
        const int widths[] = { 380, 420, 600, 900, 1240, 1780 };
        const int caps[]   = { 160, 300, 600, 1 << 20 };
        auto sweep = [&] (const juce::String& scene)
        {
            int bad = 0; juce::String firstBad;
            for (int w : widths) for (int cap : caps)
            {
                const auto L = panel.computeLayout (w, cap);
                const auto v = EJAgentPanel::checkLayout (L);
                if (v.isNotEmpty() || L.totalHeight > cap || panel.preferredHeight (w, cap) != L.totalHeight)
                { if (++bad == 1) firstBad = "w=" + juce::String (w) + " cap=" + juce::String (cap) + ": " + (v.isNotEmpty() ? v : juce::String ("height over the cap")); }
                panel.setSize (w, L.totalHeight);
                for (auto* child : panel.getChildren())
                    if (child->isVisible() && ! panel.getLocalBounds().contains (child->getBounds()))
                    { if (++bad == 1) firstBad = "w=" + juce::String (w) + " cap=" + juce::String (cap) + ": a child leaves the panel"; }
            }
            check (bad == 0, "L " + scene + ": every width 380..1780 x every height cap passes checkLayout, fits the cap, and no button leaves the card", firstBad);
        };
        // scene 1: the plan card with a long summary, a notice and live text
        r.client.start ("build a mix bus chain for a hip-hop record that keeps the dynamics but lands at -12 LUFS integrated with a -0.1 dBTP ceiling", "chat_1");
        r.round (1);
        r.delta ("I will read the rack and the analysis first, then propose a corrective EQ, glue compression, saturation, a level trim and a limiter at -0.1 dBTP.");
        r.tool (kLook);
        r.tool (R"({"id":"tc_2","name":"do","args":{"op":"build"},"approval":"ask_first","summary":"Build: EchoJay EQ (bell 300 Hz -2 dB Q 1.4, high shelf 10 kHz +1 dB), API-2500 (s) ratio 4:1 attack 10 ms release 300 ms, bx_saturator V2 drive 2 dB, EchoJay Level option dynamic target -12 LUFS, EchoJay Limiter ceiling -0.1 dBTP","why":"a build needs your ok"})");
        r.tool (R"({"id":"tc_3","name":"do","args":{"op":"set_pre_gain","db":-6},"approval":"ask_first","summary":"pre-gain -6 dB"})");
        r.await ({ "tc_1", "tc_2", "tc_3" });
        pumpMs (10);
        r.client.interceptTyped ("wait");
        check (r.client.state() == EJAgentClient::State::AwaitingApproval, "L0 scene 1 is the plan card");
        sweep ("plan card + long summary + notice");
        check (layoutNeeded > 0, "L1 the panel asked the editor to re-dock when its height changed");
        // scene 2: the ask card with many chips
        r.client.approveAll(); pumpMs (10);
        r.round (2);
        r.tool (R"({"id":"tc_4","name":"talk","args":{"ask":{"question":"How loud should the mix bus land?","choices":["Commercial (-8 LUFS)","Pushed (-7)","Leave dynamics, keep punch and breathing room (-12)","Match the input","Something else","Skip this"]}},"approval":"free"})");
        r.await ({ "tc_4" }); pumpMs (10);
        check (r.client.askPending(), "L2 scene 2 is the ask card");
        sweep ("ask card with six chips");
        {
            const auto L = panel.computeLayout (380, 1 << 20);
            check (L.chipRects.size() == 6 && L.chipRects.back().getY() > L.chipRects.front().getY(), "L3 at 380 px the six chips wrap into more than one row");
        }
        r.client.answerAsk ("Pushed (-7)");
        // scene 3: listening
        r.round (3);
        r.tool (R"({"id":"tc_5","name":"check","args":{"what":"playback","min_seconds":8},"approval":"free"})");
        r.await ({ "tc_5" }); pumpMs (10);
        check (r.client.state() == EJAgentClient::State::Listening, "L4 scene 3 is the listening row");
        sweep ("listening row");
        r.client.playbackGotIt();
        // scene 4: done with undoable rows
        EJAgentClientTestAccess::feed (r.client, "done", R"({"summary":"Built and landed.","rounds":3,"costUsd":0.2})");
        check (r.client.state() == EJAgentClient::State::Done && r.client.canUndoAll(), "L5 scene 4 is Done with Undo all offered");
        sweep ("done with Undo per row and Undo all");
        {
            const auto L = panel.computeLayout (380, 160);
            check (L.scrolls && L.body.getHeight() < L.contentHeight && L.totalHeight == 160, "L6 a short cap makes the body SCROLL rather than cut anything");
            const auto full = panel.computeLayout (380, 1 << 20);
            check (! full.scrolls && full.body.getHeight() == full.contentHeight, "L7 with room, the body is exactly its content");
        }
        r.client.dismiss();
        check (! panel.shouldShow(), "L8 after dismiss the panel asks to be hidden");
    }

    std::printf ("\n==== agent_client_guard: %s (%d ok, %d failed) ====\n", failures == 0 ? "GREEN" : "RED", passes, failures);
    return failures == 0 ? 0 : 1;
}
