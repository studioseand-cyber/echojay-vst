#pragma once
// =============================================================================
//  EJAgentClient — the plugin side of the AGENT LOOP (AGENT_MODE_PLAN.md 1.2).
//
//  One client = one session = one goal. It:
//    - POSTs /api/agent/start, then one /api/agent/step per round, reading each
//      response as SSE (EJAgentFraming + EJAgentProtocol);
//    - turns tool_call frames into checklist STEPS and, on the round's `await`,
//      runs them IN ORDER through the ToolExecutor: free calls at once,
//      ask_first calls only after the plan card is submitted, `do`s stopping
//      at the first failure for the rest of that round (the model sees which
//      ran);
//    - runs wait_for_playback itself: the prompt line, a listening indicator,
//      "Got it, you can stop", an early stop, a transport-stopped early end
//      and a hard timeout - it NEVER hangs;
//    - posts ONE step with every awaited id answered (ok / error), and reads
//      the next round;
//    - stops AT ONCE on stop(): the socket is cancelled, no step is posted, no
//      callback fires for the stopped generation, every unfinished step is
//      marked Stopped;
//    - keeps an undo token per landed `do` and a checkpoint from the session's
//      start, so the panel can offer Undo per step and "undo all".
//
//  THREADING. The network runs on a launched worker (the chat-stream pattern:
//  one-byte reads so paint latency is the server's, cancel through a handle
//  the worker attaches its stream to, a net::Worker census entry so teardown
//  waits). EVERYTHING ELSE is message-thread: frames are dispatched with
//  callAsync behind an alive flag AND a generation check, so nothing from a
//  stopped or superseded session ever reaches the model or the UI.
//
//  The client renders nothing. EJAgentPanel reads it and listens.
// =============================================================================
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "EJAgentProtocol.h"
#include "EJAgentTools.h"

class EJAgentClient : private juce::Timer
{
public:
    // What the transport needs from EchoJayAPI, read at START time (the token
    // can change on login). Hook T1 in the handoff: EchoJayAPI::agentTransport().
    struct Transport
    {
        juce::String baseUrl;        // e.g. api.getEndpoint() - no trailing slash needed
        juce::String authToken;      // bearer; "" = not signed in -> start() fails with sign_in
        juce::String extraHeaders;   // dev transport headers, "" in release
        juce::String appVersion;
    };
    using TransportProvider = std::function<Transport()>;

    enum class State { Idle, Connecting, Streaming, Executing, AwaitingApproval, AwaitingAsk, Listening, Posting, Done, Stopped, Failed };
    static const char* stateName (State s) noexcept;

    struct Step
    {
        enum class Status { Pending, AwaitingApproval, Running, Done, Failed, Declined, NotRun, Undone, Stopped };
        juce::String id;
        echojay::agent::ToolCall call;
        juce::String label;        // describeCall(call) - the server's summary when it sent one
        juce::String detail;       // the landed line / the error message / "undone"
        juce::String undoToken;    // a do that landed
        juce::var    result;       // the compact result that went (or will go) to the server
        Status status = Status::Pending;
        bool approved = true;      // the plan card's per-line decision (ask_first only)
        int  round = 0;
        bool isDo() const noexcept       { return call.name == "do"; }
        bool askFirst() const noexcept   { return call.askFirst(); }
        bool isFinal() const noexcept    { return status != Status::Pending && status != Status::AwaitingApproval && status != Status::Running; }
        bool answered() const noexcept   { return isFinal() && status != Status::Stopped; }
        bool canUndo() const noexcept    { return status == Status::Done && undoToken.isNotEmpty(); }
    };

    struct Playback
    {
        bool   active = false;
        double minSeconds = 8.0;
        double heardSeconds = 0.0;
        bool   reached = false;          // min reached -> "Got it, you can stop"
        juce::int64 startedMs = 0;
        int    timeoutSeconds = 60;      // plan 1.4 suggests 60
        juce::String line;               // the one sentence the panel shows
    };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void agentChanged() {}                                                 // any model change: relayout + repaint
        virtual void agentTextDelta (const juce::String& text) { juce::ignoreUnused (text); }   // streamed talk (provisional)
        // Lines for the CHAT TRANSCRIPT (hook E4): the goal as the user's turn, each round's talk once it is
        // complete, the done summary, a stop or an error. role = "user" | "assistant".
        virtual void agentTranscript (const juce::String& role, const juce::String& text) { juce::ignoreUnused (role, text); }
    };

    EJAgentClient (TransportProvider transport, echojay::agent::ToolExecutor& executor,
                   std::function<void (const juce::String&)> log);
    ~EJAgentClient() override;

    void addListener (Listener* l);
    void removeListener (Listener* l);

    // ---- control -------------------------------------------------------------
    void start (const juce::String& goal, const juce::String& chatId = {});
    void stop();                 // halts at once; the panel stays up in Stopped so Undo all is reachable
    void retry();                // after a retryable error: re-posts the last request body
    void dismiss();              // Done/Stopped/Failed -> Idle (the panel hides)
    bool isActive() const noexcept;      // a live session (not Idle/Done/Stopped/Failed)
    bool hasSession() const noexcept;    // anything to show, finished or not
    // A typed message while a session is live: answers a pending ask (true = consumed) or is refused (true, with a
    // status line) so the send path does not start a second conversation under a running agent.
    bool interceptTyped (const juce::String& typed);

    // ---- the plan card -------------------------------------------------------
    std::vector<const Step*> planLines() const;             // this round's ask_first steps still awaiting a decision
    void setLineApproved (const juce::String& id, bool approved);
    void approveAll();
    void declineAll();          // every line declined, then submitted
    void submitPlan();          // runs the approved lines in order, declines the rest

    // ---- the ask card --------------------------------------------------------
    bool askPending() const noexcept { return state_ == State::AwaitingAsk; }
    echojay::agent::TalkAsk pendingAsk() const { return pendingAsk_; }
    void answerAsk (const juce::String& choice, bool typed = false);

    // ---- playback ------------------------------------------------------------
    const Playback& playback() const noexcept { return playback_; }
    void playbackGotIt();       // the user's early stop

    // ---- undo ----------------------------------------------------------------
    bool undoAllowedNow() const noexcept;    // not while a round is executing or a request is in flight
    bool canUndoAll() const noexcept;
    void undoStep (const juce::String& id);
    void undoAll();

    // ---- model (read by the panel) -------------------------------------------
    State state() const noexcept { return state_; }
    const std::vector<Step>& steps() const noexcept { return steps_; }
    juce::String goal() const { return goal_; }
    juce::String sessionId() const { return sessionId_; }
    int  round() const noexcept { return round_; }
    juce::String liveText() const { return liveText_; }        // this round's streamed talk so far
    juce::String statusLine() const;                           // one line for the header
    juce::String errorMessage() const { return errorMessage_; }
    bool errorRetryable() const noexcept { return errorRetryable_; }
    juce::String doneSummary() const { return doneSummary_; }
    double costUsd() const noexcept { return costUsd_; }
    // One line the panel shows above the checklist until cleared: a refused send while active, an undo-all outcome.
    juce::String notice() const { return refusedTypedLine_; }
    void clearNotice() { if (refusedTypedLine_.isNotEmpty()) { refusedTypedLine_.clear(); changed(); } }

    // Tunables (the guard shortens them)
    int stallTimeoutMs   = 90'000;   // no bytes on an open stream for this long -> stream_stalled
    int connectTimeoutMs = 60'000;
    int playbackTimeoutS = 60;
    int playbackTickMs   = 100;

    // ---- the pure decision the guard pins: which steps run, in what order, after a failure ----
    // Given the round's steps, returns the index of the next step to run, or -1 when none can run now:
    //   -2 = a plan decision is needed first (an ask_first step without a decision, and the plan is not submitted)
    // A do that follows a FAILED do in the same round is marked NotRun by the caller (see advance()).
    static int nextRunnable (const std::vector<Step>& steps, int round, bool planSubmitted);

private:
    // tools/agent_client_guard ONLY: the guard replaces the network with a recorder and feeds frames itself. One
    // named hook, never "#define private public" (the EchoJayAPIRequestPin idiom).
    friend struct EJAgentClientTestAccess;
    std::function<void (const juce::String& path, const juce::String& body)> postOverride_;

    // ---- transport ------------------------------------------------------------
    struct Cancel
    {
        void cancel();
        void attach (juce::WebInputStream* s);
        void detach();
        bool isCancelled() const noexcept { return cancelled.load(); }
        std::atomic<bool> cancelled { false };
        std::mutex lock;
        juce::WebInputStream* active = nullptr;
    };
    void post (const juce::String& path, const juce::String& body);
    void onFrame (int generation, echojay::agent::Frame f);
    void onStreamEnded (int generation, bool sawTerminal, int statusCode, const juce::String& error);
    void onBytes (int generation);

    // ---- the round -------------------------------------------------------------
    void beginExecuting();
    void advance();                               // the pump: runs the next runnable step or posts the step
    void runStep (Step& s);
    void completeStep (int generation, const juce::String& id, echojay::agent::ToolOutcome o);
    void postStepIfComplete();
    void flushRoundTalk();                        // this round's talk -> transcript, once
    void finishSession (State s, const juce::String& line);
    void markUnfinishedStopped();
    Step* stepById (const juce::String& id);
    const Step* stepById (const juce::String& id) const;
    void setState (State s);
    void changed();
    void log (const juce::String& line) const;

    // ---- playback --------------------------------------------------------------
    void beginPlayback (Step& s);
    void tickPlayback();
    void endPlayback (const juce::String& reason);
    void timerCallback() override;

    TransportProvider transport_;
    echojay::agent::ToolExecutor& executor_;
    std::function<void (const juce::String&)> log_;
    juce::ListenerList<Listener> listeners_;

    State state_ = State::Idle;
    int   generation_ = 0;                        // bumped on start() and stop(); stale completions are ignored
    std::shared_ptr<std::atomic<bool>> alive_ { std::make_shared<std::atomic<bool>> (true) };
    std::shared_ptr<Cancel> cancel_;

    juce::String goal_, chatId_, sessionId_, checkpoint_;
    int round_ = 0;
    std::vector<Step> steps_;
    juce::StringArray awaitIds_;
    bool awaitSeen_ = false;
    bool planSubmitted_ = false;
    bool roundDoFailed_ = false;
    juce::String liveText_, roundTalk_;
    bool roundTalkFlushed_ = false;
    echojay::agent::TalkAsk pendingAsk_;
    juce::String pendingAskId_;
    Playback playback_;
    juce::String playbackStepId_;
    juce::String lastPath_, lastBody_;
    juce::String errorMessage_, doneSummary_;
    bool errorRetryable_ = false;
    double costUsd_ = 0.0;
    juce::int64 lastBytesMs_ = 0;
    juce::String refusedTypedLine_;
    int undoInFlight_ = 0;
};
