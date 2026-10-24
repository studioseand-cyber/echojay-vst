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
#include "EJAgentClientTestAccess.h"
#include "EJAgentExecutorRead.h"   // SZ2/SZ3 call the product's own inventoryPage
#include "EJAgentPanel.h"
#include "EJAgentRunView.h"    // V: the run ROW, whose geometry is what Sean reads in Logic
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace echojay::agent;

// The one named seam (declared friend in EJAgentClient.h).
// The client's test door is now one shared header (tools/include/EJAgentClientTestAccess.h),
// included above, so the two harnesses that feed frames cannot drift apart.

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
    // His 09:49: the auto-dial-only toggle, asked of the executor on every request.
    bool autoDialOnlyAnswer = false;
    bool autoDialOnly() override { return autoDialOnlyAnswer; }
    // B, 21 Oct: the header's genre, which arrived empty on agent starts.
    juce::String genreAnswer;
    juce::String genreName() override { return genreAnswer; }
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
    /** B's keep-playing contract (22 Oct 2026): the plugin a keep_playing dial is still setting,
        or empty. The product reads this from the racks; a fake only has to be able to say both. */
    juce::String keepPlaying;
    juce::String keepPlayingPlugin() const override { return keepPlaying; }
    juce::StringPairArray dialsLanded;                                   // his 08:43 ruling
    juce::StringPairArray dialResults() const override { return dialsLanded; }
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
        // THE BUILD IN THIS SCENE NAMES ITS PLUGINS NOW (22 Oct 2026). "API-2500" is spelled without
        // its "(s)" suffix for one reason: these fixtures are R"(...)" raw strings and ')"' closes
        // one, so the name as the summary spells it would end the literal mid-JSON. It cost a build.
        // It was {"op":"build"} with no chain at all - shorthand for "a build line", which is what this layout scene is about. His
        // 21:48 fault made that shorthand illegal: a build naming no plugin is never offered (UP1),
        // and the product already refused an empty one at apply time ("that build carried no chain").
        // The three names are the ones this step's own summary describes, so the line it draws - which
        // is the thing L0 measures - is unchanged.
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
        // ---- RE-AIMED 23 Oct 2026, AND SAID PLAINLY: this leg asserted the OLD sentence ------
        // It read `detail == "Got it, you can stop."`, which his 08:10 ruling overturns: this
        // fixture's run has shown no closing, so the run cannot promise that nothing else will need
        // playback and the honest line is "Got it." W2's subject is unchanged - the minimum is
        // reached, the wait ends, the reading goes back - and that is what it now reads. The
        // stop INVITATION has its own pair of legs below (KS1/KS2), where it belongs.
        check (r.step ("tc_13")->detail == "Got it." && r.client.state() != EJAgentClient::State::Listening,
               "W2c the row says the listen is done and the wait is over  (the sentence is \"Got it.\" "
               "and not \"you can stop\", because this run has rounds still to come - his 08:10 point 2)",
               "detail \"" + r.step ("tc_13")->detail + "\", state "
               + juce::String (EJAgentClient::stateName (r.client.state())));
        // ---- PW (19 Oct 2026, his 12:33): EACH FIGURE WITH ITS OWN WINDOW -------------------
        // *"check playback still says loudest 3 s -17.5 LUFS under integrated -14.1. If integrated
        // is measured over a longer window than the last 8 s, put both on the same window, or name
        // each window in the payload so B can label it truthfully."*
        // This is the THIRD site that reports a loudest-3 s: check(level) and analysisVar were
        // fixed this morning and this one was missed, which is why he read it again after an
        // install. The window naming is the part B needs; the withholding is the part that stops an
        // impossible pair being quotable at all.
        check (std::abs ((double) res.getProperty ("loudestWindowSeconds", juce::var (0.0))) >= 0.0
               && res.hasProperty ("integratedWindow"),
               "PW1 the playback payload NAMES each figure's window, so B can label them truthfully",
               "integratedWindow=\"" + res.getProperty ("integratedWindow", juce::var()).toString() + "\"");

        // ---- PW3 (B's section 86(2)): PLAIN RMS, OR NOTHING -------------------------------
        // *"The profile's curve needs the slot input as sine-referenced RMS dBFS... check(playback)
        // sends LUFS only, so the server uses the loudest-3-second LUFS as a stand-in and logs
        // proxy: true."*  The fields are sent when known and ABSENT when not - a profile fed a
        // stand-in it takes for a measurement is worse off than one that knows it has none.
        r.round (80);
        r.tool (R"({"id":"tc_rms","name":"check","args":{"what":"playback","min_seconds":4},"approval":"free"})");
        r.await ({ "tc_rms" });
        r.ex.reading = {};
        r.ex.reading.heardAboveSeconds = 4.0f;
        r.ex.reading.integratedLufs = -18.0f;
        r.ex.reading.loudestShortTermLufs = -12.0f;
        r.ex.reading.plainRmsKnown  = true;
        r.ex.reading.loudestRmsDbfs = -11.4f;
        r.ex.reading.typicalRmsDbfs = -19.2f;
        r.ex.reading.loudestPeakDbfs = -3.1f;
        pumpMs (60);
        {
            const auto rp = r.result (0).getProperty ("result", {});
            check (std::abs ((double) rp.getProperty ("loudestRmsDbfs", juce::var (0.0)) + 11.4) < 0.05
                   && std::abs ((double) rp.getProperty ("typicalRmsDbfs", juce::var (0.0)) + 19.2) < 0.05
                   && std::abs ((double) rp.getProperty ("loudestPeakDbfs", juce::var (0.0)) + 3.1) < 0.05,
                   "PW3 check(playback) carries plain dBFS RMS and the peak, so the profile's curve "
                   "stops running on a LUFS stand-in (B's 86(2))",
                   juce::JSON::toString (rp).substring (0, 150));
        }
        r.round (81);
        r.tool (R"({"id":"tc_nor","name":"check","args":{"what":"playback","min_seconds":4},"approval":"free"})");
        r.await ({ "tc_nor" });
        r.ex.reading = {};
        r.ex.reading.heardAboveSeconds = 4.0f;
        r.ex.reading.integratedLufs = -18.0f;
        r.ex.reading.plainRmsKnown = false;            // too little audio for a percentile
        pumpMs (60);
        {
            const auto rp = r.result (0).getProperty ("result", {});
            check (! rp.hasProperty ("loudestRmsDbfs") && ! rp.hasProperty ("typicalRmsDbfs"),
                   "PW3b ...and they are ABSENT rather than zero when the window is too short to "
                   "measure them", juce::JSON::toString (rp).substring (0, 110));
        }

        // PW2: HIS PAIR, -17.5 UNDER -14.1, WITHHELD WITH ITS REASON.
        r.round (90);
        r.tool (R"({"id":"tc_pw","name":"check","args":{"what":"playback","min_seconds":8},"approval":"free"})");
        r.await ({ "tc_pw" });
        r.ex.reading = {};
        r.ex.reading.heardAboveSeconds = 8.0f;
        r.ex.reading.integratedLufs = -14.1f;            // his figures, exactly
        r.ex.reading.loudestShortTermLufs = -17.5f;
        r.ex.reading.integratedIsLongerWindow = true;    // a Link: two instruments, two spans
        r.ex.reading.loudestWindowSeconds = 8.0f;
        pumpMs (60);
        {
            const auto pw = r.result (0).getProperty ("result", {});
            check (! pw.hasProperty ("loudestLufs"),
                   "PW2 a loudest-3 s BELOW the integrated level is withheld from the playback answer "
                   "too  (RED as it stood: -17.5 was reported beside -14.1, which he read twice)",
                   juce::JSON::toString (pw).substring (0, 120));
            check (pw.getProperty ("loudestUnavailable", juce::var()).toString().containsIgnoreCase ("different span"),
                   "PW2b ...with the reason, not silently");
            check (pw.getProperty ("integratedWindow", juce::var()).toString().containsIgnoreCase ("longer"),
                   "PW2c ...and the integrated figure's window is named as the longer one",
                   pw.getProperty ("integratedWindow", juce::var()).toString());
        }

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
        r.tool (R"({"id":"tc_2","name":"do","args":{"op":"build","chain":[{"name":"EchoJay EQ"},{"name":"API-2500"},{"name":"bx_saturator V2"}]},"approval":"ask_first","summary":"Build: EchoJay EQ (bell 300 Hz -2 dB Q 1.4, high shelf 10 kHz +1 dB), API-2500 (s) ratio 4:1 attack 10 ms release 300 ms, bx_saturator V2 drive 2 dB, EchoJay Level option dynamic target -12 LUFS, EchoJay Limiter ceiling -0.1 dBTP","why":"a build needs your ok"})");
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

    // ---- ONCE, NOT TWICE (19 Oct 2026, his 17:20 run) -----------------------------------------
    //
    // *"a failed step's error shows twice ('slot 0 is not on the rack (1..1).' on two lines); show
    // it once."*
    //
    // When the step's failure IS the run's failure, the step's own `detail` and the run's `closing`
    // (from errorMessage()) are the same sentence, so the card printed it on the step line and again
    // underneath. The step line is the one that belongs - it names WHICH step failed.
    std::printf ("== ONE FAILURE, ONE LINE: the closing never repeats a failed step's own words ==\n");
    {
        Rig rf;
        rf.ex.onDo = [] (const ToolCall&)
        {
            return ToolOutcome::failureFor ("unknown_slot",
                                            "slot 0 is not on the rack (1..1)",
                                            "slot 0 is not on the rack (1..1).");
        };
        rf.client.start ("set slot 0", "chat_once");
        rf.round (1);
        rf.tool (R"({"id":"tc_f1","name":"do","args":{"op":"set","slot":0},"approval":"free"})");
        rf.await ({ "tc_f1" });
        pumpMs (20);
        const auto* fs = rf.step ("tc_f1");
        check (fs != nullptr && fs->status == EJAgentClient::Step::Status::Failed,
               "ON0 the fixture: the step failed",
               fs == nullptr ? juce::String ("no step") : fs->detail);

        // THE RUN then fails with the SAME sentence, which is what his 17:20 card did.
        EJAgentClientTestAccess::feed (rf.client, "error",
            R"({"code":"unknown_slot","message":"slot 0 is not on the rack (1..1).","retryable":false})");
        pumpMs (10);
        const auto v = echojay::agentui::runViewFrom (rf.client, {}, true);
        bool onAStep = false;
        for (const auto& st : v.steps)
            if (st.right.containsIgnoreCase ("not on the rack")
                || st.detail.containsIgnoreCase ("not on the rack")) onAStep = true;
        check (onAStep,
               "ON1 the failure is still ON the step line, which is the line that names which step it was");
        check (! v.closing.containsIgnoreCase ("not on the rack"),
               "ON2 ...and the closing does NOT repeat it  (RED as it stood: the same sentence on two "
               "lines, the step's and the run's)", v.closing.substring (0, 90));

        // THE OTHER DIRECTION. A closing that says something the steps do NOT say is still shown -
        // a dedupe that swallowed every closing would lose the run's own account of itself.
        Rig rg;
        rg.ex.onDo = [] (const ToolCall&)
        { return ToolOutcome::failureFor ("unknown_slot", "slot 0 is not on the rack (1..1)",
                                          "slot 0 is not on the rack (1..1)."); };
        rg.client.start ("set slot 0", "chat_once2");
        rg.round (1);
        rg.tool (R"({"id":"tc_g1","name":"do","args":{"op":"set","slot":0},"approval":"free"})");
        rg.await ({ "tc_g1" });
        pumpMs (20);
        EJAgentClientTestAccess::feed (rg.client, "error",
            R"({"code":"round_failed","message":"The model returned no content for round 2.","retryable":true})");
        pumpMs (10);
        const auto vg = echojay::agentui::runViewFrom (rg.client, {}, true);
        check (vg.closing.isNotEmpty(),
               "ON3 a closing that says something no step said is STILL shown", vg.closing.substring (0, 90));
    }

    // ---- VIS: A RUN THAT IS STARTED GETS A VISIBLE CARD (Sean's 18:18 item 3, 20 Oct 2026) ------
    //
    // *"Add a guard that fails if a run is started and no row is drawn within a second, since this
    // is the third time today a run has gone invisible."*
    //
    // THE WALL-CLOCK VERSION OF THAT IS A RIG ROW - only a real editor in a real host draws a row,
    // and this guard has no editor. What CAN be held here is the decision the editor makes, which is
    // where all three failures actually were, and it is now a named function instead of a condition
    // inside a 200-line sync method: echojay::agentui::rowBelongsInChat.
    //
    // His three blanks, as assertions:
    //   16:06  a new run in a new chat, refused because a member described the LAST run
    //   18:18  the same after a chat switch, which destroys the row the member was keyed to
    //   11:59  the case that must KEEP working: an older run waiting elsewhere stays there
    {
        std::printf ("== VIS: a started run is drawn where it was started, and only there ==\n");
        using echojay::agentui::rowBelongsInChat;

        check (rowBelongsInChat ("chatA", "chatA"),
               "VIS1 a run started in this chat gets its row HERE - his 16:06 and 18:18 blanks "
               "(\"No card, no spinner, no Stop\") were this returning false");
        check (! rowBelongsInChat ("chatA", "chatB"),
               "VIS2 a run that belongs to another chat is NOT drawn here - his 11:59, survival is "
               "not capture");
        // AN UNKNOWN EITHER SIDE DRAWS IT. An invisible live run is the worse failure by a long way:
        // he cannot even stop it. So absence of information is never a reason to hide one.
        check (rowBelongsInChat ("", "chatB"),
               "VIS3 a run with no chat id is still DRAWN - an invisible run he cannot stop is worse "
               "than a card in the wrong place");
        check (rowBelongsInChat ("chatA", ""),
               "VIS4 ...and so is a run in a window with no chat open yet");
        check (rowBelongsInChat ("", ""),
               "VIS5 ...and both unknown");
        check (rowBelongsInChat ("  chatA  ", "chatA"),
               "VIS6 whitespace is not an identity - a padded id is the same chat");

        // ---- AND THE RUN IS THE AUTHORITY, which is the whole 18:18 fix -----------------------
        // The client is handed the chat id at start() and keeps it. Before this round nothing could
        // read it, so the editor keyed on a member of its own that a chat switch invalidates.
        {
            Rig r;
            r.client.start ("add a delay", "chatLink");
            check (r.client.runChatId() == "chatLink",
                   "VIS7 the RUN knows which chat it was started in - the fact the editor was "
                   "guessing at", r.client.runChatId());
            check (rowBelongsInChat (r.client.runChatId(), "chatLink"),
                   "VIS8 ...so a row opens in that chat");
            check (! rowBelongsInChat (r.client.runChatId(), "chatMixBus"),
                   "VIS9 ...and not in the one he came from");
            // A SECOND RUN IN A DIFFERENT CHAT TAKES ITS OWN ID. This is his 18:18 sequence exactly:
            // a run was waiting in the mix bus's chat, he switched to the Link's chat and typed.
            r.client.start ("add a delay", "chatLink2");
            check (r.client.runChatId() == "chatLink2",
                   "VIS10 a NEW run re-stamps the chat - so the next one cannot inherit the last "
                   "run's chat, which is what left three runs invisible today", r.client.runChatId());
            check (rowBelongsInChat (r.client.runChatId(), "chatLink2"),
                   "VIS11 ...and IS drawn in the chat he typed it in");
        }
    }


    // ---- S86: ONLY "RUN A CHECK" LISTENS AGAIN (19 Oct 2026, B's section 86(1)) --------------
    //
    // *"The closing is a talk ask whose text is the closing... Its id starts with tc_check... Only
    // 'Run a check' listens again and verifies. Under the contract, a check(playback) after the
    // move is refused in process unless the user tapped 'Run a check'."*  And Sean: *"your
    // level-match loop is exempt, it's not the agent asking"* - exempt by construction, since the
    // loop reads the tallies in-process and never asks for a wait.
    //
    // Keyed on B's own id marker. My first cut keyed on "has anything been applied", which refused
    // the waits that come BEFORE a closing - every wait a run legitimately makes - and turned
    // fifteen assertions in this file red.
    {
        Rig r86;
        r86.client.start ("mix it", "chat_86");     // a session, as every other leg's rig has

        // ---- BEFORE ANY MOVE, A PLAYBACK WAIT RUNS (19 Oct 2026, B's section 92) --------------
        // *"at 17:20 your check(playback) refusal fired in round 1 with nothing changed, and B no
        // longer draws a step for it. Refuse a playback check only after a move has landed in this
        // run. It's on nearly every agent-on run and costs a slow round each time."*
        //
        // FIRST, because this is the case the old key got wrong: the refusal was armed by the
        // CLOSING having been seen, which is not "something has changed" - and which start() never
        // cleared, so every run after the first in a session refused its own round 1.
        r86.round (1);
        r86.tool (R"({"id":"tc_w0","name":"check","args":{"what":"playback","min_seconds":4},"approval":"free"})");
        r86.await ({ "tc_w0" });
        pumpMs (60);
        check (r86.client.state() == EJAgentClient::State::Listening && r86.client.playback().active,
               "S92a round 1 with NOTHING changed: the playback wait RUNS  (RED as it stood: refused "
               "with \"a listen after a change needs the user's own Run a check\" before any change "
               "existed, on nearly every agent-on run)", r86.client.statusLine());

        r86.round (2);
        r86.tool (R"({"id":"tc_check_1","name":"talk","args":{"text":"Done.","ask":{"question":"How's it sounding?","choices":[{"label":"Sounds good"},{"label":"Run a check"}]}},"approval":"free"})");
        r86.await ({ "tc_check_1" });
        pumpMs (20);
        check (r86.client.askPending(), "S86a the closing ask is up (its id starts with tc_check)");

        // A CLOSING ALONE STILL ARMS NOTHING: it is shown by runs that changed nothing at all (a
        // question, a read, a refusal), which is the other half of what the old key got wrong.
        r86.round (3);
        r86.tool (R"({"id":"tc_w0b","name":"check","args":{"what":"playback","min_seconds":4},"approval":"free"})");
        r86.await ({ "tc_w0b" });
        pumpMs (60);
        check (r86.client.state() == EJAgentClient::State::Listening,
               "S92b a closing with no change behind it does NOT arm the refusal",
               r86.client.statusLine());

        // NOW A MOVE LANDS, and only now is a second listen the agent's to decline.
        r86.round (4);
        r86.tool (R"({"id":"tc_do1","name":"do","args":{"op":"add","name":"EchoJay EQ"},"approval":"free"})");
        r86.await ({ "tc_do1" });
        pumpMs (20);
        check (r86.step ("tc_do1") != nullptr
               && r86.step ("tc_do1")->status == EJAgentClient::Step::Status::Done,
               "S92c the fixture: a change has landed in this run");

        r86.round (5);
        r86.tool (R"({"id":"tc_w1","name":"check","args":{"what":"playback","min_seconds":4},"approval":"free"})");
        r86.await ({ "tc_w1" });
        pumpMs (60);
        check (r86.client.state() != EJAgentClient::State::Listening,
               "S86b a playback wait AFTER a landed change is refused - no second listen he did not ask "
               "for  (RED as it stood: the run opened a window and waited)", r86.client.statusLine());
        check (r86.step ("tc_w1") != nullptr
               && r86.step ("tc_w1")->detail.containsIgnoreCase ("Run a check"),
               "S86c ...and the row names the chip that would allow one",
               r86.step ("tc_w1") != nullptr ? r86.step ("tc_w1")->detail.substring (0, 90)
                                             : juce::String ("no step"));

        r86.client.answerAsk ("Run a check");
        r86.round (6);
        r86.tool (R"({"id":"tc_w2","name":"check","args":{"what":"playback","min_seconds":4},"approval":"free"})");
        r86.await ({ "tc_w2" });
        pumpMs (60);
        check (r86.client.state() == EJAgentClient::State::Listening && r86.client.playback().active,
               "S86d after \"Run a check\" the wait runs - one tap, one listen", r86.client.statusLine());

        // ---- AND A NEW RUN STARTS CLEAN (19 Oct 2026, the leak behind his 17:20) --------------
        // This is the mechanism, not just the symptom: start() cleared neither closingAskSeen_ nor
        // userAskedForCheck_ nor the landed-move flag, so run 2 of a session inherited run 1's
        // state. A run that inherits "the closing is up" refuses its own first listen - which is
        // precisely what he saw on nearly every agent-on run, and why it looked like round 1.
        r86.client.start ("now the vocal", "chat_86b");
        r86.round (1);
        r86.tool (R"({"id":"tc_w3","name":"check","args":{"what":"playback","min_seconds":4},"approval":"free"})");
        r86.await ({ "tc_w3" });
        pumpMs (60);
        check (r86.client.state() == EJAgentClient::State::Listening && r86.client.playback().active,
               "S92d a SECOND run in the same session starts clean: its round 1 wait runs  (RED as it "
               "stood: run 1's closing was still believed, so run 2 refused before doing anything)",
               r86.client.statusLine());
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
            // ---- Z2u: AN UNDONE RUN'S CARD DOES NOT FOLD THE VOIDED PROSE BACK IN -------------
            //
            // Sean's #14: *"after 'Everything the agent changed has been undone' the run still shows
            // 'Pre gain on the Mix Bus is set to 0 dB', a stale sentence."*  build() replaces the
            // CLOSING when every change is undone - and the card's closing is folded from the
            // opening plus the closing, so the fold wrote the model's stale prose straight back into
            // the field the replacement had just cleaned. Rig row C5 failed on this for three days
            // and its only coverage was a 28-minute host run; this is the same fault in a second.
            RunView v;
            v.sessionId = "ag_z2u"; v.changeCount = 0;
            v.opening = "The pre gain on the Mix Bus is set to 3 dB.";
            v.closing = "Everything this run changed has been undone, so what it said above no longer "
                        "describes the rack.";
            v.closingBeforeUndo = "The pre gain on the Mix Bus is set to 3 dB.";
            v.foldProseIntoTheCard();
            check (! v.closing.containsIgnoreCase ("is set to"),
                   "Z2u RED-FIRST: after Undo all the card no longer claims the change (RED as it "
                   "stood: the fold put the opening's claim back on, so the card read \"The pre gain "
                   "... is set to 3 dB\" above the sentence saying it had been undone)",
                   v.closing);
            check (v.closing.startsWithIgnoreCase ("Everything this run changed"),
                   "Z2u ...and what it says instead is the undone sentence, by itself", v.closing);
        }
        {
            // ---- AND THE OTHER DIRECTION, WHICH IS WHAT KEEPS THE GUARD HONEST ----------------
            // The same view with a change that LANDED after the undo folds normally again. Without
            // this leg the fix above could have been "never fold", which would pass Z2u and lose
            // every model answer on every card.
            RunView v;
            v.sessionId = "ag_z2v"; v.changeCount = 1;
            v.opening = "The pre gain on the Mix Bus is set to 3 dB.";
            v.closing = "Added the CL 1B.";
            v.closingBeforeUndo = "an earlier undo left its mark in the record";
            v.foldProseIntoTheCard();
            check (v.closing == "The pre gain on the Mix Bus is set to 3 dB.\n\nAdded the CL 1B.",
                   "Z2v a run that landed something AFTER an undo folds normally - the mark in the "
                   "record does not latch the rule on for ever",
                   v.closing);
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
            // ---- A PLAN WITH A NAMELESS SLOT IS NEVER OFFERED (his 21:48 fault, 22 Oct 2026) ----
            //
            // *"'build 6 slots: EchoJay EQ, EchoJay Compressor, EchoJay Compressor, , , EchoJay
            //  Limiter' with Apply/Skip. Two slots have NO name ... The fallback must never offer a
            //  slot with no plugin. If the plan can't be read, say so plainly ('I couldn't read the
            //  plan, try again') with a Retry chip, rather than building a generic chain."*
            //
            // His own six-slot op, two entries nameless, exactly as it arrived.
            Rig w;
            w.client.start ("build a mix bus", "chat_unreadable");
            w.round (1);
            w.tool (R"EJ({"id":"tc_bad","name":"do","approval":"ask_first","summary":"build a mix bus","args":{"op":"build","chain":[{"name":"EchoJay EQ"},{"name":"EchoJay Compressor"},{"name":"EchoJay Compressor"},{"settings":"glue"},{},{"name":"EchoJay Limiter"}]}})EJ");
            w.await ({ "tc_bad" });
            pumpMs (20);
            const auto* bad = w.step ("tc_bad");
            check (bad != nullptr && bad->status == EJAgentClient::Step::Status::Failed,
                   "UP1 a build whose slots name no plugin is NOT offered for approval  (RED as it "
                   "shipped at 21:48: it came up as a plan line with Apply/Skip, and Apply would have "
                   "put four plugins in and dropped two without saying so)",
                   bad == nullptr ? juce::String ("no step at all")
                                  : juce::String ("status ") + juce::String ((int) bad->status));
            check (bad != nullptr && bad->detail == "I couldn't read the plan, try again.",
                   "UP2 ...and it says so in his words, rather than building a generic chain",
                   bad != nullptr ? "\"" + bad->detail + "\"" : juce::String());
            const auto badView = runViewFrom (w.client, {}, true);
            check (w.client.planUnreadable() && badView.showRetry,
                   "UP3 ...and the card offers RETRY, which is the only honest road out of a plan that "
                   "cannot be read",
                   "planUnreadable=" + juce::String ((int) w.client.planUnreadable())
                   + " showRetry=" + juce::String ((int) badView.showRetry));
            check (bad != nullptr && bad->modelDetail.contains ("name no plugin")
                   && bad->modelDetail.contains ("\"op\""),
                   "UP4 ...and the op AS IT ARRIVED is kept behind Details, so the next one of these "
                   "answers whether the names were absent or spelled under a key we do not read - "
                   "evidence for B rather than a guess about B",
                   bad != nullptr ? bad->modelDetail.substring (0, 80) : juce::String());

            // ---- HIS 21:52: THAT BUILD IS A PLAN, NOT A ONE-LINE APPLY ---------------------
            //
            // *"A raw 'build N slots' step with Apply/Skip must not be offered for a build either.
            //  If the server sends a build op with no plan, show it as a plan needing Go, or refuse
            //  it plainly."*  The answer is SHOW IT (the reasoning is on buildPlanSlots): a readable
            // build is work he asked for, so it is drawn as a plan - a heading, a numbered line per
            // slot, and one door that says Go. His own row, from the log at 21:52:20.
            Rig b;
            b.client.start ("build a mix bus", "chat_2152");
            b.round (1);
            b.tool (R"EJ({"id":"tc_2152","name":"do","approval":"ask_first","summary":"build 5 slots: bx townhouse Buss Compressor, elysia alpha mix, elysia museq mix, MSaturator, bx clipper","args":{"op":"build","chain":[{"name":"bx townhouse Buss Compressor"},{"name":"elysia alpha mix"},{"name":"elysia museq mix"},{"name":"MSaturator"},{"name":"bx clipper"}]}})EJ");
            b.await ({ "tc_2152" });
            pumpMs (20);
            {
                const auto v = runViewFrom (b.client, {}, true);
                const echojay::agentui::RunStep* row = nullptr;
                for (const auto& st : v.steps) if (st.id == "tc_2152") row = &st;
                check (row != nullptr && ! row->label.containsIgnoreCase ("build 5 slots"),
                       "BP1 the row is no longer the server's raw \"build 5 slots: a, b, c, d, e\" line "
                       "- the comma list he had to parse is gone from the heading  (RED as it shipped "
                       "at 21:52)",
                       row != nullptr ? "\"" + row->label + "\"" : juce::String ("no row"));
                check (row != nullptr && row->buildPlan,
                       "BP2 ...the row is marked as a PLAN, which is the decision he asked for "
                       "(\"show it as a plan needing Go\") rather than a refusal of readable work",
                       row != nullptr ? juce::String ((int) row->buildPlan) : juce::String());
                // ONE NUMBERED LINE PER SLOT, in the chain's order and all five of them.
                bool allNumbered = row != nullptr;
                const char* want[] = { "1. bx townhouse Buss Compressor", "2. elysia alpha mix",
                                       "3. elysia museq mix", "4. MSaturator", "5. bx clipper" };
                int lastAt = -1;
                for (const auto* wn : want)
                {
                    const int at = row != nullptr ? row->detail.indexOf (wn) : -1;
                    if (at < 0 || at < lastAt) allNumbered = false;
                    lastAt = at;
                }
                check (allNumbered,
                       "BP3 ...and the slots are the row's OWN NUMBERED LINES, all five, in the chain's "
                       "order - which is what he can read at a glance on a narrow card and what the "
                       "comma list could not give him",
                       row != nullptr ? "\"" + row->detail.replaceCharacter ('\n', '/') + "\""
                                      : juce::String());
                // THE DOOR SAYS GO. The word lives on the ZONE, so this reads the same place the
                // painter does - a leg that read a literal in the painter would pass on a card
                // nobody can press.
                const auto L = layoutRun (v, 0, 0, 420);
                juce::String goWord, noWord;
                for (const auto& z : L.zones)
                {
                    if (z.kind == echojay::agentui::RunHit::ApplyLine) goWord = z.label;
                    if (z.kind == echojay::agentui::RunHit::SkipLine)  noWord = z.label;
                }
                check (goWord == "Go" && noWord == "Cancel",
                       "BP4 ...and the pair on the line reads [Go] [Cancel], the plan's own vocabulary, "
                       "instead of [Apply] [Skip]  (one door, as his #4 ruled - this changes the WORD "
                       "on it, not the number of them)",
                       "\"" + goWord + "\" / \"" + noWord + "\"");
                check (! goWord.isEmpty(),
                       "BP4b ...and the decision is still THERE - a plan he cannot say Go to would be "
                       "worse than the line he complained about",
                       "apply zone label \"" + goWord + "\"");
            }
            // ---- AND NOT WHEN THE SERVER ALREADY LAID THE PLAN OUT ------------------------
            //
            // His ruling is word for word about a build arriving WITH NO PLAN, and B's own fix of
            // 23 Oct makes every build a plan first: the plan card's items carry the server's own
            // per-step words ("elysia alpha mix for about 2 dB"). Re-heading those rows would throw
            // the better sentence away AND draw the plan twice - his test 8b from the other side.
            // So this leg sends the same build WITH a plan card and asserts we leave it alone.
            {
                Rig pc;
                pc.client.start ("build a mix bus", "chat_plancard");
                pc.round (1);
                pc.tool (R"EJ({"id":"tc_pc","name":"do","approval":"ask_first","summary":"elysia alpha mix for about 2 dB, then the EchoJay Limiter","args":{"op":"build","chain":[{"name":"elysia alpha mix"},{"name":"EchoJay Limiter"}]}})EJ");
                pc.plan (R"EJ({"round":1,"heading":"Measured: -14.3 LUFS, +1.5 dBTP.","items":[{"id":"tc_pc","summary":"elysia alpha mix for about 2 dB, then the EchoJay Limiter","why":"a build needs your ok","approval":"ask_first"}]})EJ");
                pc.await ({ "tc_pc" });
                pumpMs (20);
                const auto v = runViewFrom (pc.client, {}, true);
                const echojay::agentui::RunStep* row = nullptr;
                for (const auto& st : v.steps) if (st.id == "tc_pc") row = &st;
                check (row != nullptr && ! row->buildPlan
                       && row->label.contains ("elysia alpha mix for about 2 dB"),
                       "BP6 and a build that arrives WITH the server's plan card keeps the server's own "
                       "words on its row - one author for the prose, and the plan is not drawn twice "
                       "(his 21:52 ruling says \"with no plan\"; B's 23 Oct fix makes the plan the "
                       "ordinary road, so this is now the common case)",
                       row == nullptr ? juce::String ("no row")
                                      : "buildPlan=" + juce::String ((int) row->buildPlan)
                                        + ", label \"" + row->label + "\"");
            }

            // AND AN ORDINARY OP IS UNTOUCHED: an `add` awaiting approval keeps Apply/Skip. The rule
            // is about builds, and a change that quietly renamed every button would be a bigger
            // fault than the one it fixed.
            {
                Rig a;
                a.client.start ("add a compressor", "chat_add");
                a.round (1);
                a.tool (R"EJ({"id":"tc_add","name":"do","approval":"ask_first","summary":"add EchoJay Compressor","args":{"op":"add","name":"EchoJay Compressor"}})EJ");
                a.await ({ "tc_add" });
                pumpMs (20);
                const auto v = runViewFrom (a.client, {}, true);
                const auto L = layoutRun (v, 0, 0, 420);
                juce::String w1, w2;
                for (const auto& z : L.zones)
                {
                    if (z.kind == echojay::agentui::RunHit::ApplyLine) w1 = z.label;
                    if (z.kind == echojay::agentui::RunHit::SkipLine)  w2 = z.label;
                }
                bool planned = false;
                for (const auto& st : v.steps) if (st.buildPlan) planned = true;
                check (! planned && w1 == "Apply" && w2 == "Skip",
                       "BP5 and a plain `add` awaiting his decision still reads [Apply] [Skip] with no "
                       "plan heading - the ruling is about BUILDS, and the other direction is what "
                       "makes it a rule rather than a rename",
                       "\"" + w1 + "\" / \"" + w2 + "\", buildPlan=" + juce::String ((int) planned));
            }

            // THE OTHER DIRECTION, and it is the one that matters most: an ordinary build is still
            // offered. A rule that refused every build would be worse than the fault it fixes.
            Rig g;
            g.client.start ("build a mix bus", "chat_readable");
            g.round (1);
            g.tool (R"EJ({"id":"tc_good","name":"do","approval":"ask_first","summary":"build a mix bus","args":{"op":"build","chain":[{"name":"EchoJay EQ"},{"name":"EchoJay Compressor"},{"name":"EchoJay Limiter"}]}})EJ");
            g.await ({ "tc_good" });
            pumpMs (20);
            const auto* good = g.step ("tc_good");
            check (good != nullptr && good->status == EJAgentClient::Step::Status::AwaitingApproval
                   && ! g.client.planUnreadable(),
                   "UP5 and a build whose every slot names a plugin is offered exactly as before - "
                   "three named slots, awaiting his decision",
                   good == nullptr ? juce::String ("no step")
                                   : juce::String ("status ") + juce::String ((int) good->status)
                                     + ", unreadable=" + juce::String ((int) g.client.planUnreadable()));
        }
        {
            // ---- B'S KEEP-PLAYING CONTRACT (docs/CONTRACT_keep_playing_2026-10-22.md) -----------
            //
            // *"While any slot from this run has keep_playing and its dial_on_playback has not
            //  finished, never show 'Got it, you can stop' (or any stop prompt)."*
            //
            // Sean's test 9, 15:24: the listen had its 8 seconds, the card said he could stop, and
            // the plan's second compressor was still to set itself on that same playback. The
            // minimum is about having heard ENOUGH - it says nothing about whether the rack has
            // finished using what it heard.
            //
            // The executor is the one that knows, so this leg makes it say "the bx_opto is still
            // setting itself" and then stops saying it, which is the only two states that matter.
            Rig w;
            w.ex.keepPlaying = "bx_opto Pedal";        // a dial from this run is still working
            w.ex.reading.transportKnown    = true;
            w.ex.reading.playing           = true;
            w.ex.reading.heardAboveSeconds = 12.0f;    // well past any minimum
            w.client.start ("build a vocal chain with two compressors", "chat_kp");
            w.round (1);
            w.tool (R"EJ({"id":"tc_wait","name":"check","summary":"wait for playback","approval":"free","args":{"what":"playback","min_seconds":8}})EJ");
            w.await ({ "tc_wait" });
            pumpMs (120);
            // THE LINE HE WATCHES IS THE PLAYBACK BLOCK'S, NOT THE STEP'S DETAIL. The client writes
            // the step's detail at the END, with the outcome (19 Oct: "'Listening... 3 of 8 s' draws
            // twice" - the progress line belongs to one surface while the wait is live). So this
            // reads what the card actually draws: RunView::playbackLine. My first cut read the step
            // and failed on a line the product was right not to have written yet.
            const auto heldLine = runViewFrom (w.client, {}, true).playbackLine;
            check (w.client.state() == EJAgentClient::State::Listening
                   && ! heldLine.containsIgnoreCase ("you can stop"),
                   "KP1 the minimum is reached and the run does NOT say \"you can stop\" while a "
                   "keep_playing dial is still setting itself  (RED as it shipped at 15:24: the listen "
                   "ended on the minimum alone and the second compressor was still to be dialled)",
                   "state " + juce::String (EJAgentClient::stateName (w.client.state()))
                   + ", line \"" + heldLine + "\"");
            check (heldLine.containsIgnoreCase ("bx_opto Pedal"),
                   "KP2 ...and it says WHICH plugin it is waiting for, so he knows why he is still "
                   "playing (the contract's own wording: \"I'll say when it's done\")",
                   "line \"" + heldLine + "\"");
            // ...AND IT LETS GO. The dial lands; the next tick may say what it always said.
            w.ex.keepPlaying = {};
            pumpMs (120);
            const auto* freedStep = w.step ("tc_wait");
            const auto freedLine = freedStep != nullptr ? freedStep->detail : juce::String();
            juce::ignoreUnused (freedLine);
            check (freedLine.containsIgnoreCase ("you can stop")
                   || w.client.state() != EJAgentClient::State::Listening,
                   "KP3 and when the dial has landed the listen ends as it always did - the contract "
                   "holds the prompt, it does not remove it  (a rule that only ever withheld would be "
                   "a run he could never stop playing for)",
                   "state " + juce::String (EJAgentClient::stateName (w.client.state()))
                   + ", line \"" + freedLine + "\"");
        }
        {
            // ---- THE READ-BACK WAITS FOR THE PLUGIN'S REPORT (his test 4 at 08:40, 23 Oct) ----
            //
            // *"the Tube-Tech CL 1B step said 'Settings not confirmed: the plugin had not reported
            //  them yet', although the compressor was clearly working. The read-back ran before the
            //  plugin reported its values. Wait for the plugin's report, within a bound."*
            //
            // His log: "Tube-Tech CL 1B loaded at host sampleRate" and the landed line "Settings not
            // confirmed" are BOTH at 08:40:20, and the "re-reading in 800 ms (24 more looks)" line
            // three seconds later belongs to the next add. Zero looks on the one that needed them.
            using echojay::agent::readBackShouldLookAgain;
            check (readBackShouldLookAgain (2, /*matched*/ false, /*missed*/ false, /*unreadable*/ false),
                   "RB1 a plugin that has reported NOTHING against 2 asked settings is a reason to "
                   "look again  (RED as it shipped at 08:40: the condition only re-read what had "
                   "been read and was wrong, so the one case that most needed another look got none)",
                   "looks again = yes");
            check (readBackShouldLookAgain (1, false, true, false)
                   && readBackShouldLookAgain (1, false, false, true),
                   "RB2 ...and the two reasons it always had still hold: a value read back WRONG, or "
                   "one known to be unreadable",
                   "both yes");
            check (! readBackShouldLookAgain (2, /*matched*/ true, false, false),
                   "RB3 ...and a clean read finishes AT ONCE - a plugin that answers immediately "
                   "never reaches this test, which is what keeps the fix from costing every add 800 ms",
                   "looks again = no");
            check (! readBackShouldLookAgain (0, false, false, false),
                   "RB4 ...and an add that asked for NOTHING waits for nothing: there is no report "
                   "owed, so \"nothing reported\" is not a reason to look  (an add with no settings "
                   "has its own door, and this would have held it open for 20 s)",
                   "looks again = no");
        }
        {
            // ---- "YOU CAN STOP" IS ONLY SAID WHEN IT IS TRUE (his test 0 point 2, 08:10) -------
            //
            // *"The first listen still says 'Got it, you can stop.' on a build goal, and the plan
            //  then asks me to keep playing. On a build goal, the first listen says 'Got it' only,
            //  never 'you can stop'; the stop line only comes when nothing in the run will need
            //  playback."*
            //
            // His 08:08:29 and 08:10:19, in one fixture: a listen early in a build run, then the
            // same run's closing listen. BOTH DIRECTIONS IN ONE PLACE, because the rule is not
            // "never say it" - a run he can never stop playing for would be the fault with the sign
            // flipped, and it is the shape KP3 already guards for the dial.
            Rig e;
            e.ex.reading.transportKnown    = true;
            e.ex.reading.playing           = true;
            e.ex.reading.heardAboveSeconds = 12.0f;
            e.client.start ("build a mix bus", "chat_stopinvite");
            e.round (1);
            e.tool (R"EJ({"id":"tc_first","name":"check","summary":"wait for playback","approval":"free","args":{"what":"playback","min_seconds":8}})EJ");
            e.await ({ "tc_first" });
            pumpMs (120);
            const auto firstLine = e.step ("tc_first") != nullptr ? e.step ("tc_first")->detail : juce::String();
            check (firstLine.containsIgnoreCase ("got it")
                   && ! firstLine.containsIgnoreCase ("you can stop"),
                   "KS1 the FIRST listen of a build run says \"Got it.\" and nothing more - the run has "
                   "its plan still to write and will ask for playback again  (RED as it shipped at "
                   "08:08:29: \"Got it, you can stop.\", and round 10's plan then ended \"Keep it "
                   "playing while the Pro Audio DSP DSM V3 sets itself\")",
                   "\"" + firstLine + "\"");
            const auto vFirst = runViewFrom (e.client, {}, true);
            check (! vFirst.canStopPlaying,
                   "KS1b ...and the BUTTON agrees with the line, because both ask one function - two "
                   "surfaces disagreeing about one fact is how this got on screen",
                   "canStopPlaying=" + juce::String ((int) vFirst.canStopPlaying));
            // ...AND THEN THE CLOSING, which is the one moment the run can promise it. B's own
            // marker for it: a talk ask whose id starts tc_check (section 86(1)).
            e.round (2);
            e.tool (R"EJ({"id":"tc_check_done","name":"talk","approval":"free","args":{"ask":{"question":"That's the chain in and set. How's it sounding?","choices":[{"label":"Sounds good"},{"label":"Run a check"}],"allowFreeText":true}}})EJ");
            e.await ({ "tc_check_done" });
            pumpMs (20);
            e.client.answerAsk ("Run a check");
            e.round (3);
            e.tool (R"EJ({"id":"tc_last","name":"check","summary":"wait for playback","approval":"free","args":{"what":"playback","min_seconds":8}})EJ");
            e.await ({ "tc_last" });
            pumpMs (120);
            const auto lastLine = e.step ("tc_last") != nullptr ? e.step ("tc_last")->detail : juce::String();
            check (lastLine.containsIgnoreCase ("you can stop"),
                   "KS2 ...and the CLOSING listen does say \"you can stop\" - by then the run has shown "
                   "its closing and nothing else is coming, so the sentence is true  (a rule that only "
                   "ever withheld it would leave him playing for a run that had finished)",
                   "\"" + lastLine + "\"");
        }
        {
            // ---- THE CONTRACT'S POINT 2: THE DIAL IS ITS OWN STEP (his overnight item 4) -------
            //
            // *"the dial on playback appears as its own numbered step on the card"* - because at
            // 15:24 the ONLY sign that a compressor was still setting itself was the listen refusing
            // to end, and a card that holds him on playback without a line saying what for is a card
            // that looks stuck. Point 1 (above) is the prompt it withholds; this is the line he reads
            // while it happens, and both answer the SAME function on the executor so they cannot
            // disagree - which is what the legs below pin, in both directions.
            Rig w;
            w.ex.keepPlaying = "bx_opto Pedal";
            w.ex.reading.transportKnown    = true;
            w.ex.reading.playing           = true;
            w.ex.reading.heardAboveSeconds = 12.0f;
            w.client.start ("build a vocal chain with two compressors", "chat_kp2");
            w.round (1);
            w.tool (R"EJ({"id":"tc_wait2","name":"check","summary":"wait for playback","approval":"free","args":{"what":"playback","min_seconds":8}})EJ");
            w.await ({ "tc_wait2" });
            pumpMs (120);
            {
                const auto v = runViewFrom (w.client, {}, true);
                const echojay::agentui::RunStep* dial = nullptr;
                for (const auto& st : v.steps)
                    if (st.id == "dial:bx_opto Pedal") dial = &st;
                check (dial != nullptr,
                       "KP4 the dial on playback is a STEP OF ITS OWN on the card, not only a line in "
                       "the playback block  (RED as it shipped: the card had no row for it at all, so "
                       "the dial was invisible unless he read the log)",
                       dial != nullptr ? juce::String ("id \"") + dial->id + "\""
                                       : juce::String ("no dial step among ")
                                         + juce::String ((int) v.steps.size()) + " steps");
                check (dial != nullptr && dial->label.contains ("bx_opto Pedal"),
                       "KP4b ...and the step NAMES THE PLUGIN, as the contract asks - \"the dial\" with "
                       "no name is the same silence in longer words",
                       dial != nullptr ? "\"" + dial->label + "\"" : juce::String());
                check (dial != nullptr && dial->status == (int) EJAgentClient::Step::Status::Running,
                       "KP4c ...and it is RUNNING, so it draws as work in flight rather than something "
                       "done or waiting on him - he has nothing to answer, he has only to keep playing",
                       dial != nullptr ? juce::String ("status ") + juce::String (dial->status)
                                       : juce::String());
                check (! v.steps.empty() && v.steps.back().id == "dial:bx_opto Pedal",
                       "KP4d ...and it is the LAST step, so its number is one past the steps that did "
                       "the work - a dial that renumbered the plan would move the rows he is reading",
                       v.steps.empty() ? juce::String ("no steps")
                                       : juce::String ("last is \"") + v.steps.back().id + "\"");
            }
            // ---- THE DIAL LANDS: THE STEP FINISHES AND SHOWS ITS RESULT (his 08:43 ruling) --
            //
            // *"the dial-on-playback result arrived as a loose message below the card ... the dial
            //  step finishes and shows its result, then the card says it's done. Nothing appears
            //  outside the card."*
            //
            // RE-AIMED FROM "THE STEP GOES", which is what I built this morning before his 08:43
            // report and which would have thrown the result away: the row vanished and the only
            // copy of "Adjusted the Mixland Vac Attack's Left Reduction to +0.0 dB" was the chat
            // bubble he was complaining about. The step stays; it stops ASKING and starts TELLING.
            // ---- AND ITS STEP SITS WITH ITS ADD (his test 3 of 16:40, 23 Oct 2026) ---------
            //
            // *"On the card, each dial is its own step right after its add."*  The dial steps were
            // appended after everything else, so a four-plugin build read as "four adds, then the
            // dials" instead of as the order the work happens in. Two adds here, the dial armed for
            // the FIRST one: its step has to be the row after that add and not the last row on the
            // card. KP4d's rule (a dial never renumbers the plan) is unchanged for a dial whose add
            // is not on the card - that is the tail sweep, and KP5 below still measures it.
            {
                Rig w2;
                w2.ex.keepPlaying = "bx_opto Pedal";
                w2.client.start ("build a vocal chain", "chat_kp6");
                w2.round (1);
                w2.tool (R"EJ({"id":"tc_add_opto","name":"do","approval":"free","args":{"op":"add","name":"bx_opto Pedal"}})EJ");
                w2.tool (R"EJ({"id":"tc_add_vsm","name":"do","approval":"free","args":{"op":"add","name":"Vertigo VSM-3"}})EJ");
                w2.await ({ "tc_add_opto", "tc_add_vsm" });
                pumpMs (60);
                const auto v6 = runViewFrom (w2.client, {}, true);
                int atAdd = -1, atDial = -1, atVsm = -1;
                for (int k = 0; k < (int) v6.steps.size(); ++k)
                {
                    if (v6.steps[(size_t) k].id == "tc_add_opto")        atAdd  = k;
                    if (v6.steps[(size_t) k].id == "dial:bx_opto Pedal") atDial = k;
                    if (v6.steps[(size_t) k].id == "tc_add_vsm")         atVsm  = k;
                }
                juce::StringArray order;
                for (const auto& st : v6.steps) order.add (st.id);
                check (atAdd >= 0 && atDial == atAdd + 1 && atVsm > atDial,
                       "KP6 the dial's step is the row straight after the add that armed it, not the "
                       "last row on the card  (his 16:40: \"each dial is its own step right after its "
                       "add\" - they were all appended at the end, so a build read as every add and "
                       "then every dial)",
                       "order: " + order.joinIntoString (" | "));
                check (atDial >= 0 && v6.steps[(size_t) atDial].status
                                        == (int) EJAgentClient::Step::Status::Running,
                       "KP6b ...and it is RUNNING there while he plays, so the row he is reading is the "
                       "one doing the work",
                       atDial < 0 ? juce::String ("no dial step")
                                  : juce::String ("status ") + juce::String (v6.steps[(size_t) atDial].status));
            }
            w.ex.keepPlaying = {};
            w.ex.dialsLanded.set ("bx_opto Pedal",
                                  "Adjusted the bx_opto Pedal's Drive to +1.4 dB - it is working "
                                  "1.1 dB.");
            pumpMs (120);
            {
                const auto v = runViewFrom (w.client, {}, true);
                const echojay::agentui::RunStep* dial = nullptr;
                for (const auto& st : v.steps)
                    if (st.id == "dial:bx_opto Pedal") dial = &st;
                check (dial != nullptr && dial->status == (int) EJAgentClient::Step::Status::Done,
                       "KP5 when the dial lands its step FINISHES - it is still there, now done, so "
                       "the card stops asking him to keep playing without losing the row  (RED as it "
                       "shipped at 08:43: there was no dial row at all and the result was posted as a "
                       "chat line BELOW the card)",
                       dial == nullptr ? juce::String ("no dial step")
                                       : juce::String ("status ") + juce::String (dial->status));
                check (dial != nullptr && dial->detail.contains ("working 1.1 dB"),
                       "KP5b ...and it carries the DIAL'S OWN SENTENCE as its result - the dial wrote "
                       "it, the step shows it, and nothing composes a second wording of it",
                       dial != nullptr ? "\"" + dial->detail + "\"" : juce::String());
                check (! dial->label.containsIgnoreCase ("keep it playing"),
                       "KP5c ...and it no longer asks him to keep playing, because it is done",
                       dial != nullptr ? "\"" + dial->label + "\"" : juce::String());
                // ONE ROW PER PLUGIN, not one asking and one telling.
                int dialRows = 0;
                for (const auto& st : v.steps) if (st.id.startsWith ("dial:")) ++dialRows;
                check (dialRows == 1,
                       "KP5d ...and there is exactly ONE row for that dial, not a finished one beside "
                       "a still-asking one - the row is keyed by the plugin so the two states cannot "
                       "both be drawn",
                       juce::String (dialRows) + " dial row(s)");
            }
        }
        {
            // ---- THE PLAN, ONCE (his test 8, 22 Oct 2026) ---------------------------------------
            //
            // *"the plan is shown TWICE. Once inside the agent step with the Go / Change / Cancel
            //  chips, then again in full as a plain message below the card."*
            //
            // Z4's exact-equality rule could not see this one: B sends the plan as the talk's TEXT
            // and the question beside it, the client PREPENDS that text to the question (so the card
            // reads as one sentence pair), and the step's label was composed earlier from the call.
            // Two spellings of one sentence. The count is what is asserted here - a distinctive
            // phrase from the plan may appear exactly ONCE anywhere the card draws prose - because
            // "shown twice" is a claim about the whole card and not about one field.
            Rig w;
            w.client.start ("build a vocal chain", "chat_plan");
            w.round (1);
            w.tool (R"EJ({"id":"tc_plan","name":"talk","summary":"the plan","approval":"free","args":{"text":"From what was measured: the bus sits at -12 LUFS and the low mids run hot. The plan is a compressor, an EQ and a limiter.","ask":{"question":"Go, change it, or cancel?","choices":[{"label":"Go"},{"label":"Change"},{"label":"Cancel"}],"allowFreeText":false}}})EJ");
            w.await ({ "tc_plan" });
            pumpMs (10);
            const auto v = runViewFrom (w.client, {}, true);
            const juce::String phrase = "From what was measured";
            int saidTimes = 0;
            if (v.opening.contains (phrase)) ++saidTimes;
            if (v.closing.contains (phrase)) ++saidTimes;
            if (v.askQuestion.contains (phrase)) ++saidTimes;
            for (const auto& s : v.steps)
            {
                if (s.label.contains (phrase))    ++saidTimes;
                if (s.detail.contains (phrase))   ++saidTimes;
                if (s.answered.contains (phrase)) ++saidTimes;
            }
            check (saidTimes == 1,
                   "Z4c the plan is drawn ONCE across the whole card  (RED as it shipped at 15:20: the "
                   "step's line and the question block below it both carried it, because the client "
                   "prepends the talk's text to the question and the exact-match rule saw two "
                   "different strings)",
                   "said " + juce::String (saidTimes) + " time(s); block=\"" + v.askQuestion.substring (0, 40)
                   + "\" rows=" + juce::String ((int) v.steps.size()));
            // ...AND IT IS THE ROW THAT HAS IT, with the chips, which is the ruling the clearing
            // serves ("the row wins, it carries the chips" - 18 Oct). A rule that cleared the block
            // and left the plan nowhere would pass the count above and lose his plan.
            bool onARowWithChips = false;
            for (const auto& s : v.steps)
                if (s.label.contains (phrase) && s.chips.size() == 3) onARowWithChips = true;
            check (onARowWithChips,
                   "Z4d ...and the one place it is drawn is the STEP that carries Go / Change / Cancel "
                   "- not the block, and not nowhere",
                   "steps " + juce::String ((int) v.steps.size()) + ", chips on the plan's row: "
                   + (v.steps.empty() ? juce::String ("-") : v.steps.back().chips.joinIntoString (", ")));
            check (v.askQuestion.isEmpty()
                   && (! v.steps.empty() && v.steps.back().label.contains ("Go, change it, or cancel?")),
                   "Z4e ...and the question itself is still there, on that same row - the block went, "
                   "the sentence did not",
                   "block=\"" + v.askQuestion + "\", row=\""
                   + (v.steps.empty() ? juce::String() : v.steps.back().label.substring (0, 80)) + "\"");
        }
        {
            // ---- THE PLAN ONCE, ON THE SURFACE THE 22 OCT FIX COULD NOT SEE (his test 0, 08:10) --
            //
            // *"The plan is shown TWICE again: inside the step with Go / Change chips, then in full
            //  as plain text below the card. Your plan-once render was meant to be in this build."*
            //
            // WHY IT DID NOT CATCH THIS ONE, and it is worth writing down: Z4c counts the copies on
            // the CARD - opening, closing, the question block, every step's label and detail - and
            // the second copy here is not on the card at all. It is the chat row's own prose
            // (ChatMsg::content), which the client writes by flushing the ROUND'S STREAMED TEXT as
            // an assistant line. B sends the plan twice by design and correctly: once as the deltas
            // that stream it while the model writes, and once as the talk's ask, which is the
            // formal version with the chips. The card draws the ask; the chat row drew the deltas;
            // neither knew about the other. Z4c could never see it because `opening` is the
            // EDITOR's field and runViewFrom leaves it empty - the guard was reading a surface the
            // fault does not live on.
            //
            // So this leg reads the TRANSCRIPT, which is the road to that bubble, and it is his
            // 08:10 shape exactly: the plan streamed, then the same plan as the ask, then Go.
            Rig d;
            d.client.start ("build a mix bus", "chat_test0");
            d.round (1);
            const char* kPlanProse =
                "Measured: the loudest 3 s sit around -13.4 LUFS, peaks are hitting +1.3 dBTP. "
                "Here's the plan for the mix bus: 1. Solid Bus Comp at 4 for about 1 dB. "
                "2. Pro Audio DSP DSM V3, set on the first playback. 3. bx clipper, a light clip. "
                "4. EchoJay Limiter for -8 LUFS, ceiling -0.1 dBTP.";
            d.delta (kPlanProse);                       // the model writing it, as it streams
            d.tool (juce::String (R"EJ({"id":"tc_plan0","name":"talk","approval":"free","args":{"ask":{"question":")EJ")
                    + kPlanProse + " Go?\",\"choices\":[{\"label\":\"Go\"},{\"label\":\"Change limiter\"},{\"label\":\"Cancel\"}],\"allowFreeText\":true}}}");
            d.await ({ "tc_plan0" });
            pumpMs (20);
            d.client.answerAsk ("Go");                  // his press at 08:10:34 - this posts the step
            pumpMs (20);
            int proseLines = 0;
            juce::String firstProse;
            for (const auto& t : d.transcript)
                if (t.first == "assistant" && t.second.contains ("Here's the plan for the mix bus"))
                { ++proseLines; if (firstProse.isEmpty()) firstProse = t.second; }
            const auto v = runViewFrom (d.client, {}, true);
            bool onTheCard = false;
            for (const auto& st : v.steps)
                if (st.label.contains ("Here's the plan for the mix bus")) onTheCard = true;
            check (onTheCard,
                   "Z4f the plan is on the card, in the step that carried the chips (the half that was "
                   "always right, and the ground for the next leg)",
                   "steps " + juce::String ((int) v.steps.size()));
            check (proseLines == 0,
                   "Z4g ...and the SAME plan is not also sent to the chat as a plain assistant line - "
                   "which is the second copy he read below the card at 08:10  (RED as it shipped: the "
                   "round's streamed prose was flushed as a chat line whatever the card had already "
                   "drawn, so B sending the plan as deltas AND as the ask put it on screen twice)",
                   juce::String (proseLines) + " plain line(s); first \""
                   + firstProse.substring (0, 70) + "\"");
            // ---- B'S FRAME EXACTLY AS HE SENDS IT, AND TWO SPELLINGS (his test 1, 16:24) -----
            //
            // *"the plan is STILL shown twice, in the card with chips and then in full as plain text
            //  below the card ... B confirms the server sends the plan once: the plan card's
            //  tool_call carries the text, and the message frame for it has EMPTY text and only
            //  'of' pointing at the card. So the copy below is drawn by the plugin."*
            //
            // So this leg is the WIRE he had, not an approximation of it: the talk's ask carries the
            // plan, and B's message frame follows with empty text and `of`. Z4f/Z4g above hold the
            // flush road (deltas), which that session's log shows was not involved at all - this is
            // the other road, the card's own two fields.
            //
            // AND THE TWO SPELLINGS ARE THE POINT. The row's label and the ask's question differed
            // by one character in the middle ("4. bx clipper" against "4. bx_clipper"), which is
            // exactly what containment cannot see: with the prose test in place this leg is RED,
            // and it stays red for any other wording the two roads do not share, which is why the
            // fix is identity and not a wider normaliser.
            {
                Rig d3;
                d3.client.start ("build a mix bus", "chat_test1");
                d3.round (6);
                // The row's spelling - this is what describeCall composes the label from.
                const char* kRowSpelling =
                    "Build this mix bus chain?\n"
                    "1. EchoJay EQ - cut about 1 dB at 300 Hz\n"
                    "2. SSL Native Bus Compressor 2 at 2:1, Threshold 20 for about 2 dB\n"
                    "3. Shadow Hills Mastering Compressor, set on the first playback\n"
                    "4. bx clipper - a light clip, about 1.5 dB off the peaks\n"
                    "5. EchoJay Limiter for -8 LUFS, ceiling -0.1 dBTP. Go?";
                // The ask's spelling: one character different, in the middle. His session's own pair.
                const juce::String kAskSpelling = juce::String (kRowSpelling).replace ("bx clipper", "bx_clipper");
                // THE CALL'S OWN SUMMARY carries the row's spelling and the ASK carries the other,
                // which is the pair his log shows: the frame logged "4. bx clipper" as the step's
                // label while the ask card read "4. bx_clipper". No `text`, because that session's
                // talk had none - the whole plan was in the ask.
                d3.tool (juce::String (R"EJ({"id":"tc_pick_look_ask_6_1","name":"talk","approval":"free","summary":")EJ")
                         + kRowSpelling + R"EJ(","args":{"ask":{"question":")EJ" + kAskSpelling
                         + R"EJ(","choices":[{"label":"Go"},{"label":"Change limiter"},{"label":"Cancel"}],"allowFreeText":true}}})EJ");
                // B'S MESSAGE FRAME FOR THAT CARD: empty text, `of` pointing at the call. The client
                // must take nothing from it - and this is the frame the fault was blamed on first.
                EJAgentClientTestAccess::feed (d3.client, "message",
                                               R"({"text":"","of":"tc_pick_look_ask_6_1"})");
                d3.await ({ "tc_pick_look_ask_6_1" });
                pumpMs (20);
                const auto v3 = runViewFrom (d3.client, {}, true);
                // COUNT THE COPIES, on every surface the card has and in the chat: the question
                // block, every step's label and detail, and the transcript's assistant lines.
                int copies = 0;
                const auto counts = [] (const juce::String& t)
                { return t.contains ("Build this mix bus chain?") ? 1 : 0; };
                copies += counts (v3.askQuestion);
                for (const auto& st : v3.steps) copies += counts (st.label) + counts (st.detail);
                int chatCopies = 0;
                for (const auto& t : d3.transcript) if (counts (t.second)) ++chatCopies;
                check (copies == 1,
                       "Z4i the plan is drawn ONCE on the card, with B's frame exactly as he sends it "
                       "(the ask carries the plan; the message frame has empty text and only `of`) and "
                       "the row and the question spelled one character apart  (RED as it shipped at "
                       "16:24: the row said \"4. bx clipper\" and the question \"4. bx_clipper\", so "
                       "the containment test kept both - prose standing in for identity)",
                       juce::String (copies) + " cop(ies) on the card, block=\""
                       + v3.askQuestion.substring (0, 40) + "\"");
                check (chatCopies == 0,
                       "Z4j ...and none of it reaches the chat either: B's empty message frame is not "
                       "a sentence, and nothing on the card is repeated below it",
                       juce::String (chatCopies) + " chat line(s)");
                bool chipsOnTheCopy = false;
                for (const auto& st : v3.steps)
                    if (counts (st.label) && ! st.chips.isEmpty()) chipsOnTheCopy = true;
                check (chipsOnTheCopy,
                       "Z4k ...and the one copy is the row that carries the chips, not a block he "
                       "cannot answer (his 18 Oct ruling: the row wins, it carries the chips)",
                       chipsOnTheCopy ? "chips on the plan's row" : "the plan is on a row with no chips");
            }
            // AND NOT BY SILENCING THE ROUND'S PROSE IN GENERAL: a round whose streamed text is its
            // own (a sentence the card does not carry) still reaches the chat, or every run loses
            // its words.
            Rig d2;
            d2.client.start ("what is on this rack?", "chat_test0b");
            d2.round (1);
            d2.delta ("I had a look at the rack: there are four slots, and the compressor on slot 2 "
                      "is doing most of the work on the loud phrases right now.");
            d2.tool (R"EJ({"id":"tc_look0","name":"look","approval":"free","args":{"what":"rack"}})EJ");
            d2.await ({ "tc_look0" });
            pumpMs (20);
            bool kept = false;
            for (const auto& t : d2.transcript)
                if (t.first == "assistant" && t.second.contains ("there are four slots")) kept = true;
            check (kept,
                   "Z4h and a round whose streamed prose is NOT on the card still reaches the chat - the "
                   "rule drops a REPEAT, it does not silence the run  (the fault with the sign flipped: "
                   "a plugin that swallowed the model's words would be worse than one that said them "
                   "twice)",
                   "assistant lines: " + juce::String (d2.assistantLines()));
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

    // ---- MOV: A MESSAGE TYPED IN ANOTHER CHAT (Sean's ruling, 20 Oct 2026) --------------------
    //
    // *"The "agent is still working on build a mix bus. Stop it first, or wait for it to finish."
    // notice appeared in the MIX BUS chat, not the Link chat he typed in, and his text was thrown
    // away. That run was only waiting on its loudness ask, not working."*
    //
    //   1. a run only WAITING does not block: it is stopped ("Stopped: you moved on to <channel>")
    //      and the new run starts in the chat he typed in;
    //   2. a run actually EXECUTING refuses, with the notice in the chat he typed in;
    //   3. a typed message is never dropped silently.
    //
    // The state half is EJAgentClient::stateBlocksANewMessage, walked exhaustively below: the old
    // gate asked askPending(), and his run was sitting on the LOUDNESS loop's question - a different
    // ask, the same wait - so it did not apply. "What kind of wait is it?" was the wrong question.
    {
        std::printf ("== MOV: waiting does not block; working does ==\n");
        using S = EJAgentClient::State;
        // THE FOUR THAT BLOCK: genuinely mid-flight, where two conversations would race the rack.
        for (auto st : { S::Connecting, S::Streaming, S::Executing, S::Posting })
            check (EJAgentClient::stateBlocksANewMessage (st),
                   juce::String ("MOV1 ") + EJAgentClient::stateName (st)
                     + " BLOCKS a new message - the agent is mid-flight");
        // THE THREE THAT WAIT ON HIM: a plan awaiting Go, an ask, a wait for playback. His run was
        // in one of these and was told it was "still working".
        for (auto st : { S::AwaitingApproval, S::AwaitingAsk, S::Listening })
            check (! EJAgentClient::stateBlocksANewMessage (st),
                   juce::String ("MOV2 ") + EJAgentClient::stateName (st)
                     + " does NOT block - it is waiting on him, which is a question in another "
                       "shape (his ruling 1)");
        // AND THE ENDED ONES, which must never block either.
        for (auto st : { S::Idle, S::Done, S::Stopped, S::Failed })
            check (! EJAgentClient::stateBlocksANewMessage (st),
                   juce::String ("MOV3 ") + EJAgentClient::stateName (st) + " does not block");

        // RULING 1, end to end: a run waiting in chat A, a message typed in chat B. The run's own
        // chat says it is not for this chat, the run is not working, so it is stopped WITH A REASON
        // its card shows - and the reason is consumed, so the next stop says the plain thing.
        {
            Rig r;
            r.client.start ("build a mix bus", "chatMixBus");
            check (r.client.runChatId() == "chatMixBus",
                   "MOV4 the run knows its chat", r.client.runChatId());
            check (! echojay::agentui::rowBelongsInChat (r.client.runChatId(), "chatLink"),
                   "MOV5 ...so a message typed in the Link's chat is not for it");
            r.transcript.clear();
            r.client.stopWithReason ("you moved on to Aitch_4_01");
            bool said = false;
            for (const auto& t : r.transcript)
                if (t.second.contains ("Stopped: you moved on to Aitch_4_01")) said = true;
            check (said, "MOV6 the waiting run's OWN card says why it stopped - \"Stopped: you moved "
                         "on to <channel>\", his ruling 1",
                   r.transcript.empty() ? juce::String ("(nothing said)") : r.transcript.back().second);
            check (r.client.state() == EJAgentClient::State::Stopped,
                   "MOV7 ...and it really is stopped, so the next message starts its own run");
        }
        // AND THE REASON DOES NOT LEAK ONTO THE NEXT STOP: a plain Stop press says "Stopped."
        {
            Rig r2;
            r2.client.start ("add a compressor", "chatLink");
            r2.transcript.clear();
            r2.client.stop();
            bool plain = false, leaked = false;
            for (const auto& t : r2.transcript)
            {
                if (t.second.trim() == "Stopped.") plain = true;
                if (t.second.contains ("moved on to")) leaked = true;
            }
            check (plain && ! leaked,
                   "MOV8 a plain Stop still says \"Stopped.\" - a reason cannot outlive the stop it "
                   "describes and turn up on the next one",
                   r2.transcript.empty() ? juce::String ("(nothing said)") : r2.transcript.back().second);
        }
    }

    // ---- AD: THE AUTO-DIAL-ONLY TOGGLE RIDES BOTH AGENT REQUESTS (Sean's 09:49, 21 Oct 2026) --
    //
    // *"the agent added an unmapped plugin (bx opto Pedal) while his setting "Only suggest plugins
    // EchoJay can auto-dial" was on. Confirm whether that setting is sent on agent start and step
    // requests (as it is on agent off). If not, send it, under the same key agent off uses."*
    //
    // It was sent on NEITHER: `autoDial` existed in exactly one request path, EchoJayAPI's chat
    // body, and in no agent file at all - so the server picked plugins for an agent build without
    // knowing the toggle was on. A STEP matters as much as a START: a round that picks at round 3
    // can pick an unmapped plugin just as well as one that picks at round 1.
    {
        std::printf ("== AD: autoDial on start AND step, same key as agent off ==\n");
        {
            Rig r;
            r.ex.autoDialOnlyAnswer = true;
            r.ex.genreAnswer = "Hip-Hop";
            r.client.start ("add a compressor", "chatA");
            check (! r.posts.empty() && r.posts.front().first == "/api/agent/start",
                   "AD0 the fixture posted a start");
            const auto startBody = juce::JSON::parse (r.posts.front().second);
            check ((bool) startBody.getProperty ("autoDial", false),
                   "AD1 the START body carries autoDial:true under the chat road's own key",
                   r.posts.front().second.substring (0, 160));
            check (startBody.getProperty ("genre", juce::var()).toString() == "Hip-Hop",
                   "AD1b ...and the GENRE, under the chat road's own key (B, 21 Oct: B's mix-bus "
                   "loudness default depends on it and it arrived empty)",
                   startBody.getProperty ("genre", juce::var()).toString());
        }
        // ...and OFF sends nothing at all, so an older server and a body with the toggle off are
        // byte-for-byte what they were.
        {
            Rig r2;
            r2.ex.autoDialOnlyAnswer = false;
            r2.client.start ("add a compressor", "chatA");
            const auto b = juce::JSON::parse (r2.posts.front().second);
            check (! b.hasProperty ("autoDial"),
                   "AD2 with the toggle OFF the key is ABSENT, not false - an older body unchanged",
                   r2.posts.front().second.substring (0, 160));
            check (! b.hasProperty ("genre"),
                   "AD2b ...and an UNSET genre leaves the key absent, not \"\" - a default keyed on "
                   "an empty string is a default keyed on a guess",
                   r2.posts.front().second.substring (0, 160));
        }
    }

    // ---- SC: A STOPPED RUN'S LINE GOES TO ITS OWN CHAT (Sean's 11:25, 21 Oct 2026) ------------
    //
    // *"The old run was stopped (right), but its "Stopped." line landed at the top of the NEW Nafe
    // chat instead of the Aitch chat it belongs to ... A stopped run's line must go to the run's own
    // chat id (client.runChatId()), never the current one."*
    //
    // The record's gate compared the open chat against an EDITOR member that moves to whichever chat
    // a row was last born in - so when he opened the Nafe chat and typed, the new run's row moved
    // the member and the OLD run's stop line was recorded against it. The destination is now
    // client.runChatId(), and THAT is what this leg holds: the run keeps its own chat across the
    // stop, so the id the record will use is still the chat the run belongs to.
    {
        std::printf ("== SC: the run's own chat survives its stop ==\n");
        Rig r;
        r.client.start ("add a delay", "chatAitch");
        check (r.client.runChatId() == "chatAitch",
               "SC1 the run knows its chat while it runs", r.client.runChatId());
        r.client.stopWithReason ("you moved on to Nafe Main_01");
        check (r.client.runChatId() == "chatAitch",
               "SC2 RED-FIRST: and STILL knows it after being stopped - the stop line is recorded "
               "after stop(), so a run that forgot its chat here would write into whatever is on "
               "screen (his 11:25)", r.client.runChatId());
        check (r.client.state() == EJAgentClient::State::Stopped,
               "SC3 ...and it really is stopped");
        // ...and the NEXT run re-stamps it, so the old chat is not inherited by the new run either.
        r.client.start ("add the bx opto Pedal", "chatNafe");
        check (r.client.runChatId() == "chatNafe",
               "SC4 a new run takes the chat it was typed in - the old one is not inherited",
               r.client.runChatId());
        check (! echojay::agentui::rowBelongsInChat ("chatAitch", "chatNafe"),
               "SC5 ...and the two chats are still told apart, which is what keeps the old card "
               "where it belongs");
    }

    // ---- TH: "THINKING..." SITS UNDER THE LATEST STEP (his 14:08 ruling, 21 Oct 2026) ---------
    //
    // *"While an agent run is working, a "Thinking..." line always sits under the latest step, from
    // the moment that step ticks until the next step appears, every time, not only at the start.
    // Today "Thinking..." shows before the first step, then disappears once a step completes, so the
    // card looks idle while it waits on the server ... The line goes when the run is waiting on Sean
    // (an ask with buttons) or has finished."*
    //
    // The decision is echojay::agentui::lastRowHasSettled plus the two clauses around it, and the
    // legs hold every clause he wrote - including the two where the line must NOT appear, which is
    // where a careless version of this would put two moving things on one card.
    {
        std::printf ("== TH: the Thinking line, under the latest step, every time ==\n");
        using echojay::agentui::lastRowHasSettled;
        using echojay::agentui::runViewFrom;
        const juce::String kThinking = juce::String::fromUTF8 ("Thinking\xe2\x80\xa6");
        const auto thinkingRows = [&kThinking] (const echojay::agentui::RunView& v)
        {
            int n = 0;
            for (const auto& s : v.steps) if (s.id == "thinking") ++n;
            juce::ignoreUnused (kThinking);
            return n;
        };

        Rig r;
        r.client.start ("set the kick's compressor", "chatTH");
        {   // BEFORE THE FIRST STEP: today's behaviour, and it stays
            const auto v = runViewFrom (r.client, {}, true);
            check (thinkingRows (v) == 1 && v.steps.size() == 1
                     && v.steps.back().label == kThinking,
                   "TH1 before the first step the card has the Thinking line (this part already "
                   "worked)", juce::String ((int) v.steps.size()) + " row(s)");
            check (v.stepCount == 0,
                   "TH2 ...and it is NOT counted as a step - the control must not say \"1 step\" for "
                   "work that has not reported", juce::String (v.stepCount));
        }
        r.round (1);
        r.tool (R"({"id":"tc_th1","name":"look","args":{"what":"rack"},"approval":"free"})");
        r.await ({ "tc_th1" });
        pumpMs (20);
        {   // THE CASE HE REPORTED: a step has ticked and the run is waiting on the server
            const auto v = runViewFrom (r.client, {}, true);
            check (v.steps.size() >= 2,
                   "TH3 RED-FIRST: with one step DONE and the run still working there are TWO rows - "
                   "the step and the Thinking line under it. This is his card: \"read the rack\" "
                   "ticked, then nothing but \"Working\" top right",
                   juce::String ((int) v.steps.size()) + " row(s)");
            check (! v.steps.empty() && v.steps.back().id == "thinking"
                     && v.steps.back().label == kThinking,
                   "TH4 ...and it is the LAST row, under the step, not above it",
                   v.steps.empty() ? juce::String ("(none)") : v.steps.back().id);
            check (v.steps.back().status == (int) EJAgentClient::Step::Status::Running,
                   "TH5 ...carrying the Running glyph, so it pulses like work in progress",
                   juce::String (v.steps.back().status));
            check (v.stepCount == 1,
                   "TH6 ...and the real step count is still 1, not 2 (the phantom is counted out)",
                   juce::String (v.stepCount));
            check (v.running, "TH7 ...and \"Working\" top right stays - both, as he asked");
        }
        {   // AND IT GOES WHEN THE RUN IS OVER
            EJAgentClientTestAccess::feed (r.client, "done", R"({"summary":"Done."})");
            pumpMs (20);
            const auto v = runViewFrom (r.client, {}, true);
            check (thinkingRows (v) == 0,
                   "TH8 a finished run has NO Thinking line - his \"or has finished\"",
                   juce::String ((int) v.steps.size()) + " row(s)");
            check (! v.running, "TH9 ...and nothing claims to be working");
        }
    }
    {
        // AND IT GOES WHILE THE RUN WAITS ON HIM. An ask with buttons is his turn, not the server's,
        // and a pulsing line under the question would say the opposite.
        // THE REAL SHAPE OF AN ASK, which my first cut got wrong: it is a `talk` tool carrying an
        // `ask` object - the frame his own log shows ("step tc_check2 RUN talk \"How's it
        // sounding?\"") - not a tool called "ask". With the wrong frame there was no pending ask at
        // all, the run was simply working, and the leg was asserting the opposite of its own name.
        Rig r2;
        r2.client.start ("pick a compressor", "chatTH2");
        r2.round (1);
        r2.tool (R"({"id":"tc_th2","name":"talk","args":{"text":"Added it.","ask":{"question":"How's it sounding?","choices":[{"label":"Sounds good"},{"label":"Run a check"},{"label":"Use a different one"}],"allowFreeText":true}},"approval":"free"})");
        r2.await ({ "tc_th2" });     // the await is what RUNS the talk tool and raises the ask
        pumpMs (20);
        check (r2.client.askPending(),
               "TH10a the fixture really is waiting on him (an ask with buttons)",
               juce::String ((int) r2.client.pendingAsk().labels.size()) + " label(s)");
        const auto v = echojay::agentui::runViewFrom (r2.client, {}, true);
        int think = 0; for (const auto& s : v.steps) if (s.id == "thinking") ++think;
        check (think == 0,
               "TH10 a run waiting on HIM has no Thinking line - the ask's own buttons are the thing "
               "he is looking at (his \"the line goes when the run is waiting on Sean\")",
               juce::String ((int) v.steps.size()) + " row(s), chips on last: "
                 + (v.steps.empty() ? juce::String ("-") : juce::String (v.steps.back().chips.size())));
    }
    {
        // AND THE PURE DECISION, both directions, on the rows themselves - the test that keeps a
        // second moving line off a card where something is already moving.
        using echojay::agentui::lastRowHasSettled;
        using St = EJAgentClient::Step::Status;
        std::vector<echojay::agentui::RunStep> rows;
        check (lastRowHasSettled (rows), "TH11 no rows yet: the line stands in for the first step");
        rows.push_back ({}); rows.back().status = (int) St::Running;
        check (! lastRowHasSettled (rows),
               "TH12 a step still RUNNING is its own indicator - no second pulsing line under it");
        rows.back().status = (int) St::AwaitingApproval;
        check (! lastRowHasSettled (rows),
               "TH13 ...nor under a step waiting for an answer");
        rows.back().status = (int) St::Pending;
        check (! lastRowHasSettled (rows), "TH14 ...nor under one that has not started");
        for (auto st : { St::Done, St::Failed, St::Declined, St::NotRun, St::Undone, St::Stopped })
        {
            rows.back().status = (int) st;
            check (lastRowHasSettled (rows),
                   juce::String ("TH15 a settled last row takes the line: status ")
                     + juce::String ((int) st));
        }
    }

    // ---- LV: A REOPENED WAITING CARD KEEPS ITS CHIPS (his 14:15 item 2, 21 Oct 2026) ----------
    //
    // *"He left the CL 1B add waiting on "How's it sounding?", closed and reopened the V2 window ...
    // the waiting card came back with no Sounds good / Run a check / Use a different one."*
    //
    // HIS LOG, BY ITS ORDER:
    //   14:15:18  EJAgentRow: this window ADOPTED the run's row ... chips=3 in the snapshot,
    //             clientPendingAsk=3 label(s)
    //   14:15:19  EJStores: merged 2 row(s) ...
    // Both copies had the chips; the replay that ran AFTER the adopt reissued every restored row's
    // integer id, the editor's `live` test compared integers, and a card declared history lays out
    // no chips, no Stop and no Undo. The run's KEY survives all of that, which is why it decides.
    //
    // The sequence itself (run waiting, close, reopen) is a rig row - only a real window replays a
    // chat. What belongs here is the decision those three steps turn on, both directions.
    {
        std::printf ("== LV: which row is the live run ==\n");
        using echojay::agentui::rowIsLiveRun;

        check (rowIsLiveRun ("key-A", "key-A", 7, 7, true),
               "LV1 the row this window wrote: key and id both agree");
        check (rowIsLiveRun ("key-A", "key-A", 9, 7, true),
               "LV2 RED-FIRST: after a replay REISSUED the id, the KEY still says live - this is his "
               "14:15 card, and the integer test is what took its chips away");
        check (! rowIsLiveRun ("key-B", "key-A", 9, 7, true),
               "LV3 another run's row is NOT live, however the integers fall - his 11:59, survival "
               "is not capture");
        check (rowIsLiveRun ("", "key-A", 7, 7, true),
               "LV4 a row with no key yet (just created, nothing written a snapshot) is live on its "
               "id - the fast path has to keep working or a brand-new card loses its own chips");
        check (! rowIsLiveRun ("", "key-A", 0, 0, true),
               "LV5 ...but id 0 is \"no id\", not a match: two unidentified rows are not each other");
        check (! rowIsLiveRun ("key-A", "key-A", 7, 7, false),
               "LV6 no session, no live row - a finished card is history whatever it carries");
        check (rowIsLiveRun ("  key-A  ", "key-A", 0, 0, true),
               "LV7 whitespace is not an identity (the same rule rowBelongsInChat keeps)");
        check (! rowIsLiveRun ("", "", 0, 0, true),
               "LV8 nothing known either side: not live, so a card with no identity is never handed "
               "another run's controls");
    }

    // ---- SF: B'S STATUS FRAMES (his 15:28 request, 21 Oct 2026) ------------------------------
    //
    // *"under the "Thinking..." line, show the latest status text B sends during the wait ...
    // replacing the previous one as each arrives ... It disappears when the next step appears or an
    // ask is raised. With no status yet, just "Thinking...". "*
    //
    // Agreed with B as docs/CONTRACT_status_frames_2026-10-21.md on both sides: SSE event `status`,
    // {type, text, round?}, sent only when the phase changes. Every clause of his sentence is a leg,
    // and the three that clear it matter most - a status that outlives its wait is a claim about the
    // present made from the past.
    {
        std::printf ("== SF: the status line under Thinking ==\n");
        const juce::String kThinking = juce::String::fromUTF8 ("Thinking\xe2\x80\xa6");
        Rig r;
        r.client.start ("build a mix bus", "chatSF");
        {
            const auto v = echojay::agentui::runViewFrom (r.client, {}, true);
            check (! v.steps.empty() && v.steps.back().label == kThinking
                     && v.steps.back().detail.isEmpty(),
                   "SF1 with no status yet the row is just \"Thinking...\" - nothing is invented",
                   v.steps.empty() ? juce::String ("(no rows)") : v.steps.back().detail);
        }
        EJAgentClientTestAccess::feed (r.client, "status", R"({"type":"status","text":"Reading the rack","round":1})");
        pumpMs (10);
        check (r.client.serverStatusLine() == "Reading the rack",
               "SF2 B's words are held as sent - not rewritten, not capitalised, not punctuated",
               r.client.serverStatusLine());
        {
            const auto v = echojay::agentui::runViewFrom (r.client, {}, true);
            check (! v.steps.empty() && v.steps.back().label == kThinking
                     && v.steps.back().detail == "Reading the rack",
                   "SF3 RED-FIRST: it is drawn UNDER the Thinking line, on the same row",
                   v.steps.empty() ? juce::String ("(no rows)") : v.steps.back().detail);
            check (v.statusText == "Reading the rack",
                   "SF4 ...and it rides the ENTRY's payload, which is where he asked it be drawn from",
                   v.statusText);
        }
        // THE LATEST REPLACES THE PREVIOUS: no list, no history.
        EJAgentClientTestAccess::feed (r.client, "status", R"({"type":"status","text":"Choosing the compressor","round":1})");
        pumpMs (10);
        check (r.client.serverStatusLine() == "Choosing the compressor",
               "SF5 each frame REPLACES the last - one line, never a growing list",
               r.client.serverStatusLine());
        // AN EMPTY FRAME IS IGNORED, and a stale round is dropped: both are B's own rules.
        EJAgentClientTestAccess::feed (r.client, "status", R"({"type":"status","text":"   "})");
        pumpMs (10);
        check (r.client.serverStatusLine() == "Choosing the compressor",
               "SF6 a frame with no text clears nothing and shows nothing", r.client.serverStatusLine());
        r.round (4);
        EJAgentClientTestAccess::feed (r.client, "status", R"({"type":"status","text":"a late frame","round":2})");
        pumpMs (10);
        check (r.client.serverStatusLine() != "a late frame",
               "SF7 a frame stamped with an older round is DROPPED - that is what B sends `round` for",
               r.client.serverStatusLine());
        // AND IT GOES WHEN THE WAIT ENDS: a step, an ask, the end of the run.
        EJAgentClientTestAccess::feed (r.client, "status", R"({"type":"status","text":"Adding the plugins","round":4})");
        pumpMs (10);
        r.tool (R"({"id":"tc_sf","name":"look","args":{"what":"rack"},"approval":"free"})");
        pumpMs (10);
        check (r.client.serverStatusLine().isEmpty(),
               "SF8 a step beginning clears it - the wait it described is over",
               r.client.serverStatusLine());
    }
    {
        // AN ASK CLEARS IT TOO: his turn, not the server's, and the chips are what he is looking at.
        Rig r2;
        r2.client.start ("pick a compressor", "chatSF2");
        r2.round (1);
        EJAgentClientTestAccess::feed (r2.client, "status", R"({"type":"status","text":"Choosing the compressor","round":1})");
        pumpMs (10);
        r2.tool (R"({"id":"tc_sf2","name":"talk","args":{"text":"Added it.","ask":{"question":"How's it sounding?","choices":[{"label":"Sounds good"}],"allowFreeText":true}},"approval":"free"})");
        r2.await ({ "tc_sf2" });
        pumpMs (20);
        check (r2.client.askPending() && r2.client.serverStatusLine().isEmpty(),
               "SF9 an ask clears it - the run is waiting on HIM, and a pulsing server status under "
               "his buttons would say the opposite", r2.client.serverStatusLine());
        int think = 0;
        const auto v = echojay::agentui::runViewFrom (r2.client, {}, true);
        for (const auto& st : v.steps) if (st.id == "thinking") ++think;
        check (think == 0, "SF10 ...and there is no Thinking row to put it under either");
    }
    {
        // THE NARROW WINDOW (his 17:3x item 1): the right-hand word gives up its column and becomes
        // the row's own second line, so the step text always has the card's width.
        std::printf ("== SF/NW: the \"your answer?\" label in a narrow window ==\n");
        using namespace echojay::agentui;
        RunView v;
        v.running = true;
        RunStep s; s.id = "ask"; s.label = "Which compressor should I use on this?";
        s.right = "your answer?"; s.status = (int) EJAgentClient::Step::Status::Running;
        v.steps.push_back (s);
        const auto wide = layoutRun (v, 0, 0, 520);
        const auto narrow = layoutRun (v, 0, 0, 200);
        check (! wide.rows.empty() && wide.rows[0].button.getWidth() > 0,
               "NW1 at a comfortable width the word keeps its own column, as the mockup has it",
               juce::String (wide.rows.empty() ? 0 : wide.rows[0].button.getWidth()));
        check (! narrow.rows.empty() && narrow.rows[0].button.getWidth() == 0,
               "NW2 RED-FIRST: in a narrow window NO column is reserved - his step text was squeezed "
               "into a thin strip beside it",
               juce::String (narrow.rows.empty() ? -1 : narrow.rows[0].button.getWidth()));
        // MY FIRST ASSERTION WAS MEANINGLESS: it compared label widths across two different card
        // widths (176 against 412) and could only ever fail. What he photographed is the text
        // STOPPING SHORT of the card's edge, so that is what is measured: in the narrow card the
        // label runs to the edge, and in the wide one it stops to leave the column.
        check (! narrow.rows.empty() && narrow.rows[0].label.getRight() >= 200 - 2,
               "NW3 ...so in a narrow card the step text runs to the card's edge - no column is "
               "taken out of it",
               juce::String (narrow.rows.empty() ? 0 : narrow.rows[0].label.getRight()) + " of 200");
        check (! wide.rows.empty() && wide.rows[0].label.getRight() <= 520 - 76,
               "NW4 ...while a comfortable card still keeps the column clear for the word",
               juce::String (wide.rows.empty() ? 0 : wide.rows[0].label.getRight()) + " of 520");
    }

    std::printf ("\n==== agent_client_guard: %s (%d ok, %d failed) ====\n", failures == 0 ? "GREEN" : "RED", passes, failures);
    return failures == 0 ? 0 : 1;
}
