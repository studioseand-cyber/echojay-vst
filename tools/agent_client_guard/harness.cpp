// agent_client_guard - THE AGENT LOOP'S PLUGIN SIDE, in-process, no network, no Logic (Session A2, 9 Oct 2026).
//
// The client (EJAgentClient) is driven through a recorder in place of the socket and a fake executor in place of
// A's; frames are fed as the wire would deliver them (EJAgentProtocol::parseFrame over the plan's 1.3 examples).
// Every leg below is a sentence from AGENT_MODE_PLAN.md or from Sean's brief, and each one is RED without the
// behaviour it names:
//   P   the protocol (CONTRACT_AGENT_TOOLS.md 2-4): frames are typed by the event line ONLY; a do with no approval
//       class asks; the step / start / stop bodies; the plan frame; talk(ask) choices with detail + allowFreeText;
//       the playback timeout answer verbatim; open_editor's line
//   N   nextRunnable: free calls run before the card is shown; then the plan; approved lines in wire order
//   R1  one round: free calls run, the plan frame raises the card with its heading, Apply all runs it, ONE step is
//       posted with every awaited id answered in await order, the round's talk reaches the transcript ONCE
//   R2  Skip all: the skipped call returns {ok:false, error:{code:"approval_declined"}} and nothing ran
//   R3  per-line taps: a skipped line is approval_declined, the applied one runs, order preserved
//   R4  a failed do stops the rest of the round's do calls (not_run); a look after it still runs
//   A   talk(ask): the ask card waits; a tap answers {tapped}, a typed answer {typed}; allowFreeText:false refuses
//       typing WITH a line; the soft cap (tc_keep_going) is an ordinary ask
//   W1  wait_for_playback with no audio TIMES OUT and returns {played:false, waited:60, sentence} - it never hangs
//   W2  min_seconds of audio -> "Got it, you can stop." and the reading goes back
//   W3  the user's early stop returns what was heard, marked short
//   W4  a stopped transport after some audio ends the wait early
//   S   Stop halts at once: POST /api/agent/stop {sessionId} goes out, the running step is marked Stopped, NO step
//       is posted, a late completion and a stale frame are ignored, "Stopped." reaches the transcript, Undo is
//       reachable afterwards
//   U   Undo per step sends that step's token; Undo all sends the session's checkpoint; rows read "undone"
//   E   an error frame -> Failed (retryable keeps the body for Retry, which re-posts it byte for byte); a stream that
//       ends with no terminal frame -> Failed; no token -> "Sign in"; done -> Done with its reason, summary in the
//       transcript; dismiss
//   T   a typed message under a running agent is refused WITH a line
//   L   the panel: at every editor width (380..1780) and height cap, no text row is shorter than its wrapped text,
//       no button overlaps text or leaves the card, chips wrap, and a body taller than its room scrolls
//   X   the CARD AND THE CHAT, from Sean's 12:05 Logic session (18 Oct 2026): a new run is new from the instant
//       Send is pressed (runSerial, not the sessionId, which is empty in exactly that window); a question's line is
//       the question, never the server's bare summary "ask"; an answer is carried onto the ask it answered even
//       when the server reuses one tool id for two asks; and a card carrying his 4,000-character compressor list
//       fits its rects at every column width, including one narrower than the old 220 px floor
//   V   the run ROW at the instant of Send (18 Oct 2026): a live run with nothing back from the server yet still
//       lays out, with item 9's "Thinking..." line and its Stop - because a row that lays out to nothing is the
//       blank avatar Sean read in Logic. And the other direction: a real step replaces that line, and a run that
//       is not live never carries it.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJAgentFraming.h"
#include "EJAgentProtocol.h"
#include "EJAgentTools.h"
#include "EJAgentClient.h"
#include "EJAgentExecutorRead.h"   // SZ2/SZ3 call the product's own inventoryPage
#include "EJAgentPanel.h"
#include "EJAgentRunView.h"    // V: the run ROW, whose geometry is what Sean reads in Logic
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

    // ITEM 2 (13 Oct 2026): the client asks the executor to name a cross-channel target so the plan card can
    // say "on 88_7even Kick_2_01". The fake answers for one uid, which is all the label legs need.
    juce::String otherChannelName = "88_7even Kick_2_01";
    juce::String describeOtherChannel (const ToolCall& c) override
    { return argString (c, "channel") == "bb411b8240" ? otherChannelName : juce::String(); }

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
    void plan (const juce::String& json) { EJAgentClientTestAccess::feed (client, "plan", json); }
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
        check (a.kind == FrameKind::ToolCall && a.call.id == "tc_2" && a.call.summary == "EchoJay EQ: bell 300 Hz -2 dB Q 1.4" && b.kind == FrameKind::Unknown,
               "P1 a frame is typed by its event line ONLY; a \"type\" field inside the JSON is not a type (contract 4)");
        check (isTerminalFrame (FrameKind::Await) && isTerminalFrame (FrameKind::Done) && isTerminalFrame (FrameKind::Error)
               && ! isTerminalFrame (FrameKind::Plan) && ! isTerminalFrame (FrameKind::ToolCall),
               "P1b await, done and error are the terminal frames; plan and tool_call are not");
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
               && (bool) (*arr)[1].getProperty ("ok", {}) == false && (*arr)[1].getProperty ("error", {}).getProperty ("code", {}).toString() == "approval_declined",
               "P6 the step body is the contract's shape: sessionId, round, results[{id,ok,result}|{id,ok,error{code,message}}]; Skip is approval_declined");
        check (juce::JSON::parse (buildStopBody ("ag_1")).getProperty ("sessionId", {}).toString() == "ag_1", "P6b the stop body is {sessionId}");
        const auto start = juce::JSON::parse (buildStartBody ("make it louder", juce::var(), "1.2.3", "chat_9"));
        check (start.getProperty ("goal", {}).toString() == "make it louder" && (bool) start.getProperty ("agentMode", {}) == true
               && start.getProperty ("appVersion", {}).toString() == "1.2.3" && start.getProperty ("chatId", {}).toString() == "chat_9",
               "P7 the start body carries goal, agentMode:true (the phase-1 switch), appVersion and chatId");
        const auto err = parseFrame ("error", R"({"code":"model_unavailable","message":"busy","retryable":true})");
        check (err.kind == FrameKind::Error && err.errorCode == "model_unavailable" && err.retryable, "P8 an error frame carries code, message, retryable");
        check (parseFrame ("delta", "not json").kind == FrameKind::Unknown && parseFrame ("", "{\"x\":1}").kind == FrameKind::Unknown
               && parseFrame ("whatever", "{}").kind == FrameKind::Unknown,
               "P9 a malformed, unnamed or unknown-named frame is Unknown (skipped), never fatal");
        const auto pl = parseFrame ("plan", R"({"round":3,"heading":"Here is the plan.","items":[{"id":"tc_9","summary":"add EchoJay Compressor after slot 2","why":"adding a plugin needs your ok","approval":"ask_first"},{"id":"tc_10","summary":"pre-gain -6 dB"}]})");
        check (pl.kind == FrameKind::Plan && pl.planRound == 3 && pl.planHeading == "Here is the plan." && pl.planItems.size() == 2
               && pl.planItems[0].id == "tc_9" && pl.planItems[0].why == "adding a plugin needs your ok" && pl.planItems[1].approval == "ask_first",
               "P10 the plan frame: round, heading, items[{id, summary, why?, approval}] (approval defaults to ask_first)");
        const auto rd = parseFrame ("round", R"({"sessionId":"ag_x","round":4,"ofSoftCap":20})");
        const auto aw = parseFrame ("await", R"({"ids":["a","b"],"timeoutMs":120000})");
        const auto dn = parseFrame ("done", R"({"reason":"hard_cap","summary":"s","rounds":40,"costUsd":1.5})");
        check (rd.ofSoftCap == 20 && aw.awaitTimeoutMs == 120000 && aw.awaitIds.size() == 2 && dn.doneReason == "hard_cap" && dn.rounds == 40,
               "P11 round.ofSoftCap, await.timeoutMs and done.reason are read");
        const auto ask = talkAskOf (parseToolCall (juce::JSON::parse (R"({"id":"tc_keep_going","name":"talk","args":{"text":"Twenty rounds in.","ask":{"question":"Keep going?","choices":[{"label":"Yes","detail":"another 20 rounds","intent":"continue"},{"label":"Stop here"}],"allowFreeText":false}}})")));
        check (ask.present && ask.question == "Keep going?" && ask.labels.size() == 2 && ask.details[0] == "another 20 rounds" && ask.intents[0] == "continue"
               && ask.details[1].isEmpty() && ask.allowFreeText == false,
               "P12 talk(ask): choices [{label, detail?, intent?}] and allowFreeText are read; the soft cap is an ordinary ask");
        check (talkAskOf (parseToolCall (juce::JSON::parse (R"({"id":"t","name":"talk","args":{"ask":{"question":"Q","choices":["A"]}}})"))).allowFreeText == true,
               "P12b allowFreeText absent -> typing allowed");
        check (askAnswerVar ("Yes", false).getProperty ("tapped", {}).toString() == "Yes" && ! askAnswerVar ("Yes", false).hasProperty ("typed")
               && askAnswerVar ("the museq", true).getProperty ("typed", {}).toString() == "the museq" && ! askAnswerVar ("x", true).hasProperty ("tapped"),
               "P13 the answer is {tapped:\"<label>\"} or {typed:\"<text>\"}, never both");
        const auto to = playbackTimeoutResult (60);
        check ((bool) to.getProperty ("played", {}) == false && (int) to.getProperty ("waited", {}) == 60
               && to.getProperty ("sentence", {}).toString() == "Nothing played in 60 seconds - play the loudest section and ask me again.",
               "P14 the playback timeout answer is the contract's, verbatim");
        check (describeCall (parseToolCall (juce::JSON::parse (R"({"id":"o","name":"do","args":{"op":"open_editor","slot":3},"approval":"free"})"))) == "Open the editor for slot 3",
               "P15 open_editor {slot} is a free do with its own line");
    }

    // ---- N: nextRunnable ---------------------------------------------------------------------------------------
    {
        using S = EJAgentClient::Step;
        std::vector<S> st (3);
        st[0].call.name = "look"; st[0].status = S::Status::Pending; st[0].round = 1;
        st[1].call.name = "do"; st[1].call.approval = "ask_first"; st[1].status = S::Status::AwaitingApproval; st[1].round = 1;
        st[2].call.name = "check"; st[2].status = S::Status::Pending; st[2].round = 1;
        check (EJAgentClient::nextRunnable (st, 1, false) == 0, "N1 the first free step runs first");
        st[0].status = S::Status::Done;
        check (EJAgentClient::nextRunnable (st, 1, false) == 2, "N2 the free check BEHIND the ask_first step runs before the card is shown (contract 3)");
        st[2].status = S::Status::Done;
        check (EJAgentClient::nextRunnable (st, 1, false) == -2, "N3 with every free call done, the undecided ask_first step asks for the plan decision (-2)");
        check (EJAgentClient::nextRunnable (st, 1, true) == 1, "N4 once the plan is submitted the same step runs");
        st[1].status = S::Status::Running;
        check (EJAgentClient::nextRunnable (st, 1, true) == -1, "N5 nothing runs while a step is running");
        st[1].status = S::Status::Done;
        check (EJAgentClient::nextRunnable (st, 1, true) == -1, "N6 all answered: nothing to run (the step is posted)");
    }

    // ---- V: THE ROW AT THE INSTANT OF SEND (18 Oct 2026, Sean in Logic) ---------------------------------------
    //
    // *"I press Enter on the Kick's Link and see only a blank 'E' avatar for several seconds before the answer
    // appears."* The row is opened by AgentRunBridge::agentChanged, and EJAgentClient::post() calls changed()
    // from INSIDE start() - so the row is born while the client is Connecting, with no sessionId, no steps and
    // no closing. RunView::empty() was true there, layoutRun returned a zero height, and the assistant message
    // that carries the row drew its avatar and nothing else. These legs are that moment, and they are RED
    // without the waiting line: `empty()` true, totalHeight 0.
    {
        using namespace echojay::agentui;
        using St = EJAgentClient::Step::Status;
        Rig vr;
        vr.client.start ("make the kick hit harder", "chat_v");
        check (vr.client.state() == EJAgentClient::State::Connecting,
               "V0 PRECONDITION: start() leaves the client Connecting - the state the row is born in");
        const auto atSend = runViewFrom (vr.client, {}, true);
        const auto L = layoutRun (atSend, 0, 0, 420);
        check (! atSend.empty() && L.totalHeight > 0,
               "V1 the row LAYS OUT the moment the run exists: a live run with nothing back yet is not an empty "
               "run, and a card of zero height is the blank avatar he read",
               juce::String ("empty=") + (atSend.empty() ? "yes" : "no") + " h=" + juce::String (L.totalHeight));
        check (atSend.steps.size() == 1
               && atSend.steps[0].label == juce::String::fromUTF8 ("Thinking\xe2\x80\xa6")
               && atSend.steps[0].status == (int) St::Running,
               "V2 ...carrying item 9's own word, on a Running line so the mockup's glyph pulses - one indicator, "
               "the same one the stage row was showing a frame earlier",
               atSend.steps.empty() ? juce::String ("no steps") : atSend.steps[0].label);
        check (! L.rows.empty() && ! L.statusText.isEmpty() && atSend.statusWord == "Working" && atSend.showStop,
               "V3 ...and the header's pulsing status and Stop are on screen with it, so a slow round can be "
               "abandoned instead of waited out");
        bool pressable = false;
        for (const auto& z : L.zones) if (z.kind == RunHit::Stop) pressable = true;
        check (pressable, "V3b ...Stop as a REAL zone, not a drawn word (the dead-Details fault of item 3)");

        // THE OTHER DIRECTION, 1: a real step takes the line's place. The waiting line cannot outlive the
        // thing it stands in for, which is what makes it safe to put in the snapshot at all.
        vr.round (1);
        vr.tool (kLook);
        const auto withStep = runViewFrom (vr.client, {}, true);
        check (withStep.steps.size() == 1
               && withStep.steps[0].label != juce::String::fromUTF8 ("Thinking\xe2\x80\xa6"),
               "V4 the first real step REPLACES the waiting line - it is never a second row beside the work",
               withStep.steps.empty() ? juce::String ("no steps") : withStep.steps[0].label);

        // THE OTHER DIRECTION, 2: a run that is over carries no waiting line, so nothing pulses on a dead row.
        Rig done;
        done.client.start ("tell me the level", "chat_v2");
        EJAgentClientTestAccess::feed (done.client, "done", R"({"reason":"complete","rounds":1,"costUsd":0.0})");
        const auto after = runViewFrom (done.client, {}, true);
        check (! after.running && after.steps.empty() && after.empty()
               && layoutRun (after, 0, 0, 420).totalHeight == 0,
               "V5 a run that is NOT live has no waiting line and lays out to nothing, exactly as before - the "
               "change is additive to the live case only",
               juce::String ("steps=") + juce::String ((int) after.steps.size())
                 + " running=" + (after.running ? "yes" : "no"));
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
        r.plan (R"({"round":1,"heading":"One change needs your ok.","items":[{"id":"tc_2","summary":"EchoJay EQ: bell 300 Hz -2 dB Q 1.4","why":"over the 3 dB line","approval":"ask_first"}]})");
        check (r.client.steps().size() == 3 && r.client.steps()[1].label == "EchoJay EQ: bell 300 Hz -2 dB Q 1.4"
               && r.client.steps()[1].status == EJAgentClient::Step::Status::AwaitingApproval && r.client.steps()[0].status == EJAgentClient::Step::Status::Pending
               && r.client.planHeading() == "One change needs your ok.",
               "R1f three tool_call frames are three checklist rows; the plan frame marks its item and carries the heading");
        r.await ({ "tc_1", "tc_2", "tc_3" });
        pumpMs (20);
        check (r.ex.calls.size() == 2 && r.ex.calls[0].name == "look" && r.ex.calls[1].name == "check",
               "R1g on await every FREE call ran (the look, and the check behind the plan line) before the card is shown", juce::String ((int) r.ex.calls.size()));
        check (r.client.state() == EJAgentClient::State::AwaitingApproval && r.client.planLines().size() == 1 && r.posts.size() == 1,
               "R1h the plan card is up with one line and nothing was posted");
        r.client.approveAll();
        pumpMs (20);
        check (r.ex.calls.size() == 3 && r.ex.calls[2].name == "do" && r.ex.calls[2].id == "tc_2", "R1i Apply all runs the do");
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
        check (r.ex.calls.size() == before && (bool) res.getProperty ("ok", {}) == false && res.getProperty ("error", {}).getProperty ("code", {}).toString() == "approval_declined",
               "R2b Skip all: nothing ran, the step says {ok:false, error.code:\"approval_declined\"}");
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
        check (r.result (0).getProperty ("error", {}).getProperty ("code", {}).toString() == "approval_declined" && (bool) r.result (1).getProperty ("ok", {}),
               "R3d the step keeps the wire order: approval_declined, then ok");
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
        r.tool (R"({"id":"tc_10","name":"talk","args":{"text":"Two EQs are on the rack.","ask":{"question":"Which one?","choices":[{"label":"EchoJay EQ","detail":"slot 1"},{"label":"elysia museq","detail":"slot 4"}],"allowFreeText":true}},"approval":"free"})");
        r.await ({ "tc_10" });
        pumpMs (10);
        check (r.client.state() == EJAgentClient::State::AwaitingAsk && r.client.askPending() && r.client.pendingAsk().labels.size() == 2
               && r.client.pendingAsk().labels[1] == "elysia museq" && r.client.pendingAsk().details[1] == "slot 4" && r.client.pendingAsk().question.contains ("Which one?"),
               "A1 the ask card waits with the question and both choices with their detail");
        const auto postsBefore = r.posts.size();
        check (r.client.interceptTyped ("the museq") && r.posts.size() == postsBefore + 1, "A2 a typed answer is TAKEN (allowFreeText) and the step goes out");
        check (r.result (0).getProperty ("result", {}).getProperty ("typed", {}).toString() == "the museq" && ! r.result (0).getProperty ("result", {}).hasProperty ("tapped"),
               "A3 ...as {typed:\"<text>\"}");
        check (r.transcript.back().first == "user" && r.transcript.back().second == "the museq", "A4 the answer is the user's turn in the transcript");
        // A FRESH ASK FOR THE TAP LEGS. The run above was stopped by the new instruction - which is
        // the point of it - so the next two legs need a question of their own rather than asserting
        // against a client that has correctly finished.
        r.round (7);
        // ---- ITS OWN ID (16 Oct 2026) --------------------------------------------------------
        // I added this ask this morning and gave it `tc_12`, which the playback leg below already
        // owns. stepById returns the FIRST match, so the playback step's completion was dropped on
        // `s->isFinal()` against THIS one: the wait worked, the row read right, and nothing was ever
        // posted ("posts 8->8"). An id in this harness is a name, and two legs cannot share one.
        r.tool (R"({"id":"tc_11b","name":"talk","args":{"ask":{"question":"Louder?","choices":[{"label":"Yes"},{"label":"No"}],"allowFreeText":false}},"approval":"free"})");
        r.await ({ "tc_11b" });
        pumpMs (10);
        const auto p3 = r.posts.size();
        r.client.answerAsk ("Perhaps");
        check (r.client.askPending() && r.posts.size() == p3, "A5b a tap that is not one of the offered labels is ignored");
        r.client.answerAsk ("No");
        check (r.result (0).getProperty ("result", {}).getProperty ("tapped", {}).toString() == "No" && ! r.result (0).getProperty ("result", {}).hasProperty ("typed"),
               "A6 a tapped choice goes back as {tapped:\"<label>\"}");
        r.round (7);
        r.tool (R"({"id":"tc_keep_going","name":"talk","args":{"text":"Twenty rounds in.","ask":{"question":"Keep going?","choices":[{"label":"Yes"},{"label":"Stop here"}],"allowFreeText":false}},"approval":"free"})");
        r.await ({ "tc_keep_going" });
        pumpMs (10);
        r.client.answerAsk ("Yes");
        check (r.result (0).getProperty ("id", {}).toString() == "tc_keep_going" && r.result (0).getProperty ("result", {}).getProperty ("tapped", {}).toString() == "Yes"
               && r.logHas ("[soft cap]"),
               "A7 the soft cap is an ordinary ask with id tc_keep_going; a tap answers it like any other");

    }
    // ---- W: wait_for_playback NEVER hangs ------------------------------------------------------------------------------
    {
        r.client.playbackTimeoutS = 1;
        r.ex.reading = {};
        r.round (8);
        r.tool (R"({"id":"tc_12","name":"check","args":{"what":"playback","min_seconds":2},"approval":"free"})");
        const auto postsBefore = r.posts.size();
        r.await ({ "tc_12" });
        pumpMs (20);
        check (r.client.state() == EJAgentClient::State::Listening && r.client.playback().active && r.client.playback().line == "Play the chorus or the loudest section."
               && r.ex.windowsBegun == 1,
               "W1a the wait begins: Listening, the prompt line, a fresh window");
        pumpMs (1400);
        check (r.posts.size() == postsBefore + 1 && (bool) r.result (0).getProperty ("result", {}).getProperty ("played", {}) == false
               && (int) r.result (0).getProperty ("result", {}).getProperty ("waited", {}) == 1
               && r.result (0).getProperty ("result", {}).getProperty ("sentence", {}).toString() == "Nothing played in 60 seconds - play the loudest section and ask me again.",
               "W1b with no audio the wait TIMES OUT and returns {played:false, waited:<timeout>, sentence} - the contract's sentence verbatim",
               "posts " + juce::String ((int) postsBefore) + "->" + juce::String ((int) r.posts.size())
               + " last=" + (r.posts.empty() ? juce::String ("-") : r.posts.back().first));
        check (r.step ("tc_12")->detail == "Nothing played in 60 seconds - play the loudest section and ask me again.", "W1c ...and the row says the same sentence", r.step ("tc_12")->detail);
        check (r.logHas ("playback wait ends (timeout)"), "W1d the log names the ending");

        r.client.playbackTimeoutS = 10;
        r.round (9);
        r.tool (R"({"id":"tc_13","name":"check","args":{"what":"playback","min_seconds":2},"approval":"free"})");
        r.await ({ "tc_13" });
        r.ex.reading.heardAboveSeconds = 0.6f;
        pumpMs (60);
        check (r.client.playback().line.startsWith ("Listening..."), "W2a audio arriving turns the prompt into a progress line", r.client.playback().line);
        // ---- PB1 (19 Oct 2026, Sean at 11:06): THE PROGRESS LINE HAS ONE SURFACE -------------
        // *"'Listening... 3 of 8 s' draws twice (step + line under it)."*  It did: the tick wrote
        // the progress text into the STEP's detail as well as into playback().line, and the card
        // draws both - the row, and the playback block with its ring. The block owns it while the
        // wait is live; the step's detail is written once, at the end, with the outcome.
        check (! r.step ("tc_13")->detail.startsWith ("Listening..."),
               "PB1 the progress line is NOT also written into the step's detail, so the card cannot "
               "draw it twice  (RED as it stood: the row and the block both said \"Listening... N of M s\")",
               "step detail: \"" + r.step ("tc_13")->detail + "\"");
        r.ex.reading.heardAboveSeconds = 2.4f; r.ex.reading.integratedLufs = -14.2f; r.ex.reading.loudestShortTermLufs = -11.9f; r.ex.reading.truePeakDbtp = -1.3f;
        pumpMs (60);
        auto res = r.result (0).getProperty ("result", {});
        check ((bool) res.getProperty ("played", {}) && std::abs ((double) res.getProperty ("seconds", {}) - 2.4) < 0.05
               && std::abs ((double) res.getProperty ("integratedLufs", {}) + 14.2) < 0.05 && std::abs ((double) res.getProperty ("loudestLufs", {}) + 11.9) < 0.05
               && std::abs ((double) res.getProperty ("peakDbtp", {}) + 1.3) < 0.05 && ! res.hasProperty ("short"),
               "W2b min_seconds heard: {played:true, seconds, integratedLufs, loudestLufs, peakDbtp}");
        check (r.step ("tc_13")->detail == "Got it, you can stop." && r.client.state() != EJAgentClient::State::Listening, "W2c the row reads \"Got it, you can stop.\" and the wait is over");

        r.round (10);
        r.tool (R"({"id":"tc_14","name":"check","args":{"what":"playback","min_seconds":8},"approval":"free"})");
        r.await ({ "tc_14" });
        r.ex.reading = {}; r.ex.reading.heardAboveSeconds = 1.5f;
        pumpMs (40);
        r.client.playbackGotIt();
        res = r.result (0).getProperty ("result", {});
        check ((bool) res.getProperty ("played", {}) && (bool) res.getProperty ("short", {}) && res.getProperty ("endedBy", {}).toString() == "user",
               "W3 the user's early stop returns what was heard, marked short");
        check (r.step ("tc_14")->detail.startsWith ("Got it - using the 1.5 s heard"), "W3b ...and the row says how much", r.step ("tc_14")->detail);

        r.round (11);
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
        r.round (12);
        r.tool (R"({"id":"tc_16","name":"do","args":{"op":"set","slot":1},"approval":"free","summary":"a slow dial"})");
        r.tool (R"({"id":"tc_17","name":"look","args":{"what":"rack"},"approval":"free"})");
        r.await ({ "tc_16", "tc_17" });
        pumpMs (10);
        check (r.step ("tc_16")->status == EJAgentClient::Step::Status::Running && r.ex.deferred.size() == 1, "S1 the do is running (the executor holds its completion)");
        const auto postsBefore = r.posts.size();
        const auto transcriptBefore = r.transcript.size();
        r.client.stop();
        check (r.client.state() == EJAgentClient::State::Stopped && ! r.client.isActive() && r.client.hasSession(), "S2 Stop -> Stopped at once (the card stays)");
        check (r.posts.size() == postsBefore + 1 && r.posts.back().first == "/api/agent/stop"
               && juce::JSON::parse (r.posts.back().second).getProperty ("sessionId", {}).toString() == "ag_1",
               "S2b ...and POST /api/agent/stop {sessionId} went out, and NO step");
        check (r.step ("tc_16")->status == EJAgentClient::Step::Status::Stopped && r.step ("tc_17")->status == EJAgentClient::Step::Status::Stopped,
               "S3 every unfinished step is marked Stopped");
        check (r.transcript.size() == transcriptBefore + 1 && r.transcript.back().second == "Stopped.", "S4 \"Stopped.\" reaches the transcript");
        auto held = r.ex.deferred.back(); r.ex.deferred.clear();
        held.second (ToolOutcome::success (juce::var (new juce::DynamicObject()), "u_late"));
        EJAgentClientTestAccess::feedStale (r.client, "round", "{\"sessionId\":\"ag_1\",\"round\":12}");
        pumpMs (30);
        check (r.posts.size() == postsBefore + 1 && r.step ("tc_16")->status == EJAgentClient::Step::Status::Stopped && r.client.round() == 12,
               "S5 a late completion and a stale frame change NOTHING: no step posted after the stop, the row stays Stopped");
        check (r.logHas ("STOP requested"), "S6 the log records the stop with the state it interrupted");
        check (r.client.undoAllowedNow() && r.client.canUndoAll(), "S7 Undo is reachable after a stop");
        r.ex.deferDo = false;
    }
    // ---- U: Undo ----------------------------------------------------------------------------------------------------------
    {
        // ---- D17, RULED 16 OCT 2026 (Sean: "accept, Undo all on multi-step runs, per-step Undo
        //      only where it's true") -------------------------------------------------------------
        // The plugin-wide history COALESCES consecutive edits and the sink pops its top entry, so a
        // per-step undo is only ever TRUE of a run with one undoable step. This run has more, so the
        // step's own Undo is refused - no token sent, the row untouched - and "Undo all" is the road.
        const auto tokensBefore = (int) r.ex.undoneTokens.size();
        r.client.undoStep ("tc_2");
        pumpMs (10);
        check ((int) r.ex.undoneTokens.size() == tokensBefore
               && r.step ("tc_2")->status != EJAgentClient::Step::Status::Undone
               && r.logHas ("undoable steps and the plugin's history COALESCES"),
               "U1 with SEVERAL undoable steps a step's own Undo is REFUSED and says why (D17)",
               "tokens " + juce::String (tokensBefore) + "->" + juce::String ((int) r.ex.undoneTokens.size()));
        check (r.client.canUndoAll(), "U2 another landed do remains, so Undo all is still offered");
        r.client.undoAll();
        pumpMs (10);
        check (r.ex.undoneCheckpoint == "cp_7" && ! r.client.canUndoAll() && r.client.notice().contains ("undone"), "U3 Undo all sends the session's checkpoint; every landed row is undone; a notice says so");
    }
    // ---- U4/U5: D17's OTHER DIRECTION - one undoable step, and the button does what it says -------
    {
        r.client.dismiss();
        r.client.start ("set the pre-gain to 5 dB", "chat_1");
        r.round (1);
        r.tool (R"({"id":"tc_u1","name":"do","args":{"op":"set_pre_gain","db":5},"approval":"free"})");
        r.await ({ "tc_u1" });
        pumpMs (20);
        EJAgentClientTestAccess::feed (r.client, "done", R"({"reason":"complete","summary":"Pre-gain at 5 dB.","rounds":1,"costUsd":0.0})");
        pumpMs (10);
        check (r.step ("tc_u1") != nullptr && r.step ("tc_u1")->canUndo(),
               "U4 a ONE-change run has exactly one undoable step");
        const auto t2 = (int) r.ex.undoneTokens.size();
        r.client.undoStep ("tc_u1");
        pumpMs (20);
        check ((int) r.ex.undoneTokens.size() == t2 + 1
               && r.step ("tc_u1")->status == EJAgentClient::Step::Status::Undone
               && r.step ("tc_u1")->detail == "undone",
               "U5 ...and ITS own Undo sends that step's token and the row reads undone",
               "tokens " + juce::String (t2) + "->" + juce::String ((int) r.ex.undoneTokens.size()));
    }
    // ---- E: errors, retry, done, dismiss -----------------------------------------------------------------------------------
    {
        r.client.start ("cut 2 dB at 300 Hz", "chat_1");
        check (r.client.steps().empty() && r.client.state() == EJAgentClient::State::Connecting && r.posts.back().first == "/api/agent/start", "E1 a new session starts clean");
        r.round (1);
        EJAgentClientTestAccess::feed (r.client, "error", R"({"code":"model_unavailable","message":"The model is busy.","retryable":true})");
        // ---- THE DRAWN LINE IS OURS NOW (17 Oct 2026) -------------------------------------------
        // This asserted the server's own sentence reached the screen verbatim. After tonight it does
        // not: Sean read "result toolu_01VtSX... is over 8 KB; send the compact summary shape
        // (CONTRACT_AGENT_TOOLS section 5)" in his chat, so the LINE is translated and the server's
        // words are kept for Details. The code still decides, the state and retryability are
        // unchanged, and the raw text is still available - which is what this now checks.
        check (r.client.state() == EJAgentClient::State::Failed && r.client.errorRetryable()
               && r.client.errorMessage().containsIgnoreCase ("busy")
               && r.client.rawErrorDetail() == "The model is busy."
               && r.transcript.back().second.containsIgnoreCase ("busy"),
               "E2 an error frame -> Failed, retryable, a PLAIN line on screen and the server's own "
               "sentence kept for Details");
        const auto lastBody = r.posts.back().second; const auto lastPath = r.posts.back().first; const auto n = r.posts.size();
        r.client.retry();
        // E3 still holds for a failure the server never saw - a 429 like this one, a 500, a timeout -
        // which is the case Retry exists for. What changed tonight is the OTHER case: a body the
        // server saw and refused for its content is never resent (W6).
        check (r.posts.size() == n + 1 && r.posts.back().first == lastPath && r.posts.back().second == lastBody,
               "E3 Retry re-posts the SAME request byte for byte when the server never saw it (429)");
        r.round (1); r.delta ("Done: the EQ took a 2 dB cut at 300 Hz.");
        EJAgentClientTestAccess::feed (r.client, "done", R"({"reason":"complete","summary":"Cut 2 dB at 300 Hz on the EchoJay EQ.","rounds":2,"costUsd":0.04})");
        check (r.client.state() == EJAgentClient::State::Done && r.client.doneReason() == "complete" && r.client.round() == 2 && std::abs (r.client.costUsd() - 0.04) < 1e-9
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
        r.plan (R"({"round":1,"heading":"Here is the plan for the mix bus - two changes need your ok before anything is applied to the rack.","items":[{"id":"tc_2","summary":"Build: EchoJay EQ (bell 300 Hz -2 dB Q 1.4, high shelf 10 kHz +1 dB), API-2500 (s) ratio 4:1 attack 10 ms release 300 ms, bx_saturator V2 drive 2 dB, EchoJay Level option dynamic target -12 LUFS, EchoJay Limiter ceiling -0.1 dBTP","why":"a build needs your ok","approval":"ask_first"},{"id":"tc_3","summary":"pre-gain -6 dB","approval":"ask_first"}]})");
        r.await ({ "tc_1", "tc_2", "tc_3" });
        pumpMs (10);
        r.client.interceptTyped ("wait");
        check (r.client.state() == EJAgentClient::State::AwaitingApproval && r.client.planLines().size() == 2 && r.client.planHeading().startsWith ("Here is the plan"),
               "L0 scene 1 is the plan card with its heading");
        sweep ("plan card + heading + long summary + notice");
        check (layoutNeeded > 0, "L1 the panel asked the editor to re-dock when its height changed");
        // scene 2: the ask card with many chips
        r.client.approveAll(); pumpMs (10);
        r.round (2);
        r.tool (R"EJ({"id":"tc_4","name":"talk","args":{"ask":{"question":"How loud should the mix bus land?","choices":[{"label":"Commercial (-8 LUFS)"},{"label":"Pushed (-7)"},{"label":"Leave dynamics, keep punch and breathing room (-12)","detail":"option dynamic"},{"label":"Match the input"},{"label":"Something else"},{"label":"Skip this"}],"allowFreeText":true}},"approval":"free"})EJ");
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

    // ---------------------------------------------------------------- M
    // ITEM 2 (Sean, 13 Oct 2026): ONE OK, NOT TWO.
    //
    // Sean pressed Apply on the plan and the executor then refused with "This is on the kick's channel, go
    // ahead?". Both gates were right in isolation and nothing connected them, so the press was discarded and
    // the same question came back from a layer below, in worse words. These legs assert the connection: the
    // card NAMES the track, and approving a line records the confirmation ON THAT LINE's call.
    {
        std::printf ("\n-- M: item 2, the plan names the track and Apply confirms it --\n");
        Rig r;
        r.client.start ("a touch more top end on the kick");
        r.round (1);
        r.tool ("{\"id\":\"s1\",\"name\":\"do\",\"approval\":\"ask_first\",\"summary\":\"EQ: high shelf +1.5 dB\","
                "\"args\":{\"op\":\"set\",\"slot\":2,\"channel\":\"bb411b8240\"}}");
        r.tool ("{\"id\":\"s2\",\"name\":\"do\",\"approval\":\"ask_first\",\"summary\":\"EQ: low cut 40 Hz\","
                "\"args\":{\"op\":\"set\",\"slot\":1}}");
        r.plan ("{\"round\":1,\"heading\":\"Top end on the kick\",\"items\":["
                "{\"id\":\"s1\",\"summary\":\"EQ: high shelf +1.5 dB\"},"
                "{\"id\":\"s2\",\"summary\":\"EQ: low cut 40 Hz\"}]}");

        r.await ({ "s1", "s2" });   // the await is what raises the card: submitPlan only runs from AwaitingApproval
        pumpMs (20);
        check (r.client.state() == EJAgentClient::State::AwaitingApproval, "M0 the card is up");
        const auto lines = r.client.planLines();
        check (lines.size() == 2, "M1 both lines are on the card", juce::String ((int) lines.size()));
        if (lines.size() == 2)
        {
            check (lines[0]->label.contains ("88_7even Kick_2_01"),
                   "M2 the cross-channel line NAMES the track  (RED as it stood: the card said only \"EQ: high "
                   "shelf +1.5 dB\", so Apply was a decision about an unnamed channel)", lines[0]->label);
            check (! lines[1]->label.contains ("88_7even Kick_2_01") && ! lines[1]->label.contains (" - on "),
                   "M3 a line on the channel already open says nothing about a channel", lines[1]->label);
        }
        // Apply all: every approved line's call carries the confirmation afterwards.
        r.client.approveAll();
        pumpMs (20);
        bool s1Confirmed = false, s2Confirmed = false;
        for (const auto& st : r.client.steps())
        {
            const bool c = argFlag (st.call, "confirm");
            if (st.id == "s1") s1Confirmed = c;
            if (st.id == "s2") s2Confirmed = c;
        }
        check (s1Confirmed, "M4 approving a line records confirm:true on ITS call, so the executor does not ask "
                            "again  (RED as it stood: nothing was recorded and doOp refused)");
        juce::ignoreUnused (s2Confirmed);
    }
    {
        // THE GATE STILL FAILS CLOSED. A line the user DECLINED must not carry a confirmation - otherwise the
        // fix would have turned one press into blanket permission, which is the opposite of what it is for.
        std::printf ("-- M5: a declined line carries no confirmation --\n");
        Rig r;
        r.client.start ("two changes");
        r.round (1);
        r.tool ("{\"id\":\"d1\",\"name\":\"do\",\"approval\":\"ask_first\",\"summary\":\"one\","
                "\"args\":{\"op\":\"set\",\"slot\":1,\"channel\":\"bb411b8240\"}}");
        r.plan ("{\"round\":1,\"heading\":\"h\",\"items\":[{\"id\":\"d1\",\"summary\":\"one\"}]}");
        r.await ({ "d1" });
        pumpMs (20);
        r.client.declineAll();
        pumpMs (20);
        bool confirmed = false;
        for (const auto& st : r.client.steps()) if (st.id == "d1") confirmed = argFlag (st.call, "confirm");
        check (! confirmed, "M5 a DECLINED line is not confirmed - the press is permission for what was pressed");
    }

    // ---- A9: A TAPPED PICK IS THE APPROVAL, INCLUDING ON ANOTHER TRACK (16 Oct 2026) ------------
    //
    // Sean's step 7, in Logic, on the Kick's Link: he tapped "Transient Master" from the chips and
    // was then asked to press Apply on "add Transient Master - on 88_7even Kick_2_01". Two gates,
    // each right on its own; between them he answered the same question twice.
    //
    // Both directions, because the dangerous mistake here is the other one - a tap that approves
    // something it did not choose:
    //   the step that carries out the TAPPED pick     -> runs, with confirm:true recorded on it
    //   a step for something else in the same round   -> still waits for a press
    {
        r.client.dismiss();
        r.client.start ("add a transient shaper to the kick", "chat_1");
        r.round (1);
        r.tool (R"({"id":"tc_pick","name":"talk","args":{"ask":{"question":"Which transient shaper?","choices":[{"label":"Transient Master"},{"label":"MTransient"}],"allowFreeText":false}},"approval":"free"})");
        r.await ({ "tc_pick" });
        pumpMs (10);
        r.client.answerAsk ("Transient Master");
        pumpMs (10);
        r.round (2);
        r.tool (R"({"id":"tc_add","name":"do","approval":"ask_first","summary":"add Transient Master - on 88_7even Kick_2_01","args":{"op":"add","name":"Transient Master","apply_to":"kick"}})");
        pumpMs (10);
        const auto* added = r.step ("tc_add");
        check (added != nullptr && added->status != EJAgentClient::Step::Status::AwaitingApproval
               && added->autoApplied
               && (bool) added->call.args.getProperty ("confirm", false)
               && r.logHas ("IS the approval for this step"),
               "A9a the step that carries out the TAPPED pick runs without a second press, and the tap "
               "is recorded as the cross-channel confirmation (Sean's step 7)",
               added == nullptr ? juce::String ("no step") 
                                : ("status=" + juce::String ((int) added->status)
                                   + " autoApplied=" + juce::String (added->autoApplied ? "y" : "n")
                                   + " confirm=" + juce::String ((bool) added->call.args.getProperty ("confirm", false) ? "y" : "n")));
        // ...and the tap is SPENT: anything else ask_first in the same run still waits.
        r.tool (R"({"id":"tc_other","name":"do","approval":"ask_first","summary":"add an EchoJay Limiter","args":{"op":"add","name":"EchoJay Limiter"}})");
        pumpMs (10);
        const auto* other = r.step ("tc_other");
        check (other != nullptr && other->status == EJAgentClient::Step::Status::AwaitingApproval
               && ! other->autoApplied,
               "A9b ...and the tap covers ONLY what it chose: another ask_first step still waits",
               other == nullptr ? juce::String ("no step")
                                : ("status=" + juce::String ((int) other->status)));
        r.client.stop();
    }

    // ---- A5 LIVES LAST TOO, AND FOR THE SAME REASON (16 Oct 2026) -------------------------------
    // This leg ENDS a run - that is what Sean's #21 asks for - and a stopped session posts no step
    // results (contract 1, which S5 asserts). Sitting where it used to sit, it quietly finished the
    // session that W1b then waited on: "posts 9->9", a timeout that had worked perfectly reported as
    // a failure. Every leg that stops a run now runs last, each in a session of its own.
    {
        r.client.dismiss();
        r.client.start ("louder?", "chat_1");
        r.round (5);
        pumpMs (10);
        r.round (6);
        r.tool (R"({"id":"tc_11","name":"talk","args":{"ask":{"question":"Louder?","choices":[{"label":"Yes"},{"label":"No"}],"allowFreeText":false}},"approval":"free"})");
        r.await ({ "tc_11" });
        pumpMs (10);
        const auto p2 = r.posts.size();
        // ---- SEAN'S #21 (16 Oct 2026) MOVED THIS LEG, AND IT IS A BETTER ANSWER ------------------
        // It used to assert that a typed reply under allowFreeText:false was REFUSED with a line and
        // the run kept waiting. His ruling: *"A new request typed while a run waits on an ask got
        // merged into that run. A new instruction closes the waiting run (marked stopped) and starts
        // a new run."* So with chips on screen and the server asking for a tap, text that names no
        // chip is a new instruction: interceptTyped RETURNS FALSE (the composer's own road takes it)
        // and the run is STOPPED. Nothing is dropped - which is the half of his rule the old
        // behaviour did keep, and the half this one keeps as well.
        // THE STOP IS ITSELF A POST, and always has been (stop() -> postStop() -> /api/agent/stop).
        // My first cut of this leg asserted the post count was unchanged, which was simply wrong
        // about the build: it failed from this morning until the gate ran the whole suite tonight.
        // What must be true is that NO STEP answered the ask and the last thing posted is the stop.
        juce::ignoreUnused (p2);
        // ---- AND SEAN'S ITEM 5 (17 Oct 2026) MOVED IT AGAIN, THE OTHER WAY --------------------
        //
        // He typed "cl1b" into "Which CL 1B do you have loaded?" - three chips, allowFreeText
        // false - and the run was STOPPED and restarted on that text as a goal. His ruling: *"A
        // name answers the ask (fuzzy match to installed plugins); only a sentence is a new
        // instruction."*
        //
        // So the test under this ask is no longer "does the text name a chip" but "is the text a
        // SENTENCE". "maybe" is neither a plugin name nor a sentence, so it is an ANSWER - which is
        // also the only reading that keeps his standing blocker ("a typed reply must never be
        // silently dropped") true for a one-word reply to a yes/no question.
        check (r.client.interceptTyped ("maybe") && ! r.client.askPending()
               && r.client.state() != EJAgentClient::State::Stopped,
               "A5 allowFreeText:false + a text that is NOT a sentence = the ANSWER (Sean's item 5: "
               "only a sentence is a new instruction; a one-word reply is a reply)");
    }

    // ---- A5c: AND A SENTENCE UNDER THE SAME ASK STILL STOPS THE RUN (#21 intact) ---------------
    // Named A5c because an A5b already exists 400 lines up ("a tap that is not one of the offered
    // labels is ignored") - the same id collision this harness had once before with tc_12, and a
    // duplicate id makes a scorecard lie about which leg ran.
    // The half of #21 that item 5 did NOT change, asserted on its own so the two rules cannot be
    // confused again: chips on screen, tap-only, and a verb-led instruction typed into it.
    {
        r.client.dismiss();
        r.client.start ("louder?", "chat_1");
        r.round (5);
        pumpMs (10);
        r.round (6);
        r.tool (R"({"id":"tc_11s","name":"talk","args":{"ask":{"question":"Louder?","choices":[{"label":"Yes"},{"label":"No"}],"allowFreeText":false}},"approval":"free"})");
        r.await ({ "tc_11s" });
        pumpMs (10);
        check (! r.client.interceptTyped ("add a compressor to this rack")
               && r.posts.back().first == "/api/agent/stop"
               && ! r.client.askPending()
               && r.client.state() == EJAgentClient::State::Stopped,
               "A5c ...while a SENTENCE typed into the same question is a NEW INSTRUCTION: the run is "
               "stopped and the text goes down the composer's own road (Sean's #21, unchanged)");
    }

    // ---- SL1-SL4: AN AGENT STOP READS AS WORDS, IN BOTH DIRECTIONS (17 Oct 2026) ----------------
    //
    // Named SL (stop line) and not W, because W1a-W4 in this same harness are the wait_for_playback
    // legs - I wrote "W3" and "W4" twice in one scorecard and caught it in the same session in which
    // I renamed A5b and ROW F2 for exactly that. A duplicate id makes a report unable to say which
    // leg ran.
    //
    // What Sean read on 17b: "The agent stopped: result toolu_01VtSXwQ7CTqLmVPLGMUao8n is over 8 KB;
    // send the compact summary shape (CONTRACT_AGENT_TOOLS section 5)". A guard that only checked
    // the happy wording would have passed all week, so this is held BOTH ways: the known codes get
    // their sentence, and a sentence carrying machinery is refused even when the code is unknown.
    {
        const auto tooBig = EJAgentClient::plainStopLine ("result_too_large",
            "result toolu_01VtSXwQ7CTqLmVPLGMUao8n is over 8 KB; send the compact summary shape "
            "(CONTRACT_AGENT_TOOLS section 5)");
        check (! tooBig.contains ("toolu_") && ! tooBig.containsIgnoreCase ("CONTRACT_")
               && ! tooBig.containsIgnoreCase ("8 KB") && tooBig.containsIgnoreCase ("too big")
               && tooBig.containsIgnoreCase ("less at a time"),
               "SL1 result_too_large reads as words: no tool id, no byte limit, no contract reference, "
               "and it says what to do next");

        const auto unknownClean = EJAgentClient::plainStopLine ("some_new_code",
                                                                "The widget shelf fell over.");
        check (unknownClean == "The widget shelf fell over.",
               "SL2 an unknown code with a plain sentence keeps the server's own words");

        const auto unknownDirty = EJAgentClient::plainStopLine ("some_new_code",
            "result toolu_01ABC is over 8 KB; see CONTRACT_AGENT_TOOLS section 5");
        check (! unknownDirty.contains ("toolu_") && ! unknownDirty.containsIgnoreCase ("CONTRACT_")
               && unknownDirty.isNotEmpty(),
               "SL3 ...but an unknown code whose text carries a tool id or a contract reference is "
               "REPLACED, not dressed up");

        // ---- B'S SECTION 85(1) (19 Oct 2026): A REAL MESSAGE IS NEVER MASKED ----------------
        // *"Show the server's error frame message as it comes, never 'I could not reach the
        // assistant just then'."*  A KNOWN code used to get our canned sentence whatever the server
        // had written, so a message B had taken trouble over was thrown away. Now the code only
        // supplies the words when the frame carries none - and SL1/SL3 above still hold, because
        // machinery is still never the headline (Sean's 17 Oct rule, which this does not touch).
        const auto bSays = EJAgentClient::plainStopLine ("round_failed",
            "The model returned no content for round 3 - nothing was changed.");
        check (bSays == "The model returned no content for round 3 - nothing was changed.",
               "SL5 a KNOWN code with a real sentence shows the SERVER's words  (RED as it stood: "
               "\"I could not reach the assistant just then\", whatever B had written)", bSays);
        const auto bSilent = EJAgentClient::plainStopLine ("round_failed", "");
        check (bSilent.containsIgnoreCase ("could not reach") || bSilent.isNotEmpty(),
               "SL5b ...and a frame with no words still gets a sentence from its code", bSilent);

        const auto empty = EJAgentClient::plainStopLine ("", "");
        check (empty.isNotEmpty() && ! empty.containsIgnoreCase ("error."),
               "SL4 no code and no text still produces a sentence, not a blank or \"an error\"");
    }

    // ---- X: THE CARD AND THE CHAT, FROM HIS 12:05 SESSION (18 Oct 2026) -----------------------
    {
        using namespace echojay::agentui;

        // ---- X1: WHICH RUN IS THIS -------------------------------------------------------------
        // His log: run #2's row was handed run #1's key one second before the server answered, so it
        // adopted run #1's record - the closing showed twice and the old answers came with it. The key
        // test was "has the sessionId moved", and the sessionId is EMPTY in that window.
        {
            Rig w;
            check (w.client.runSerial() == 0, "X1 PRECONDITION: no run has started yet");
            w.client.start ("what compressors do I have?", "chat_w");
            const int first = w.client.runSerial();
            check (first == 1 && w.client.sessionId().isEmpty(),
                   "X1 a run is identifiable the moment Send is pressed - runSerial has moved while the "
                   "sessionId is still empty, which is the whole window the row and the record are born in",
                   "serial=" + juce::String (first) + " sid=\"" + w.client.sessionId() + "\"");
            w.round (1);                             // now it has a session
            const auto sid1 = w.client.sessionId();
            w.client.start ("Add a compressor", "chat_w");
            check (w.client.runSerial() == first + 1 && w.client.sessionId().isEmpty(),
                   "X1b ...and the NEXT run is a different one by the same fact, again before any session "
                   "exists (RED as it stood: nothing could tell the two apart here, so the second run took "
                   "the first one's record)",
                   "serial=" + juce::String (w.client.runSerial()) + " was sid=" + sid1);
            const int afterStart = w.client.runSerial();
            w.round (1);
            w.client.retry();                        // a retry is the SAME run
            check (w.client.runSerial() == afterStart,
                   "X1c a retry does NOT move it - a retry is the same run, which is why generation_ (which "
                   "does move) could not have been used for this");
            w.client.stop();
            check (w.client.runSerial() == afterStart, "X1d ...and nor does a stop");
        }

        // ---- X2: A QUESTION'S LINE IS THE QUESTION ---------------------------------------------
        // The frame is his, verbatim from the log: name=talk, summary "ask".
        {
            const auto asked = parseFrame ("tool_call",
                R"({"id":"tc_pick_look_ask","name":"talk","summary":"ask","approval":"free","args":{"ask":{"question":"Which compressor shall I use on the kick?","choices":[{"label":"cl1b"},{"label":"the EchoJay Compressor"}]}}})");
            const auto line = describeCall (asked.call);
            check (line == "Which compressor shall I use on the kick?",
                   "X2 the card's line for an ask is the QUESTION, not the server's summary (RED as it "
                   "stood: the summary shortcut came first and the summary was the word \"ask\")", line);
            const auto bare = parseFrame ("tool_call",
                R"({"id":"tc_x","name":"talk","summary":"ask","approval":"free","args":{"text":""}})");
            check (describeCall (bare.call).isEmpty(),
                   "X2b ...and a talk whose summary is only the tool's own verb, with nothing else to say, "
                   "contributes NOTHING rather than the word \"ask\" (\"show the question or nothing\")",
                   describeCall (bare.call));
            const auto normal = parseFrame ("tool_call",
                R"({"id":"tc_y","name":"talk","summary":"I will look at the kick first","approval":"free","args":{"text":"looking"}})");
            check (describeCall (normal.call) == "I will look at the kick first",
                   "X2c ...and every other summary still wins, untouched - the server describes its own work "
                   "better than we can guess",
                   describeCall (normal.call));
        }

        // ---- X3: "YOU SAID" BELONGS TO THE ASK IT ANSWERED -------------------------------------
        // Two asks, ONE id, which is what his server sent. The second must not inherit the first's answer.
        {
            RunView before;
            {
                RunStep a; a.id = "tc_pick_look_ask"; a.label = "Which compressor?"; a.answered = "You said: cl1b";
                RunStep look; look.id = "tc_typed_look_1"; look.label = "read the inventory";
                before.steps = { a, look };
            }
            RunView now;
            {
                RunStep a; a.id = "tc_pick_look_ask"; a.label = "Which compressor?";
                RunStep look; look.id = "tc_typed_look_1"; look.label = "read the inventory";
                RunStep again; again.id = "tc_pick_look_ask"; again.label = "Which one of these?";
                now.steps = { a, look, again };
            }
            carryAnswersForward (now, before);
            check (now.steps[0].answered == "You said: cl1b",
                   "X3 the ask that WAS answered keeps its answer", now.steps[0].answered);
            check (now.steps[2].answered.isEmpty(),
                   "X3b ...and the NEW ask, which the server gave the same tool id, carries nothing - it has "
                   "not been answered (RED as it stood: matched by id, so it inherited \"You said: cl1b\" "
                   "before he had answered it)",
                   now.steps[2].answered.isEmpty() ? juce::String ("empty") : now.steps[2].answered);
            // And a list that DID shift carries nothing rather than the wrong thing.
            RunView shifted;
            { RunStep look; look.id = "tc_typed_look_1"; RunStep a; a.id = "tc_pick_look_ask"; shifted.steps = { look, a }; }
            carryAnswersForward (shifted, before);
            check (shifted.steps[0].answered.isEmpty() && shifted.steps[1].answered.isEmpty(),
                   "X3c ...and when the ids at a position disagree, nothing is carried at all - the position "
                   "and the id must both match, so a reordering can never move an answer onto another line");
        }

        // ---- X4: HIS 4,000-CHARACTER ANSWER, AT EVERY COLUMN WIDTH -----------------------------
        // The closing is the shape of the one he read at 12:03: four numbers and then 154 plugin names.
        // 180 px is narrower than the OLD 220 px layout floor, which is the width at which the card was
        // laid out wider than the column it was drawn into and the text was clipped mid-word.
        {
            juce::String closing = "You have 154 compressors: 56 I can set, 98 you'd set by ear, and 17 more "
                                   "that can't run on this Mac. Plus the EchoJay Compressor.\n\nI can set: ";
            const char* names[] = { "Acme Opticom XLA-3", "API-2500", "Auto-Tune Vocal Compressor",
                                    "Bettermaker Bus Compressor DSP", "bx_opto", "bx_townhouse Buss Compressor",
                                    "C1 comp", "dbx-160", "DPR-402", "elysia alpha master", "kHs Compressor",
                                    "Kiive XTComp", "Lindell 254E", "MaxxVolume", "MCompressor",
                                    "Millennia TCL-2", "SSL Native X-Verb", "Shadow Hills Mastering Comp" };
            for (int rep = 0; rep < 14; ++rep)
                for (const auto* n : names) closing << n << ", ";
            check (closing.length() > 3500,
                   "X4 PRECONDITION: the closing is the size of the one he read ("
                     + juce::String (closing.length()) + " characters)");
            RunView v;
            v.sessionId = "ag_w"; v.closing = closing; v.statusWord = "Done"; v.finished = true;
            v.modeBadge = "ASK"; v.changeCount = 0; v.stepsOpen = false;
            for (int width : { 180, 220, 420, 900 })
            {
                const auto L  = layoutRun (v, 0, 0, width);
                const auto why = checkRun (v, L, 0, width);
                check (why.isEmpty(),
                       "X4 at " + juce::String (width) + " px the whole answer has room: nothing truncated, "
                       "nothing outside the column (checkRun, which until today looked at the step rows and "
                       "never at the closing)",
                       why.isEmpty() ? juce::String ("clean, height ") + juce::String (L.totalHeight) : why);
            }
        }
    }

    // ---- Y: A CHIP'S NOTE (18 Oct 2026, Sean in Logic) ---------------------------------------
    //
    // *"The chips don't show their notes. B sends them on every chip as ask.choices[i].detail ("I can
    // set it", "I can set and measure it", "you'll set it by ear") with handSet: true on the by-ear
    // ones; draw the note under or beside each chip name."*
    //
    // The note was parsed and then dropped on the floor: the card's chips were built from `labels`
    // alone, so the one thing that distinguishes the choices - whether EchoJay does the work or he
    // does - never reached the screen.
    {
        using namespace echojay::agentui;
        const char* withNotes = R"EJ({"id":"tc_pick_comp","name":"talk","summary":"ask","approval":"free","args":{"ask":{"question":"Which compressor shall I use on the kick?","choices":[{"label":"cl1b","detail":"I can set it"},{"label":"the EchoJay Compressor","detail":"I can set and measure it"},{"label":"Shadow Hills Mastering Compressor","detail":"you'll set it by ear","handSet":true}],"allowFreeText":true}}})EJ";

        Rig w;
        w.client.start ("add a compressor", "chat_y");
        w.round (1);
        w.tool (withNotes);
        w.await ({ "tc_pick_comp" });
        pumpMs (10);
        const auto& ask = w.client.pendingAsk();
        check (w.client.askPending() && ask.labels.size() == 3 && ask.details.size() == 3
               && ask.details[0] == "I can set it" && ask.details[2] == "you'll set it by ear"
               && ask.handSet.size() == 3 && ! ask.handSet[0] && ! ask.handSet[1] && ask.handSet[2],
               "Y0 the ask arrives with a note on every choice and the by-ear one MARKED (RED as it "
               "stood: handSet was not read at all, so the two kinds of answer were indistinguishable)",
               "notes=" + ask.details.joinIntoString ("|") + " hand=" + juce::String ((int) ask.handSet.size()));

        const auto v = runViewFrom (w.client, {}, true);
        const auto* qs = v.steps.empty() ? nullptr : &v.steps.back();
        check (qs != nullptr && qs->chips.size() == 3 && qs->chipNotes.size() == 3
               && qs->chipNotes[1] == "I can set and measure it"
               && qs->chipHandSet.size() == 3 && qs->chipHandSet[2],
               "Y1 the card's question row carries one note per chip, parallel to the chips, and the "
               "by-ear mark with them (RED as it stood: the row took `labels` and nothing else)",
               qs == nullptr ? juce::String ("no step") : qs->chipNotes.joinIntoString ("|"));

        // Y2: the zone is what gets drawn and what gets pressed - the note has to be ON it, and the
        // chip has to be taller than a one-line chip or the note has nowhere to go.
        {
            const auto L = layoutRun (v, 0, 0, 420);
            int chips = 0, noted = 0, hand = 0, tall = 0;
            juce::Rectangle<int> third;
            for (const auto& z : L.zones)
                if (z.kind == RunHit::Chip)
                {
                    ++chips;
                    if (z.note.isNotEmpty()) ++noted;
                    if (z.handSet) { ++hand; third = z.rect; }
                    if (z.rect.getHeight() > Metrics::chipH) ++tall;
                }
            check (chips == 3 && noted == 3 && hand == 1 && tall == 3,
                   "Y2 every chip zone carries its note, one of them is marked by-ear, and all three "
                   "are TALLER than a one-line chip so the note has room",
                   "chips=" + juce::String (chips) + " noted=" + juce::String (noted)
                     + " hand=" + juce::String (hand) + " tall=" + juce::String (tall));
            check (! third.isEmpty() && chipNameArea (third, true).getHeight() == Metrics::chipNameH
                   && chipNoteArea (third, true).getHeight() > 0
                   && chipNameArea (third, true).getBottom() <= chipNoteArea (third, true).getY(),
                   "Y2b ...and the name's area and the note's area do not overlap - the one author of "
                   "the split answers the layout, the paint and the check the same way");
        }

        // Y3: and it all fits, at his column widths. The same question asked of the closing in X4.
        for (int width : { 180, 220, 420 })
        {
            const auto L   = layoutRun (v, 0, 0, width);
            const auto why = checkRun (v, L, 0, width);
            check (why.isEmpty(),
                   "Y3 at " + juce::String (width) + " px the notes are whole: nothing truncated, "
                   "nothing outside the column",
                   why.isEmpty() ? juce::String ("clean, height ") + juce::String (L.totalHeight) : why);
        }

        // Y4: THE OTHER DIRECTION. A note drawn into a one-line chip is exactly the defect checkRun
        // is for, so it must SAY so - a check that cannot go red is not a check.
        {
            auto L = layoutRun (v, 0, 0, 420);
            for (auto& z : L.zones)
                if (z.kind == RunHit::Chip && z.note.isNotEmpty()) { z.rect.setHeight (Metrics::chipH); break; }
            const auto why = checkRun (v, L, 0, 420);
            check (why.contains ("note"), "Y4 a note squeezed back into a one-line chip is CAUGHT and named", why);
        }

        // Y5: and an ask with no notes is laid out exactly as it was before today - the same rects,
        // from the same function, through the overload every other surface still calls.
        {
            Rig q;
            q.client.start ("louder?", "chat_y2");
            q.round (1);
            q.tool (R"({"id":"tc_plain","name":"talk","args":{"ask":{"question":"Louder?","choices":[{"label":"Yes"},{"label":"No"}],"allowFreeText":false}},"approval":"free"})");
            q.await ({ "tc_plain" });
            pumpMs (10);
            const auto pv = runViewFrom (q.client, {}, true);
            const auto L  = layoutRun (pv, 0, 0, 420);
            int plain = 0, wrong = 0;
            for (const auto& z : L.zones)
                if (z.kind == RunHit::Chip)
                {
                    ++plain;
                    if (z.rect.getHeight() != Metrics::chipH || z.note.isNotEmpty()) ++wrong;
                }
            juce::StringArray yesNo; yesNo.add ("Yes"); yesNo.add ("No");
            std::vector<juce::Rectangle<int>> oldWay;
            const int h = layoutChips (yesNo, 0, 0, 420, oldWay);
            std::vector<juce::Rectangle<int>> newWay;
            const int h2 = layoutChips (yesNo, juce::StringArray(), 0, 0, 420, newWay);
            check (plain == 2 && wrong == 0 && h == h2 && oldWay.size() == newWay.size()
                   && oldWay[0] == newWay[0] && oldWay[1] == newWay[1],
                   "Y5 a chip with no note is the chip it always was: 26 px, and the notes-free call "
                   "returns the same rects and the same height as the new one",
                   "plain=" + juce::String (plain) + " wrong=" + juce::String (wrong)
                     + " h=" + juce::String (h) + "/" + juce::String (h2));
        }

        // Y6: the record. The notes ride on the saved message, additively, and a snapshot written by
        // a build that knew nothing about them still reads - both directions of the downgrade rule.
        {
            const auto back = fromJson (toJson (v));
            const auto* bs = back.steps.empty() ? nullptr : &back.steps.back();
            check (bs != nullptr && bs->chips.size() == 3 && bs->chipNotes.size() == 3
                   && bs->chipNotes[2] == "you'll set it by ear"
                   && bs->chipHandSet.size() == 3 && bs->chipHandSet[2] && ! bs->chipHandSet[0],
                   "Y6 the notes and the by-ear marks survive the snapshot the chat message holds",
                   bs == nullptr ? juce::String ("no step") : bs->chipNotes.joinIntoString ("|"));
            const auto older = fromJson (juce::JSON::parse (
                R"({"sessionId":"ag_old","steps":[{"id":"tc_old","label":"Which one?","status":"awaiting","chips":"Yes\nNo"}]})"));
            check (older.steps.size() == 1 && older.steps[0].chips.size() == 2
                   && older.steps[0].chipNotes.size() == 2 && older.steps[0].chipNotes[0].isEmpty()
                   && older.steps[0].chipHandSet.size() == 2 && ! older.steps[0].chipHandSet[1],
                   "Y6b ...and a snapshot from before today reads with its chips intact and no notes - "
                   "padded to the chips' length, never mis-paired");
        }
    }

    // ---- Z: THE ANSWER ONCE, IN THE CARD, AND THE QUESTION ONCE (18 Oct 2026, his 13:5x log) --
    {
        using namespace echojay::agentui;

        // His run, in the two strings it actually produced: the round's talk carried the whole
        // answer, and the done frame's summary carried the same answer CUT MID-WORD.
        juce::String whole = "You've got 154 compressors: 56 I can set, 98 you'd set by ear, and 31 "
                             "more that can't run on this Mac. Plus the EchoJay Compressor.\n\n"
                             "The ones I can set: Acme Opticom XLA-3, API-2500, Auto-Tune Vocal "
                             "Compressor, Bettermaker Bus Compressor DSP, bx_opto, bx_townhouse Buss "
                             "Compressor, C1 comp, C1 comp-gate, C1 comp-sc, dbx-160, DPR-402, elysia "
                             "alpha master, elysia alpha mix, Empirical Labs Mike-E Comp, kHs "
                             "Compressor, Kiive XTComp, Lindell 254E, Lindell 7X-500, Lindell MBC, "
                             "Lindell SBC, MaxxVolume, MCompressor, Millennia TCL-2, Mixland Vac "
                             "Attack, OneKnob Pressure, Pro Audio DSP DSM V3, RCompressor, RVox, "
                             "SSL Native X-Comp, Shadow Hills Mastering Compressor, Tube-Tech CL 1B.";
        const auto cut = whole.substring (0, whole.indexOf ("RCompressor") + 10);   // "...RCompresso"
        check (cut.endsWith ("RCompresso") && whole.startsWith (cut) && cut != whole
               && ! whole.endsWith (cut),
               "Z0 PRECONDITION: the summary is the answer's BEGINNING, cut mid-word - not equal to "
               "it, not its tail, not a whole line of it, which is why every test the 12:05 fix added "
               "walked straight past it",
               "cut " + juce::String (cut.length()) + " of " + juce::String (whole.length()));

        {
            RunView v;
            v.sessionId = "ag_z"; v.statusWord = "Done"; v.finished = true; v.modeBadge = "ASK";
            v.opening = whole; v.closing = cut;
            v.foldProseIntoTheCard();
            check (v.closing == whole,
                   "Z1 the card says the WHOLE answer, once (RED as it stood: the card drew the cut "
                   "copy under the bubble's full one - \"the closing still shows twice\")",
                   juce::String (v.closing.length()) + " chars, ends \""
                     + v.closing.substring (v.closing.length() - 12) + "\"");
            // and nothing of it is lost or spilled, at his column widths
            for (int width : { 180, 220, 420 })
            {
                const auto L   = layoutRun (v, 0, 0, width);
                const auto why = checkRun (v, L, 0, width);
                check (why.isEmpty() && L.totalHeight > 0,
                       "Z1b at " + juce::String (width) + " px the card holds the whole answer: nothing "
                       "truncated, nothing outside the column",
                       why.isEmpty() ? juce::String ("clean, height ") + juce::String (L.totalHeight) : why);
            }
        }
        {
            // A closing that says something NEW is still kept - after the prose, once.
            RunView v;
            v.sessionId = "ag_z2"; v.opening = "Looking at the kick.";
            v.closing = "Added the CL 1B.";
            v.foldProseIntoTheCard();
            check (v.closing == "Looking at the kick.\n\nAdded the CL 1B.",
                   "Z2 a closing that is NOT the prose is kept, under it - never lose a sentence we "
                   "were given", v.closing);
        }
        {
            // THE FOLD CANNOT DOUBLE. The card folds from the snapshot on every paint; a rule that
            // appended unconditionally would grow the answer by one copy per pass.
            RunView v;
            v.sessionId = "ag_z3"; v.opening = "Looking at the kick."; v.closing = "Added the CL 1B.";
            v.foldProseIntoTheCard();
            const auto once = v.closing;
            v.foldProseIntoTheCard();
            v.foldProseIntoTheCard();
            check (v.closing == once, "Z3 folding twice changes nothing - the card's sentence is the "
                                      "same after three passes as after one", v.closing);
        }
        {
            // THE QUESTION, ONCE. The asking step's line IS the question (X2), so the block under it
            // was a second copy of the same sentence.
            Rig w;
            w.client.start ("add a compressor", "chat_z");
            w.round (1);
            w.tool (R"EJ({"id":"tc_q","name":"talk","summary":"Which compressor do you want on the kick?","approval":"free","args":{"ask":{"question":"Which compressor do you want on the kick?","choices":[{"label":"EchoJay Compressor"}],"allowFreeText":true}}})EJ");
            w.await ({ "tc_q" });
            pumpMs (10);
            const auto v = runViewFrom (w.client, {}, true);
            check (! v.steps.empty() && v.steps.back().label == "Which compressor do you want on the kick?"
                   && v.askQuestion.isEmpty(),
                   "Z4 the question is on the row and NOT repeated in the block below it (RED as it "
                   "stood: the step line and the ask block both drew it)",
                   "row=\"" + (v.steps.empty() ? juce::String() : v.steps.back().label)
                     + "\" block=\"" + v.askQuestion + "\"");
            const auto L = layoutRun (v, 0, 0, 420);
            check (L.askQuestion.isEmpty(), "Z4b ...so the layout reserves nothing for the block either");
        }
        {
            // AND THE QUESTION IS NEVER LOST. The block goes because the ROW carries it: a rule that
            // cleared the block without that being true would have deleted the question itself,
            // which is the fault with the sign flipped.
            Rig w;
            w.client.start ("add a compressor", "chat_z2");
            w.round (1);
            w.tool (R"EJ({"id":"tc_q2","name":"talk","summary":"ask","approval":"free","args":{"ask":{"question":"Which compressor do you want on the kick, and how hard should it work?","choices":[{"label":"EchoJay Compressor","detail":"I can set it"}],"allowFreeText":true}}})EJ");
            w.await ({ "tc_q2" });
            pumpMs (10);
            const auto v = runViewFrom (w.client, {}, true);
            const auto L = layoutRun (v, 0, 0, 220);
            const bool onTheRow = ! v.steps.empty()
                && v.steps.back().label == "Which compressor do you want on the kick, and how hard should it work?";
            check (onTheRow && v.askQuestion.isEmpty() && ! L.rows.empty()
                   && checkRun (v, L, 0, 220).isEmpty(),
                   "Z5 ...and the question is not LOST with the block: it is on the row, whole, inside "
                   "the column at 220 px",
                   checkRun (v, L, 0, 220));
        }
    }

    // ---- TP: A TAP IS NEVER DROPPED IN SILENCE, AND SILENCE HAS A BOUND (18 Oct 2026, item 4) --
    //
    // *"Never drop a tap silently, and 30 s with nothing back = a plain sentence with Retry."*
    {
        Rig t;
        t.client.start ("add a compressor", "chat_t");
        t.round (1);
        t.tool (R"({"id":"tc_t","name":"talk","args":{"ask":{"question":"Which one?","choices":[{"label":"Yes"},{"label":"No"}],"allowFreeText":false}},"approval":"free"})");
        t.await ({ "tc_t" });
        pumpMs (10);
        check (t.client.askPending() && t.client.notice().isEmpty(),
               "TP0 PRECONDITION: a question is waiting and there is no notice on the row");

        const auto before = t.posts.size();
        t.client.answerAsk ("Maybe", false);                 // a label the card never offered
        check (t.posts.size() == before && t.client.askPending(),
               "TP1 a tap that is none of the offered choices still sends NOTHING and leaves the "
               "question waiting - the rule A5b wrote, unchanged");
        check (t.client.notice().isNotEmpty() && t.client.notice().containsIgnoreCase ("not on this question"),
               "TP1b ...and it is no longer SILENT: the row says why (RED as it stood: a bare return, so "
               "the chip went down and nothing on screen moved)",
               t.client.notice().isEmpty() ? juce::String ("(silence)") : t.client.notice());
        check (t.logHas ("none of the 2 choice(s)"),
               "TP1c ...and the log names it, with how many choices the card had");

        t.client.clearNotice();
        t.client.answerAsk ("", false);
        check (t.client.notice().isNotEmpty() && t.client.askPending(),
               "TP2 an EMPTY tap says so too rather than vanishing", t.client.notice());

        // The real answer still works, and clears the notice on its way through.
        t.client.clearNotice();
        const auto postsBefore = t.posts.size();
        t.client.answerAsk ("Yes", false);
        check (t.posts.size() == postsBefore + 1 && ! t.client.askPending(),
               "TP3 ...and an offered choice still answers the question - the refusals are refusals, "
               "not a gate on every tap");
        t.client.answerAsk ("Yes", false);                  // nothing is waiting now
        check (t.client.notice().containsIgnoreCase ("already answered"),
               "TP3b a tap that arrives after the question is gone says THAT, which is a different "
               "sentence from \"not on this question\"", t.client.notice());
    }
    {
        // SILENCE HAS A BOUND, AND IT IS 30 SECONDS. Driven on the client's own clock rather than by
        // waiting: the guard sets the bound to a few milliseconds, which is the same code path.
        // THE WATCHDOG'S OWN CADENCE IS PART OF THE TEST: it checks once a SECOND, so a bound of a
        // few milliseconds would be a bound nothing ever reads - my first cut of this leg pumped
        // 400 ms against a 40 ms bound and went red on its own clock, not on the product's.
        Rig t;
        t.client.stallTimeoutMs = 1200;                      // the 30 s rule, in guard time
        t.client.start ("add a compressor", "chat_t2");
        t.round (1);
        pumpMs (2600);                                       // nothing comes back at all, past two ticks
        check (t.client.state() == EJAgentClient::State::Failed,
               "TP4 a stream that goes silent past the bound ENDS, rather than spinning for ever",
               EJAgentClient::stateName (t.client.state()));
        check (t.client.errorMessage().containsIgnoreCase ("nothing came back")
               && ! t.client.errorMessage().containsIgnoreCase ("connection")
               && ! t.client.errorMessage().contains ("stream_stalled"),
               "TP4b ...and says so in a plain sentence - no code, no diagnosis of the network (RED as "
               "it stood: \"The connection went quiet for 90 s\")", t.client.errorMessage());
        check (t.client.errorRetryable(),
               "TP4c ...with Retry offered, which is the one thing he can do about it");
        {
            // the SHIPPED default, read off a fresh client rather than off the one the leg retuned
            Rig fresh;
            check (fresh.client.stallTimeoutMs == 30'000,
                   "TP4d and the shipped bound is his 30 seconds",
                   juce::String (fresh.client.stallTimeoutMs));
        }
    }

    // ---- SZ1: AN OVERSIZED RESULT IS COMPACTED, NOT POSTED (17 Oct 2026) ----------------------
    // The executor's own gate, asked directly with a result the size of Sean's machine: 420 entries
    // of the shape 17b sent. It must come back inside the contract's 8 KB.
    {
        juce::Array<juce::var> bad;
        for (int i = 0; i < 420; ++i)
        {
            auto* e = new juce::DynamicObject();
            e->setProperty ("name", "Seeded Unloadable " + juce::String (i + 1));
            e->setProperty ("loadable", false);
            e->setProperty ("why", "is installed but its VST3 has no arm64 build, so it cannot run in this host");
            bad.add (juce::var (e));
        }
        auto* o = new juce::DynamicObject();
        o->setProperty ("what", "inventory");
        o->setProperty ("notLoadable", juce::var (bad));
        juce::var big (o);
        const auto beforeBytes = (int) juce::JSON::toString (big, true).getNumBytesAsUTF8();
        // The BASE compactResult - the one the client calls through ToolExecutor&, and the one a
        // fixture executor inherits. ExecutorRead overrides it with fitUnder8K; this proves the
        // floor, which is what protects a run when some other executor is in the seat.
        FakeExecutor fx;
        const auto after = fx.compactResult (big);
        const auto afterBytes = (int) juce::JSON::toString (after, true).getNumBytesAsUTF8();
        check (beforeBytes > 8192 && afterBytes <= 8192,
               "SZ1 a result the size of his machine's unloadable list (" + juce::String (beforeBytes)
               + " bytes) is compacted under the contract's 8 KB (" + juce::String (afterBytes)
               + ") by the base gate every executor inherits");
    }

    // ---- SZ2/SZ3: THE PRODUCT'S OWN INVENTORY BUILDER, ON HIS MACHINE'S LIST (17 Oct 2026) -----
    //
    // SZ1 proves the FLOOR - the base gate any executor inherits. It does not touch the function
    // that actually built the 25 KB body his Logic session died on, and it uses synthetic entries.
    // So these two legs call `ExecutorRead::inventoryPage` itself, with the snapshot of HIS folder
    // that tools/host_rig/fixtures holds: 1,013 installed names, 357 of them unable to run here.
    //
    // AND BOTH DIRECTIONS, because a size test that cannot fail is not a test (the rule this tree
    // has written down once already). SZ3 reconstructs the 17b shape on the SAME input - the page
    // plus the whole unloadable list appended after the gate, which is exactly what 17b posted -
    // and FAILS if that does not break the limit. If a future change makes the old shape fit, SZ2's
    // green stops meaning anything, and SZ3 is what says so.
    {
        const auto snapFile = juce::File (__FILE__).getParentDirectory().getParentDirectory()
                                  .getChildFile ("host_rig/fixtures/real_inventory_2026-10-17.json");
        const auto snap = juce::JSON::parse (snapFile.loadFileAsString());
        juce::StringArray names;
        if (auto* arr = snap.getProperty ("names", juce::var()).getArray())
            for (const auto& n : *arr) names.add (n.toString());
        juce::Array<juce::var> bad;
        if (auto* arr = snap.getProperty ("notLoadable", juce::var()).getArray())
            for (const auto& e : *arr)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("name", e.getProperty ("name", juce::var()));
                o->setProperty ("loadable", false);
                o->setProperty ("why", e.getProperty ("why", juce::var()));
                bad.add (juce::var (o));
            }
        // The fixture is the test's input, so its absence is a RED, not a quiet pass on nothing -
        // the lesson from RP8K's first run (a comfortable number from an input that never arrived).
        check (names.size() > 900 && bad.size() > 300,
               "SZ2a the snapshot of his folder is readable and is the size it claims ("
               + juce::String (names.size()) + " names, " + juce::String (bad.size())
               + " unable to load) - without it the two legs below would measure nothing");

        using echojay::agent::ExecutorRead;
        const auto page = ExecutorRead::inventoryPage (names, {}, 0, ExecutorRead::kInventoryPage,
                                                       juce::var (bad));
        const auto pageBytes = (int) juce::JSON::toString (page, true).getNumBytesAsUTF8();
        const int  total     = (int) page.getProperty ("total", juce::var (-1));
        const int  returned  = (int) page.getProperty ("returned", juce::var (-1));
        const int  notTotal  = (int) page.getProperty ("notLoadableTotal", juce::var (-1));
        check (pageBytes <= 8192 && total == names.size() && returned > 0 && returned < total
               && notTotal == bad.size(),
               "SZ2 look(inventory) on HIS 1,013 plugins is " + juce::String (pageBytes)
               + " bytes - inside the contract's 8 KB - and still says the scale: total "
               + juce::String (total) + ", returned " + juce::String (returned)
               + ", notLoadableTotal " + juce::String (notTotal));

        // The 17b shape: the same page with the whole list appended AFTER the size check.
        juce::var old = page.clone();
        if (auto* o = old.getDynamicObject())
        {
            o->setProperty ("notLoadable", juce::var (bad));
            o->removeProperty ("notLoadableTotal");
        }
        const auto oldBytes = (int) juce::JSON::toString (old, true).getNumBytesAsUTF8();
        check (oldBytes > 8192,
               "SZ3 the shape 17b posted for the same folder is " + juce::String (oldBytes)
               + " bytes, over the limit - so SZ2's green is a fix and not an artefact of the input"
               + " (the run he lost was this, " + juce::String (oldBytes / 1024) + " KB)");

        // ---- SZ4: THE CENSUS, ON THE SAME FOLDER (18 Oct 2026, B's FOR_A 76) -------------------
        //
        // B: *"with your page cap of 200 it takes eight pages on Sean's 1,577-name Mac (nine plugin
        // steps)... on Sean's run they named none (total 180)."*  SZ2 is the ordinary page and stays
        // exactly as it is; this is the server's own sweep over the SAME input, which is the folder
        // the complaint came from rather than a synthetic one.
        const auto census = ExecutorRead::inventoryPage (names, {}, 0, 2000, juce::var (bad), true);
        const auto cBytes = (int) juce::JSON::toString (census, true).getNumBytesAsUTF8();
        const auto* cNames = census.getProperty ("names", juce::var()).getArray();
        const auto* cBad   = census.getProperty ("notLoadable", juce::var()).getArray();
        check (cNames != nullptr && cNames->size() == names.size()
               && ! (bool) census.getProperty ("hasMore", juce::var())
               && cBytes > 8192 && cBytes <= ExecutorRead::kCensusMaxBytes,
               "SZ4 the census page over his " + juce::String (names.size()) + " plugins is ONE page ("
               + juce::String (cBytes / 1024) + " KB, no hasMore) instead of the "
               + juce::String ((names.size() + ExecutorRead::kInventoryPageMax - 1) / ExecutorRead::kInventoryPageMax)
               + " the 200 cap needed - and deliberately outside the 8 KB that still binds every "
                 "other call",
               cNames == nullptr ? juce::String ("no names") : juce::String (cNames->size()) + " names");
        check (cBad != nullptr && cBad->size() == bad.size(),
               "SZ4b ...and all " + juce::String (bad.size()) + " plugins that cannot run here are NAMED on it "
               "(RED as it stood: twelve at most and page-scoped, which on this folder named none of them)",
               cBad == nullptr ? juce::String ("absent") : juce::String (cBad->size()) + " named");
    }

    // ---- RC1/RC2: A STUCK CHAT RECOVERS - no old result is ever posted again (17 Oct 2026) -----
    //
    // His screenshot: the run died on `result_too_large`, he pressed Retry and it failed with THE
    // SAME tool id, and so did his next message in that chat. Two separate faults behind one
    // symptom: retry() re-posted the refused bytes verbatim, and the stored body outlived the run.
    //
    // RC1 is the retry; RC2 is the next message. Both assert on what was POSTED, because that is the
    // only place the symptom lives.
    {
        r.client.dismiss();
        r.posts.clear();
        r.client.start ("read the inventory", "chat_stuck");
        r.round (1);
        r.tool (R"({"id":"toolu_STUCK","name":"look","args":{"what":"inventory"},"approval":"free"})");
        r.await ({ "toolu_STUCK" });
        pumpMs (30);
        const auto postsAfterStep = r.posts.size();
        // the server refuses the CONTENT of that step
        // A CUSTOM DELIMITER, because the sentence itself contains `)"` - "(CONTRACT_AGENT_TOOLS
        // section 5)" - which ends an ordinary R"( ... )" literal in the middle of the string.
        EJAgentClientTestAccess::feed (r.client, "error",
            R"JSON({"code":"result_too_large","message":"result toolu_STUCK is over 8 KB; send the compact summary shape (CONTRACT_AGENT_TOOLS section 5)","retryable":true})JSON");
        pumpMs (10);

        // W6: Retry must not put those bytes back on the wire.
        r.client.retry();
        pumpMs (20);
        bool resentOld = false;
        for (size_t i = postsAfterStep; i < r.posts.size(); ++i)
            if (r.posts[i].second.contains ("toolu_STUCK")) resentOld = true;
        check (! resentOld,
               "RC1 after result_too_large, Retry does NOT resend the refused body (no post carries the "
               "old tool id again - his screenshot showed exactly that)");

        // ...and it says something a person can act on rather than going quiet.
        check (r.client.errorMessage().isNotEmpty()
               && ! r.client.errorMessage().contains ("toolu_")
               && ! r.client.errorMessage().containsIgnoreCase ("8 KB"),
               "RC1b ...and the line he is left with is plain, with no tool id and no byte limit");

        // W7: the next message in the same chat starts clean.
        const auto before = r.posts.size();
        r.client.start ("add a compressor", "chat_stuck");
        pumpMs (20);
        bool carriedOld = false;
        for (size_t i = before; i < r.posts.size(); ++i)
            if (r.posts[i].second.contains ("toolu_STUCK")) carriedOld = true;
        check (! carriedOld && r.posts.size() > before,
               "RC2 ...and his NEXT message starts a fresh run that carries nothing from the failed one");
    }

    // ---- A8 LIVES LAST, BECAUSE IT STOPS A RUN (16 Oct 2026) ------------------------------------
    // When this block sat in the middle of the file the legs after it were testing the leftovers: W1b
    // fed its frames into the stopped session and waited for a post that could never come, and
    // "restoring" the session with dismiss + start cost S7 the landed step whose undo it asserts. A
    // leg that ends a run belongs where nothing follows it, with a session of its own.
    {
        r.client.dismiss();
        r.client.start ("add a transient shaper to the kick", "chat_1");
        r.round (1);
        pumpMs (10);
        // ---- A8: AN ANSWER IS A NAME, AN INSTRUCTION IS A SENTENCE (16 Oct 2026) -----------------
        //
        // The chip-less ask, which the joint live pass showed the real server sending five times out
        // of five (`{"question":"Pick a transient shaper...","choices":[],"allowFreeText":false}`).
        // Sean's blocker says typing must answer it; his #21 says a new instruction must close the
        // waiting run. Held in BOTH directions here, because a one-sided test of this would ship
        // either a swallow or a run that cannot be answered:
        //
        //   "MTransient"               -> the ANSWER (a name, no leading verb)
        //   "Set the pre-gain to 2 dB" -> a NEW INSTRUCTION (a verb-led sentence): run stopped,
        //                                 interceptTyped returns false so the composer takes it.
        check (! EJAgentClient::typedLooksLikeNewInstruction ("MTransient")
               && ! EJAgentClient::typedLooksLikeNewInstruction ("the EchoJay Transient")
               && ! EJAgentClient::typedLooksLikeNewInstruction ("elysia museq")
               && ! EJAgentClient::typedLooksLikeNewInstruction ("Pro-C 3"),
               "A8a a NAME is never read as an instruction (no leading verb, or too short to be a sentence)");
        check (EJAgentClient::typedLooksLikeNewInstruction ("Set the pre-gain to 2 dB")
               && EJAgentClient::typedLooksLikeNewInstruction ("Add Pro-C 2")
               && EJAgentClient::typedLooksLikeNewInstruction ("remove the limiter please"),
               "A8b a VERB-LED SENTENCE is always an instruction (Sean's #21, and the live pass's B4)");
        r.round (7);
        r.tool (R"({"id":"tc_nochip","name":"talk","args":{"ask":{"question":"Pick a transient shaper for the kick","choices":[],"allowFreeText":false}},"approval":"free"})");
        r.await ({ "tc_nochip" });
        pumpMs (10);
        check (r.client.askPending() && r.client.pendingAsk().labels.isEmpty() && r.client.pendingAsk().allowFreeText,
               "A8c a chip-less ask waits with free text FORCED on (the server said false; there is nothing to tap)");
        const auto pN = r.posts.size();
        juce::ignoreUnused (pN);
        check (! r.client.interceptTyped ("Set the pre-gain to 2 dB")
               && r.posts.back().first == "/api/agent/stop" && ! r.client.askPending()
               && r.client.state() == EJAgentClient::State::Stopped,
               "A8d ...and a verb-led sentence typed into it STOPS the run and goes down the composer's road");
        r.round (7);
        r.tool (R"({"id":"tc_nochip2","name":"talk","args":{"ask":{"question":"Pick a transient shaper for the kick","choices":[],"allowFreeText":false}},"approval":"free"})");
        r.await ({ "tc_nochip2" });
        pumpMs (10);
        const auto pN2 = r.posts.size();
        check (r.client.interceptTyped ("MTransient") && r.posts.size() == pN2 + 1
               && r.result (0).getProperty ("result", {}).getProperty ("typed", {}).toString() == "MTransient",
               "A8e ...while a NAME typed into the same question is the answer, as {typed:\"<text>\"}");

    }


    // ---- RO (19 Oct 2026, his 08:00 report): A RESTORED CARD'S READ-ONLY VIEWS ----------------
    //
    // *"The restored card is there, but only the steps toggle is clickable. Details and N change(s)
    // are in the wrong place and don't open. Both are read-only views, so on a restored card they
    // must sit exactly where they sat live and open the same. Only the buttons that act stay
    // locked: Undo, Apply, Skip, Retry, Stop, chips. Also open a restored card in the same
    // expanded/collapsed state it finished in."*
    //
    // His leg, in his words: *"finish a run with a failure and a change, reopen, and Details and
    // the change list open with the same content and position as live."*  So this lays out the LIVE
    // card, puts it through the store's own round trip (toJson -> fromJson) and the product's own
    // record transformation (RunView::makeRecord - moved out of the editor for exactly this leg, so
    // the guard cannot compare against its own idea of a record), and asks for the same rects.
    std::printf ("\n== RO: a restored card's Details and change list sit where they sat live ==\n");
    {
        using namespace echojay::agentui;
        const int width = 420;
        RunView live;
        live.sessionId = "ses_ro"; live.runKey = "key_ro";
        live.opening = "Here's what I did on the Kick.";
        live.closing = "One change landed and one step failed.";
        live.statusWord = "Done"; live.modeBadge = "ASK";
        live.finished = true; live.running = false; live.stepsOpen = true;
        {
            RunStep a;                      // THE CHANGE: a do that landed and can be undone
            a.id = "st_added"; a.label = "Added Tube-Tech CL 1B to slot 1";
            a.detail = "read back Threshold -15 dB"; a.right = "applied";
            a.status = (int) EJAgentClient::Step::Status::Done;
            a.canUndo = true;
            live.steps.push_back (a);
            RunStep b;                      // THE FAILURE: a line with a technical detail behind it
            b.id = "st_failed"; b.label = "Set the release on slot 1";
            b.detail = "The release couldn't be read.";
            b.modelDetail = "param_not_readable: Release returned \"0.3\" with no unit";
            b.status = (int) EJAgentClient::Step::Status::Failed;
            b.hasDetails = true; b.expanded = true;       // he left Details OPEN when it finished
            live.steps.push_back (b);
        }
        live.changeCount = 1;
        live.showUndoAll = true; live.showRetry = true;   // live, these are offered
        const auto Llive = layoutRun (live, 0, 0, width);

        // the store's round trip, then the product's own record transformation
        auto rec = fromJson (toJson (live));
        rec.opening = live.opening;        // the editor supplies this from the ChatMsg, as always
        rec.makeRecord();
        const auto Lrec = layoutRun (rec, 0, 0, width);

        auto zoneOf = [] (const RunRects& L, RunHit k, int stepIndex = -2)
        {
            for (const auto& z : L.zones)
                if (z.kind == k && (stepIndex == -2 || z.stepIndex == stepIndex)) return z;
            return RunZone {};
        };
        const auto dLive = zoneOf (Llive, RunHit::Details);
        const auto dRec  = zoneOf (Lrec,  RunHit::Details);
        check (! dLive.rect.isEmpty() && ! dRec.rect.isEmpty(),
               "RO1 a restored card still HAS a Details control (it is a question about the picture, "
               "not an action on the run)",
               "live " + dLive.rect.toString() + "  record " + dRec.rect.toString());
        check (dRec.rect == dLive.rect,
               "RO2 ...at exactly the rect it had live (RED as it stood: the press was answered by "
               "whichever card painted last, which is what 'in the wrong place' was)",
               "live " + dLive.rect.toString() + "  record " + dRec.rect.toString());
        const auto tLive = zoneOf (Llive, RunHit::ToggleSteps);
        const auto tRec  = zoneOf (Lrec,  RunHit::ToggleSteps);
        check (! tRec.rect.isEmpty() && tRec.rect == tLive.rect && tRec.label == tLive.label,
               "RO3 and the change list sits where it sat live, with the same caption",
               "live \"" + tLive.label + "\" " + tLive.rect.toString()
               + "  record \"" + tRec.label + "\" " + tRec.rect.toString());

        // THE ACTING CONTROLS ARE THE ONLY THING THAT GOES.
        check (zoneOf (Lrec, RunHit::Undo).rect.isEmpty()
               && zoneOf (Lrec, RunHit::UndoAll).rect.isEmpty()
               && zoneOf (Lrec, RunHit::ApplyLine).rect.isEmpty()
               && zoneOf (Lrec, RunHit::SkipLine).rect.isEmpty()
               && zoneOf (Lrec, RunHit::ApplyAll).rect.isEmpty()
               && zoneOf (Lrec, RunHit::Retry).rect.isEmpty()
               && zoneOf (Lrec, RunHit::Stop).rect.isEmpty()
               && zoneOf (Lrec, RunHit::Chip).rect.isEmpty(),
               "RO4 Undo, Apply, Skip, Retry, Stop and the chips are all gone from the record - the "
               "run they would act on is over");

        // THE CONTENT BEHIND DETAILS IS THE SAME CONTENT.
        const RunStep* fLive = nullptr; const RunStep* fRec = nullptr;
        for (const auto& st : live.steps) if (st.id == "st_failed") fLive = &st;
        for (const auto& st : rec.steps)  if (st.id == "st_failed") fRec  = &st;
        check (fLive != nullptr && fRec != nullptr
               && fRec->detail == fLive->detail && fRec->modelDetail == fLive->modelDetail,
               "RO5 the failure's detail and its technical message survive the round trip, so what "
               "Details opens is what it opened live",
               fRec != nullptr ? fRec->modelDetail.substring (0, 60) : juce::String ("missing"));
        check (fRec != nullptr && fRec->expanded,
               "RO6 and the card comes back in the state it FINISHED in - Details was open, so it "
               "opens open");
        check (rec.stepsOpen == live.stepsOpen && rec.changeCount == live.changeCount,
               "RO7 ...including whether the steps were showing, and how many changes it counted",
               juce::String ((int) rec.stepsOpen) + " / " + juce::String (rec.changeCount));

        // AND COLLAPSED STAYS COLLAPSED, which is the other half of "the state it finished in".
        {
            RunView collapsed = live;
            collapsed.stepsOpen = false;
            auto rec2 = fromJson (toJson (collapsed));
            rec2.makeRecord();
            check (! rec2.stepsOpen,
                   "RO8 a card that finished collapsed comes back collapsed");
            const auto t2 = zoneOf (layoutRun (rec2, 0, 0, width), RunHit::ToggleSteps);
            check (! t2.rect.isEmpty() && t2.label.containsIgnoreCase ("change"),
                   "RO8b ...and its change list is the control that opens it, named for what it holds",
                   t2.label);
        }
    }


    // ---- RO9: A FINISHED RUN IS NOT A RECORD (19 Oct 2026) ------------------------------------
    //
    // The rig's C5, D3 and D4 all failed on one cause and the arithmetic is conclusive:
    //   runViewFrom:        showUndoAll = finished && canUndoAll()      (only when FINISHED)
    //   agentRunViewFor:    a row was a RECORD unless isActive()        (false when finished)
    // Two mutually exclusive conditions, so from 18 Oct Undo all could not be drawn at all, and the
    // per-step Undo went with it - while the client's own contract says undo is allowed for exactly
    // Done, Stopped and Failed (undoAllowedNow). His 08:00 "only the steps toggle is clickable" was
    // this, not the restored-card fault he took it for.
    //
    // Held in BOTH directions here, which is what the withdrawn (b3) leg could not manage: the
    // finished run's own card offers the controls, and the same card as a RECORD does not.
    std::printf ("\n== RO9: a finished run keeps Undo; the same card as a record does not ==\n");
    {
        using namespace echojay::agentui;
        RunView fin;
        fin.sessionId = "ses_fin"; fin.runKey = "key_fin";
        fin.opening = "Done - I added it."; fin.statusWord = "Done";
        fin.finished = true; fin.running = false; fin.stepsOpen = true;
        fin.showUndoAll = true;                       // what runViewFrom computes for a finished run
        RunStep a;
        a.id = "st_add"; a.label = "Added Tube-Tech CL 1B to slot 1"; a.right = "applied";
        a.status = (int) EJAgentClient::Step::Status::Done;
        a.canUndo = true;
        fin.steps.push_back (a);
        fin.changeCount = 1;
        const auto L = layoutRun (fin, 0, 0, 420);
        bool undoAll = false, stepUndo = false;
        for (const auto& z : L.zones)
        {
            if (z.kind == RunHit::UndoAll) undoAll = true;
            if (z.kind == RunHit::Undo)    stepUndo = true;
        }
        check (undoAll, "RO9 a FINISHED run's card draws Undo all - the one state it can be drawn in "
                        "(RED as it stood: the editor called every finished run a record, so the two "
                        "conditions were mutually exclusive and it was never drawn)");
        check (stepUndo, "RO9b ...and the landed step's own Undo");

        auto rec = fin;
        rec.makeRecord();
        const auto LR = layoutRun (rec, 0, 0, 420);
        bool recUndoAll = false, recStepUndo = false;
        for (const auto& z : LR.zones)
        {
            if (z.kind == RunHit::UndoAll) recUndoAll = true;
            if (z.kind == RunHit::Undo)    recStepUndo = true;
        }
        check (! recUndoAll && ! recStepUndo,
               "RO9c and the SAME card as a record draws neither - a reopened chat's run is over, "
               "which is the distinction his 08:00 ruling actually draws");
    }

    std::printf ("\n==== agent_client_guard: %s (%d ok, %d failed) ====\n", failures == 0 ? "GREEN" : "RED", passes, failures);
    return failures == 0 ? 0 : 1;
}
