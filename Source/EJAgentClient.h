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
#include "EJDialFrame.h"       // A1a: what a dial frame decides, and when it answers

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

    /** ---- IS THIS RUN ACTUALLY WORKING? (Sean's ruling, 20 Oct 2026) ------------------------
        *"A run that is only waiting for Sean's answer doesn't block. A message typed in a different
        chat stops the waiting run ... If a run is actually executing, the notice goes in the chat he
        typed in and his text stays in the box."*

        His vanished message: the notice *"The agent is still working on build a mix bus. Stop it
        first, or wait for it to finish."* appeared in the MIX BUS chat, not the Link chat he typed
        in, and his text was thrown away - for a run that was only sitting on its loudness ask
        (Commercial / Pushed / Keep it dynamic). It was not working. It was waiting.

        TRUE only for the states where the agent is genuinely mid-flight and a second conversation
        would race the rack it is editing. Everything else is a question in another shape, and the
        answer to a question nobody wants to answer is a new instruction.

        A free function so a guard can walk every state without a session, a socket or a window - the
        decision has been wrong in three different shapes this week and it is one line of truth. */
    static bool stateBlocksANewMessage (State s) noexcept
    {
        switch (s)
        {
            case State::Connecting:
            case State::Streaming:
            case State::Executing:
            case State::Posting:
                return true;                        // mid-flight: two conversations would race the rack
            case State::AwaitingApproval:           // a plan waiting for Go
            case State::AwaitingAsk:                // a question waiting for an answer
            case State::Listening:                  // waiting for playback
            case State::Idle: case State::Done:
            case State::Stopped: case State::Failed:
                return false;                       // waiting on HIM, or over: it does not block
        }
        return false;                               // an unknown state never blocks him
    }
    bool isWorking() const noexcept { return stateBlocksANewMessage (state_); }

    struct Step
    {
        enum class Status { Pending, AwaitingApproval, Running, Done, Failed, Declined, NotRun, Undone, Stopped };
        juce::String id;
        echojay::agent::ToolCall call;
        juce::String label;        // describeCall(call) - the server's summary when it sent one
        juce::String detail;       // the landed line / the USER's error line / "undone"  (item 3: never the model's)
        juce::String modelDetail;  // item 3: the full text that went to the model, kept for the collapsed view
        juce::String undoToken;    // a do that landed
        juce::var    result;       // the compact result that went (or will go) to the server
        Status status = Status::Pending;
        bool approved = true;      // the plan card's per-line decision (ask_first only)
        // AUTO MODE: this step was ask_first and Auto ran it anyway (the line reads "auto-applied"), or it
        // stayed ask_first in Auto and askReason says WHY - never the plugin inventing a reason.
        bool autoApplied = false;
        juce::String askReason;
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

    /** ---- A DIAL FRAME IN FLIGHT (A1a, 28 Oct 2026) ------------------------------------
        The same shape the playback wait has, for the same reason: a step that cannot be answered
        when it is run. B's contract is explicit about what holds it open and what ends it -
        *"show step_text as the current step ... if the track isn't playing, show \"Play the track
        to set the UnFairchild\" ... wait up to 60 s for playback to start ... if it doesn't start,
        answer not_heard and keep the dial armed for the next playback"*. */
    struct DialStep
    {
        bool   active = false;
        juce::String stepId;        ///< the frame's own tool-call id
        juce::String plugin;        ///< the name B sent, which the step's line says
        int    slot1 = 0;           ///< 1-based, resolved before anything was armed
        juce::int64 startedMs = 0;
        bool   heardAnything = false;   ///< sticky: once audio has been heard, the 60 s bound is spent
        juce::String line;              ///< the one sentence the step shows
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
    /** Stop, with a REASON the run's own card shows (his ruling: *"its own card says 'Stopped: you
        moved on to <channel>'"*). The default is the bare stop every other caller wants. */
    void stopWithReason (const juce::String& reason);
    void stop();                 // halts at once; the panel stays up in Stopped so Undo all is reachable
    void retry();                // after a retryable error: re-posts the last request body
    void dismiss();              // Done/Stopped/Failed -> Idle (the panel hides)
    bool isActive() const noexcept;      // a live session (not Idle/Done/Stopped/Failed)
    bool hasSession() const noexcept;    // anything to show, finished or not
    /** Which run this is: 0 before the first, then +1 per start(). A retry does NOT move it (the same
        run), and neither does a stop. Read by EJAgentHome to tell a NEW run from the one it is holding
        before the server has given it a sessionId - see runsStarted_. */
    int  runSerial() const noexcept { return runsStarted_; }
    // A typed message while a session is live: answers a pending ask (true = consumed) or is refused (true, with a
    // status line) so the send path does not start a second conversation under a running agent.
    bool interceptTyped (const juce::String& typed);
    /** ---- THE TAP IS THE APPROVAL (16 Oct 2026, Sean in Logic, step 7) -------------------------
        The label a user TAPPED on an ask, kept until the step it chose has run.

        His step 7, on the Kick's Link: he tapped "Transient Master" from the chips and was then
        asked to press Apply on "add Transient Master - on 88_7even Kick_2_01". Both gates are right
        on their own - the server marks a cross-track add ask_first, and Ask mode waits on an
        ask_first step - but between them the user answered the same question twice. His ruling: "The
        tap IS the approval, including on another track."

        So the tap is remembered, and the step that carries out THAT pick runs without a second press.
        Cleared the moment it is used, and matched by the plugin NAME the call carries, so it can
        only ever cover the thing the tap chose. */
    juce::String tappedPick_;
    /** An answer is a name; an instruction is a sentence. Public and static so a guard can hold it
        in both directions - "MTransient" must never read as an instruction, "Set the pre-gain to
        2 dB" must never read as an answer. See the note on the definition. */
    static bool typedLooksLikeNewInstruction (const juce::String& typed);
    juce::String typedNamesAnInstalledPlugin (const juce::String& typed) const;

    // ---- the plan card -------------------------------------------------------
    std::vector<const Step*> planLines() const;             // this round's ask_first steps still awaiting a decision
    void setLineApproved (const juce::String& id, bool approved);
    void approveAll();
    void declineAll();          // every line declined, then submitted
    void submitPlan();          // runs the approved lines in order, declines the rest

    // ---- the ask card --------------------------------------------------------
    bool askPending() const noexcept { return state_ == State::AwaitingAsk; }
    /** ---- B'S LATEST STATUS LINE, OR EMPTY (his 15:28 request, 21 Oct 2026) ------------------
        *"show the latest status text B sends during the wait ... replacing the previous one as each
        arrives ... It disappears when the next step appears or an ask is raised. With no status yet,
        just "Thinking..."."*

        One line, B's own words, never stored and never remembered across a wait: it is cleared when
        a step arrives, when an ask is raised, and when the run ends - so it can only ever describe
        the wait happening now. A status that outlived its wait would be a claim about the present
        made from the past, which is the whole family this week has been about.

        NAMED FOR WHOSE LINE IT IS. statusLine() above is this CLIENT's own state word ("Thinking
        (round 3)...") and has been since the first round; this is B's. Two different sentences about
        two different things, so they do not share a name. */
    juce::String serverStatusLine() const { return statusLine_; }
    /** ---- A PLAN THIS RUN COULD NOT READ (his 21:48 fault, 22 Oct 2026) --------------------
        True once a build op arrived whose chain names no plugin in one or more slots. The card
        offers RETRY on it: the run has not failed as a whole - the server is answering - but the
        one thing it offered cannot be acted on, and "try again" is the only honest road out.
        Cleared when a new run starts, like every other per-run fact. */
    bool planUnreadable() const noexcept { return planUnreadable_; }
    /** B's keep-playing contract, point 2 (22 Oct 2026): the plugin a dial is still setting on this
        playback, or empty. Forwarded from the executor rather than cached, because the card asks it
        on every repaint and a cached answer would draw a step for a dial that had already landed -
        and point 1's prompt asks the SAME function, so the step and the prompt cannot disagree. */
    juce::String keepPlayingPlugin() const { return executor_.keepPlayingPlugin(); }
    /** What this run's dials have reported, by plugin name - the card's dial step reads it, so the
        result shows ON the step and never as a line below the card (his 08:43 ruling). */
    juce::StringPairArray dialResults() const { return executor_.dialResults(); }
    echojay::agent::TalkAsk pendingAsk() const { return pendingAsk_; }
    /** ---- THE CHAT THIS RUN BELONGS TO, FROM THE RUN ITSELF (Sean's 18:18, 20 Oct 2026) -----
        start() is given the chat id and has kept it in chatId_ all along; nothing could read it.
        The editor was keying "does this run belong here?" on a member of its OWN that is set when a
        row is born - and a row does not survive a chat switch, so after one the editor had no
        answer and fell back to the last run's chat. The run has always known. */
    juce::String runChatId() const { return chatId_; }
    void answerAsk (const juce::String& choice, bool typed = false);
    /** Which offered choice a typed answer means, or "" for none. Public because the
        rig asserts on it directly and because the composer's hint reads it. */
    juce::String matchTypedToChoice (const juce::String& typed) const;

    // ---- playback ------------------------------------------------------------
    const Playback& playback() const noexcept { return playback_; }
    void playbackGotIt();       // the user's early stop

    // ---- undo ----------------------------------------------------------------
    /** The server's checkpoint for this run, or "" before it sends one. Public because "undo
        everything this run did" is a walk back to a POINT, and the only thing that can hold the
        point on the plugin's side is whoever watched the run start (EJAgentHome). */
    juce::String checkpoint() const { return checkpoint_; }
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
    juce::String doneReason() const { return doneReason_; }          // complete | stopped | hard_cap | error
    double costUsd() const noexcept { return costUsd_; }
    juce::String planHeading() const { return planHeading_; }        // the plan frame's heading for this round
    /** Did the SERVER send a plan card this round? Not "is the heading non-empty" - a plan frame may
        carry items and no heading, and the question the card asks is whether the server has already
        laid the plan out (his 21:52 ruling is about a build arriving with NO plan). */
    bool planCardThisRound() const { return planCardThisRound_; }
    /** May the run tell him he can stop playing? See stopInviteAllowed() in the .cpp - his 08:10
        ruling: on a build goal the first listen says "Got it" only. */
    bool stopInviteAllowed() const;
    int  ofSoftCap() const noexcept { return ofSoftCap_; }
    // One line the panel shows above the checklist until cleared: a refused send while active, an undo-all outcome.
    juce::String notice() const { return refusedTypedLine_; }
    void clearNotice() { if (refusedTypedLine_.isNotEmpty()) { refusedTypedLine_.clear(); changed(); } }

    /** AUTO MODE (Sean's ruling, 14 Oct 2026): what the client WAITS for. Auto never changes whether the
        executor may act - that gate is agentModeOn(), re-read on every op - only whether an ask_first step
        stops for a press. Set from the chip before each run. */
    enum class ApprovalMode { Ask, Auto };
    void setApprovalMode (ApprovalMode m) noexcept { approvalMode_ = m; }
    ApprovalMode approvalMode() const noexcept { return approvalMode_; }
    const char* approvalModeWord() const noexcept { return approvalMode_ == ApprovalMode::Auto ? "auto" : "ask"; }

    // Tunables (the guard shortens them)
    // HIS BOUND, 18 Oct 2026: "30 s with nothing back = a plain sentence with Retry." Measured on
    // BYTES, so an answer that is still streaming never trips it - only true silence does.
    int stallTimeoutMs   = 30'000;   // no bytes on an open stream for this long -> stream_stalled
    // FIX 2 (Sean, 14 Oct): 60 s BOUNDED THE WHOLE REQUEST AND WE GAVE UP BEFORE THE SERVER DID.
    // A step that had already delivered round 10's frames died at exactly 60.000 s with "Connection failed.
    // Please check your internet connection." - nothing was wrong with the connection; the round was slow.
    // The server's budget is 300 s, so we were cutting the socket at a fifth of it and turning a slow answer
    // into a lost one. 330 s: longer than the server's own bound, so the SERVER's answer (or its timeout)
    // always arrives first and we report what it said rather than guessing.
    int connectTimeoutMs = 330'000;
    /** The shipped bound, for a guard: a client that gives up before the server does turns a slow answer
        into a lost one, so this is asserted to be at least the server's 300 s rather than left to a
        comment. (agent_round15_guard, fix 2.)
        NO ACCESS MARKERS AROUND THIS. The first cut wrapped it in public:/private:, which was a no-op on
        the way in (this section is already public) and made playbackTimeoutS and playbackTickMs PRIVATE on
        the way out - and those are the tunables the comment above promises "the guard shortens". S2 of the
        gate found it: nine errors in agent_client_guard, all of them "is a private member". */
    static constexpr int defaultConnectTimeoutMs() { return 330'000; }
    int playbackTimeoutS = echojay::agent::kPlaybackTimeoutSeconds;   // the contract's 60 s
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
    ApprovalMode approvalMode_ = ApprovalMode::Ask;
    /** B's section 86(1), 19 Oct 2026: has anything been APPLIED since the last listen, and has the
        user tapped "Run a check" since then? A playback wait the AGENT asks for is refused when the
        first is true and the second is not. The level-match loop is exempt by construction: it
        reads the chain tallies in-process and never asks for a wait. */
    bool closingAskSeen_    = false;   // the run has shown its closing ask (id "tc_check...")
    bool userAskedForCheck_ = false;   // ...and the user has tapped "Run a check" since then   // Sean's default for a new user
    /** ---- HAS THIS RUN ACTUALLY MOVED ANYTHING? (19 Oct 2026, B's section 92, Sean first) ------
        *"at 17:20 your check(playback) refusal fired in round 1 with nothing changed... refuse a
        playback check only after a move has landed in this run."*

        TRUE once a `do` step in THIS run has come back ok. The refusal is keyed on this and nothing
        else, because it is the fact the rule is about: there is no second listen to decline until
        there has been a first change.

        It replaces closingAskSeen_ as the key. That flag was a proxy - the closing OFFERS the
        "Run a check" chip, so its presence stood in for "a change has been made" - and it was wrong
        twice over. Wrong in principle, because a closing can be shown by a run that changed nothing
        (a question, a read, a refusal). And wrong in fact, because it was never reset: start() did
        not clear it, so the first run in a session to show a closing left it TRUE for every run
        after, and round 1's first playback wait was refused before anything had happened at all.
        Both are fixed here; the flag stays for the log line, which is still worth reading. */
    bool moveLandedThisRun_ = false;

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
    void postStop();                              // POST /api/agent/stop {sessionId}: fire and forget, logged
    void onFrame (int generation, echojay::agent::Frame f);
    void onStreamEnded (int generation, bool sawTerminal, int statusCode, const juce::String& error);
    void onBytes (int generation);

    // ---- the round -------------------------------------------------------------
    void beginExecuting();
    void advance();                               // the pump: runs the next runnable step or posts the step
    void runStep (Step& s);
    void completeStep (int generation, const juce::String& id, echojay::agent::ToolOutcome o);
    void postStepIfComplete();
    void flushRoundTalk();
    /** Does the card already say this prose? Empty when it does not; otherwise the reason, for the
        log. The road to the chat row's own bubble is the transcript, so this is asked before a
        round's streamed text is sent down it (his test 0, 08:10, 23 Oct). */
    juce::String cardAlreadySays (const juce::String& prose) const;                        // this round's talk -> transcript, once
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
    /** A1a: the dial frame's three moments. beginDialStep resolves the slot and arms (or answers
        skipped without arming anything); tickDialStep watches for the result; endDialStep writes
        the answer B's contract specifies. */
    void beginDialStep (Step& s);
    /** A1b: what runs once the arm has ANSWERED - the wait itself. Split out of beginDialStep
        because a Link's arm answers on its chain ack rather than inside the call, so this half
        happens later. It takes the step's ID and re-finds it: a reference into steps_ does not
        survive a step being appended while a Link thinks. */
    void beginDialWait (const juce::String& stepId, const juce::String& plugin,
                        const juce::String& stepText, int slot1);
    void tickDialStep();
    void endDialStep (echojay::dialframe::Outcome o, const juce::String& why);
    void timerCallback() override;

    TransportProvider transport_;
    echojay::agent::ToolExecutor& executor_;
    std::function<void (const juce::String&)> log_;
    juce::ListenerList<Listener> listeners_;

    State state_ = State::Idle;
    int   generation_ = 0;                        // bumped on start() and stop(); stale completions are ignored
    // ---- WHICH RUN IS THIS (18 Oct 2026, Sean in Logic) --------------------------------------------
    // Bumped ONCE per start() and never by anything else, so "is this a new run" has an answer from
    // the instant Send is pressed. generation_ is not that answer - it also moves on stop() and on
    // retry(), and a retry is the SAME run - and the sessionId is not either: it is empty until the
    // server replies, which is the window in which the row and the record are created.
    int   runsStarted_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ { std::make_shared<std::atomic<bool>> (true) };
    std::shared_ptr<Cancel> cancel_;

    juce::String goal_, chatId_, sessionId_, checkpoint_;
    /** Consumed by the next stop() and cleared there, so a reason cannot outlive its stop (his
        ruling of 20 Oct: the card says "Stopped: you moved on to <channel>"). */
    juce::String stopReason_;
    int round_ = 0;
    std::vector<Step> steps_;
    juce::StringArray awaitIds_;
    bool awaitSeen_ = false;
    int  awaitTimeoutMs_ = 0, ofSoftCap_ = 0;
    juce::String planHeading_, doneReason_;
    bool planCardThisRound_ = false;
    bool planSubmitted_ = false;
    bool roundDoFailed_ = false;
    juce::String liveText_, roundTalk_;
    bool roundTalkFlushed_ = false;
    echojay::agent::TalkAsk pendingAsk_;
    juce::String pendingAskId_;
    Playback playback_;
    juce::String playbackStepId_;
    DialStep dial_;
    juce::String lastPath_, lastBody_;
    /** The last error frame's code, so retry() can tell a failure the server never saw (a 500, a
        timeout - resend it) from one it saw and refused for its content (result_too_large - never
        resend it). 17 Oct 2026, from Sean's screenshot of Retry failing with the same tool id. */
    juce::String lastErrorCode_;
    /** The server's own error sentence, kept for Details while the drawn line stays plain. */
    juce::String rawErrorDetail_;
public:
    /** The server's error code and text, turned into one sentence a musician can act on. Static so
        the guard can hold it in both directions without a client. */
    static juce::String plainStopLine (const juce::String& code, const juce::String& serverText);
    /** The server's own words for Details, empty when there were none. */
    const juce::String& rawErrorDetail() const noexcept { return rawErrorDetail_; }
private:
    juce::String errorMessage_, doneSummary_;
    bool errorRetryable_ = false;
    double costUsd_ = 0.0;
    juce::int64 lastBytesMs_ = 0;
    juce::String statusLine_;   // B's latest status frame (his 15:28)
    bool planUnreadable_ = false;   // his 21:48: a build whose slots named no plugin
    juce::String refusedTypedLine_;
    int undoInFlight_ = 0;
};
