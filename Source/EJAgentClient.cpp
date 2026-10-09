#include "EJAgentClient.h"
#include "EJAgentFraming.h"
#include "EJNetCensus.h"   // no network worker outlives the last processor (06d item 1)
#include <algorithm>
#include <cmath>

using namespace echojay::agent;

namespace
{
    juce::int64 nowMs() { return juce::Time::currentTimeMillis(); }

    // The message a non-200 answer carries, in any of the shapes the server uses.
    juce::String errorMessageOf (int status, const juce::String& bodyText)
    {
        auto v = juce::JSON::parse (bodyText);
        if (auto* o = v.getDynamicObject())
        {
            const auto err = o->getProperty ("error");
            if (err.isString() && err.toString().isNotEmpty()) return err.toString();
            if (auto* eo = err.getDynamicObject())
            {
                const auto m = eo->getProperty ("message").toString();
                if (m.isNotEmpty()) return m;
                const auto c = eo->getProperty ("code").toString();
                if (c.isNotEmpty()) return c;
            }
            const auto m = o->getProperty ("message").toString();
            if (m.isNotEmpty()) return m;
        }
        if (status == 401 || status == 403) return "Sign in to use the agent.";
        if (status == 404) return "This server has no agent endpoint yet.";
        if (status == 429) return "Rate limited - try again in a moment.";
        return "The server answered " + juce::String (status) + ".";
    }

    void setIfFinite (juce::DynamicObject* o, const char* key, float v, int decimals = 1)
    {
        if (std::isfinite (v))
            o->setProperty (key, juce::String (v, decimals).getDoubleValue());
    }
}

// =============================================================================
const char* EJAgentClient::stateName (State s) noexcept
{
    switch (s)
    {
        case State::Idle:             return "idle";
        case State::Connecting:       return "connecting";
        case State::Streaming:        return "streaming";
        case State::Executing:        return "executing";
        case State::AwaitingApproval: return "awaiting_approval";
        case State::AwaitingAsk:      return "awaiting_ask";
        case State::Listening:        return "listening";
        case State::Posting:          return "posting";
        case State::Done:             return "done";
        case State::Stopped:          return "stopped";
        case State::Failed:           return "failed";
    }
    return "?";
}

// ---- Cancel -------------------------------------------------------------------
void EJAgentClient::Cancel::cancel()
{
    cancelled.store (true);
    const std::scoped_lock l { lock };
    if (active != nullptr) active->cancel();
}
void EJAgentClient::Cancel::attach (juce::WebInputStream* s)
{
    const std::scoped_lock l { lock };
    active = s;
    if (cancelled.load() && s != nullptr) s->cancel();   // a cancel that landed before attach still works
}
void EJAgentClient::Cancel::detach()
{
    const std::scoped_lock l { lock };
    active = nullptr;
}

// =============================================================================
EJAgentClient::EJAgentClient (TransportProvider transport, ToolExecutor& executor,
                              std::function<void (const juce::String&)> logFn)
    : transport_ (std::move (transport)), executor_ (executor), log_ (std::move (logFn))
{
}

EJAgentClient::~EJAgentClient()
{
    alive_->store (false);       // before members go: every dispatched lambda checks it first
    if (cancel_) cancel_->cancel();
    stopTimer();
}

void EJAgentClient::addListener (Listener* l)    { listeners_.add (l); }
void EJAgentClient::removeListener (Listener* l) { listeners_.remove (l); }

void EJAgentClient::log (const juce::String& line) const
{
    if (log_) log_ ("EJAgent: " + line);
}

void EJAgentClient::changed()
{
    listeners_.call ([] (Listener& l) { l.agentChanged(); });
}

void EJAgentClient::setState (State s)
{
    if (state_ == s) return;
    log ("state " + juce::String (stateName (state_)) + " -> " + stateName (s)
         + (sessionId_.isNotEmpty() ? " session=" + sessionId_ + " round=" + juce::String (round_) : juce::String()));
    state_ = s;
}

bool EJAgentClient::isActive() const noexcept
{
    return state_ != State::Idle && state_ != State::Done && state_ != State::Stopped && state_ != State::Failed;
}
bool EJAgentClient::hasSession() const noexcept { return state_ != State::Idle; }

EJAgentClient::Step* EJAgentClient::stepById (const juce::String& id)
{
    for (auto& s : steps_) if (s.id == id) return &s;
    return nullptr;
}
const EJAgentClient::Step* EJAgentClient::stepById (const juce::String& id) const
{
    for (auto& s : steps_) if (s.id == id) return &s;
    return nullptr;
}

juce::String EJAgentClient::statusLine() const
{
    switch (state_)
    {
        case State::Idle:             return {};
        case State::Connecting:       return round_ > 0 ? "Sending results..." : "Starting...";
        case State::Streaming:        return "Thinking" + (round_ > 0 ? " (round " + juce::String (round_) + ")" : juce::String()) + "...";
        case State::Executing:        return "Working...";
        case State::AwaitingApproval: return "Waiting for your OK";
        case State::AwaitingAsk:      return "Waiting for your answer";
        case State::Listening:        return playback_.line;
        case State::Posting:          return "Sending results...";
        case State::Done:             return "Done" + (round_ > 0 ? " in " + juce::String (round_) + (round_ == 1 ? " round" : " rounds") : juce::String());
        case State::Stopped:          return "Stopped";
        case State::Failed:           return errorMessage_.isNotEmpty() ? "Failed: " + errorMessage_ : juce::String ("Failed");
    }
    return {};
}

// =============================================================================
//  control
// =============================================================================
void EJAgentClient::start (const juce::String& goalIn, const juce::String& chatId)
{
    const auto goal = goalIn.trim();
    if (goal.isEmpty()) return;
    if (isActive()) stop();

    ++generation_;
    if (cancel_) cancel_->cancel();
    cancel_.reset();
    steps_.clear();
    awaitIds_.clear();
    awaitSeen_ = false; planSubmitted_ = false; roundDoFailed_ = false;
    liveText_.clear(); roundTalk_.clear(); roundTalkFlushed_ = false;
    pendingAsk_ = {}; pendingAskId_.clear();
    playback_ = {}; playbackStepId_.clear();
    errorMessage_.clear(); doneSummary_.clear(); errorRetryable_ = false; costUsd_ = 0.0;
    refusedTypedLine_.clear();
    sessionId_.clear(); round_ = 0; ofSoftCap_ = 0; awaitTimeoutMs_ = 0;
    planHeading_.clear(); doneReason_.clear();
    goal_ = goal; chatId_ = chatId;

    checkpoint_ = executor_.captureCheckpoint ("agent: " + goal);
    log ("START goal=\"" + goal + "\" checkpoint=" + (checkpoint_.isNotEmpty() ? checkpoint_ : juce::String ("(none)")));
    listeners_.call ([&] (Listener& l) { l.agentTranscript ("user", goal); });

    const auto t = transport_();
    if (t.authToken.isEmpty())
    {
        errorMessage_ = "Sign in to use the agent.";
        errorRetryable_ = false;
        finishSession (State::Failed, errorMessage_);
        return;
    }
    const auto body = buildStartBody (goal, executor_.startContext(), t.appVersion, chatId);
    post ("/api/agent/start", body);
    startTimer (1000);
}

void EJAgentClient::stop()
{
    if (! isActive()) return;
    log ("STOP requested in state " + juce::String (stateName (state_)) + " round=" + juce::String (round_)
         + " unfinished=" + juce::String ((int) std::count_if (steps_.begin(), steps_.end(), [] (const Step& s) { return ! s.isFinal(); })));
    ++generation_;                               // every in-flight completion and frame is now stale
    if (cancel_) cancel_->cancel();              // unblocks a read on a quiet socket; no STEP is posted after this
    markUnfinishedStopped();                     // every in-flight call reads stopped (contract 1: Stop)
    postStop();                                  // POST /api/agent/stop {sessionId}; the server ends the session itself
    playback_.active = false; playbackStepId_.clear();
    pendingAsk_ = {}; pendingAskId_.clear();
    flushRoundTalk();
    stopTimer();
    setState (State::Stopped);
    listeners_.call ([] (Listener& l) { l.agentTranscript ("assistant", "Stopped."); });
    changed();
}

void EJAgentClient::markUnfinishedStopped()
{
    for (auto& s : steps_)
        if (! s.isFinal())
        {
            s.status = Step::Status::Stopped;
            s.detail = "stopped";
        }
}

void EJAgentClient::retry()
{
    if (state_ != State::Failed || ! errorRetryable_ || lastBody_.isEmpty() || lastPath_.isEmpty()) return;
    log ("RETRY " + lastPath_);
    ++generation_;
    errorMessage_.clear(); errorRetryable_ = false;
    post (lastPath_, lastBody_);
    startTimer (1000);
}

void EJAgentClient::dismiss()
{
    if (isActive()) return;
    log ("dismiss");
    setState (State::Idle);
    steps_.clear();
    goal_.clear(); sessionId_.clear(); round_ = 0;
    liveText_.clear(); roundTalk_.clear();
    errorMessage_.clear(); doneSummary_.clear();
    refusedTypedLine_.clear();
    changed();
}

bool EJAgentClient::interceptTyped (const juce::String& typed)
{
    if (askPending())
    {
        if (! pendingAsk_.allowFreeText)
        {
            refusedTypedLine_ = "Tap one of the choices to answer.";   // contract 2.4: allowFreeText false
            log ("typed answer refused (allowFreeText false): \"" + typed + "\"");
            changed();
            return true;
        }
        answerAsk (typed.trim(), true);
        return true;
    }
    if (isActive())
    {
        // A second conversation under a running agent would race the rack it is editing. The send is REFUSED with
        // a line the user sees (a message that vanishes is indistinguishable from a dropped send - 08c item C).
        refusedTypedLine_ = "The agent is still working on \"" + goal_ + "\". Stop it first, or wait for it to finish.";
        log ("typed message refused while active: \"" + typed + "\"");
        changed();
        return true;
    }
    return false;
}

// =============================================================================
//  transport
// =============================================================================
void EJAgentClient::post (const juce::String& path, const juce::String& body)
{
    const auto t = transport_();
    lastPath_ = path; lastBody_ = body;
    const int gen = generation_;
    auto cancel = std::make_shared<Cancel>();
    cancel_ = cancel;
    auto alive = alive_;
    lastBytesMs_ = nowMs();
    setState (State::Connecting);
    changed();
    if (postOverride_) { log ("POST " + path + " (test recorder)"); postOverride_ (path, body); return; }

    juce::String base = t.baseUrl.trim();
    while (base.endsWithChar ('/')) base = base.dropLastCharacters (1);
    const juce::String url = base + path;
    juce::String headers = "Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (t.authToken.isNotEmpty()) headers += "Authorization: Bearer " + t.authToken + "\r\n";
    headers += t.extraHeaders;
    const int connectTimeout = connectTimeoutMs;
    log ("POST " + path + " bytes=" + juce::String (body.getNumBytesAsUTF8()));

    juce::Thread::launch ([this, gen, alive, cancel, url, headers, body, path, connectTimeout]
    {
        echojay::net::Worker netw ("agent " + path);
        // `this` is touched ONLY inside dispatched lambdas, after alive has vouched for it (the chat-stream rule).
        auto dispatch = [alive, cancel] (std::function<void()> fn)
        {
            if (! alive->load() || cancel->isCancelled()) return;
            juce::MessageManager::callAsync ([alive, cancel, fn]
            {
                if (! alive->load() || cancel->isCancelled()) return;
                fn();
            });
        };

        juce::URL u (url);
        u = u.withPOSTData (body);
        juce::WebInputStream ws (u, true);
        ws.withExtraHeaders (headers).withConnectionTimeout (connectTimeout);
        cancel->attach (&ws);
        netw.setStream (&ws);
        const struct ClearSlot { echojay::net::Worker& w; ~ClearSlot() { w.setStream (nullptr); } } clearSlot { netw };

        const bool connected = ws.connect (nullptr);
        const int status = connected ? ws.getStatusCode() : 0;
        if (! connected || status == 0)
        {
            cancel->detach();
            dispatch ([this, gen] { onStreamEnded (gen, false, 0, "Connection failed. Please check your internet connection."); });
            return;
        }
        if (status != 200)
        {
            juce::MemoryBlock mb;
            ws.readIntoMemoryBlock (mb);
            cancel->detach();
            const auto bodyText = juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize());
            const auto msg = errorMessageOf (status, bodyText);
            dispatch ([this, gen, status, msg] { onStreamEnded (gen, false, status, msg); });
            return;
        }

        // 200: the SSE stream is open. ONE-BYTE READS, for the reason EchoJayAPI::startChatStream documents at
        // length: WebInputStream::read FILLS its buffer, so any larger read re-couples our latency to the frame size.
        EJAgentFraming framing;
        char buf[1];
        bool terminal = false;
        int frames = 0;
        while (! ws.isExhausted())
        {
            if (! alive->load() || cancel->isCancelled()) { cancel->detach(); return; }
            const int n = ws.read (buf, 1);
            if (n <= 0) break;
            for (auto& ev : framing.appendChunk (buf, n))
            {
                ++frames;
                auto f = parseFrame (juce::String (ev.event), juce::String::fromUTF8 (ev.data.c_str(), (int) ev.data.size()));
                const bool isTerminal = isTerminalFrame (f.kind);   // the server closes the response after one
                dispatch ([this, gen, f] { onFrame (gen, f); });
                if (isTerminal) { terminal = true; break; }
            }
            if (terminal) break;
        }
        cancel->detach();
        dispatch ([this, gen, terminal, frames] { onStreamEnded (gen, terminal, 200, frames == 0 ? juce::String ("The agent's stream ended before any frame arrived.") : juce::String()); });
    });
}

void EJAgentClient::postStop()
{
    if (sessionId_.isEmpty()) return;
    const auto body = buildStopBody (sessionId_);
    log ("POST /api/agent/stop session=" + sessionId_);
    if (postOverride_) { postOverride_ ("/api/agent/stop", body); return; }
    const auto t = transport_();
    juce::String base = t.baseUrl.trim();
    while (base.endsWithChar ('/')) base = base.dropLastCharacters (1);
    const juce::String url = base + "/api/agent/stop";
    juce::String headers = "Content-Type: application/json\r\n";
    if (t.authToken.isNotEmpty()) headers += "Authorization: Bearer " + t.authToken + "\r\n";
    headers += t.extraHeaders;
    auto logFn = log_;
    juce::Thread::launch ([url, headers, body, logFn]
    {
        echojay::net::Worker netw ("agent /api/agent/stop");
        juce::URL u (url);
        u = u.withPOSTData (body);
        juce::WebInputStream ws (u, true);
        ws.withExtraHeaders (headers).withConnectionTimeout (10000);
        netw.setStream (&ws);
        const struct ClearSlot { echojay::net::Worker& w; ~ClearSlot() { w.setStream (nullptr); } } clearSlot { netw };
        const bool connected = ws.connect (nullptr);
        const int status = connected ? ws.getStatusCode() : 0;
        juce::MemoryBlock mb;
        if (connected) ws.readIntoMemoryBlock (mb);
        if (logFn) logFn ("EJAgent: /api/agent/stop answered " + juce::String (status));   // a log line touches no plugin state
    });
}

void EJAgentClient::onFrame (int generation, Frame f)
{
    if (generation != generation_) return;    // a stopped or superseded session's frame
    lastBytesMs_ = nowMs();
    switch (f.kind)
    {
        case FrameKind::Round:
        {
            if (f.sessionId.isNotEmpty()) sessionId_ = f.sessionId;
            round_ = f.round;
            if (f.ofSoftCap > 0) ofSoftCap_ = f.ofSoftCap;
            awaitIds_.clear(); awaitSeen_ = false; planSubmitted_ = false; roundDoFailed_ = false;
            planHeading_.clear(); awaitTimeoutMs_ = 0;
            liveText_.clear(); roundTalk_.clear(); roundTalkFlushed_ = false;
            log ("frame round session=" + sessionId_ + " round=" + juce::String (round_));
            setState (State::Streaming);
            changed();
            break;
        }
        case FrameKind::Delta:
        {
            if (f.text.isEmpty()) break;
            liveText_ += f.text;
            roundTalk_ += f.text;
            if (state_ == State::Connecting) setState (State::Streaming);
            listeners_.call ([&] (Listener& l) { l.agentTextDelta (f.text); });
            changed();
            break;
        }
        case FrameKind::ToolCall:
        {
            Step s;
            s.call = f.call;
            s.id = f.call.id.isNotEmpty() ? f.call.id : "tc_local_" + juce::String ((int) steps_.size() + 1);
            s.label = describeCall (f.call);
            s.round = round_;
            s.approved = true;
            s.status = f.call.askFirst() ? Step::Status::AwaitingApproval : Step::Status::Pending;
            log ("frame tool_call id=" + s.id + " name=" + f.call.name + " approval=" + f.call.approval + " \"" + s.label + "\"");
            steps_.push_back (std::move (s));
            if (state_ == State::Connecting) setState (State::Streaming);
            changed();
            break;
        }
        case FrameKind::Plan:
        {
            // THE PLAN CARD IS THE SERVER'S (contract 3): one frame per round carrying the heading and the ask-first
            // items in card order. The items' tool_call frames normally precede it; an item with no step yet gets
            // one so the card is never short a line, and the card's summary wins over a derived label.
            planHeading_ = f.planHeading.trim();
            for (const auto& item : f.planItems)
            {
                auto* s = stepById (item.id);
                if (s == nullptr)
                {
                    Step n;
                    n.id = item.id; n.call.id = item.id; n.call.name = "do"; n.round = round_;
                    steps_.push_back (std::move (n));
                    s = &steps_.back();
                }
                if (item.summary.isNotEmpty()) s->label = item.summary;
                if (item.why.isNotEmpty())     s->call.why = item.why;
                s->call.approval = "ask_first";
                if (s->status == Step::Status::Pending) s->status = Step::Status::AwaitingApproval;
                s->approved = true;
            }
            log ("frame plan round=" + juce::String (f.planRound) + " items=" + juce::String ((int) f.planItems.size()) + " heading=\"" + planHeading_ + "\"");
            if (state_ == State::Connecting) setState (State::Streaming);
            changed();
            break;
        }
        case FrameKind::Await:
        {
            awaitIds_ = f.awaitIds;
            awaitTimeoutMs_ = f.awaitTimeoutMs;
            if (awaitIds_.isEmpty())              // an await with no ids means "everything this round"
                for (const auto& s : steps_) if (s.round == round_) awaitIds_.add (s.id);
            awaitSeen_ = true;
            log ("frame await ids=" + awaitIds_.joinIntoString (","));
            beginExecuting();
            break;
        }
        case FrameKind::Done:
        {
            doneSummary_ = f.summary.trim();
            doneReason_ = f.doneReason;
            costUsd_ = f.costUsd;
            if (f.rounds > 0) round_ = f.rounds;
            log ("frame done reason=" + doneReason_ + " rounds=" + juce::String (f.rounds) + " costUsd=" + juce::String (f.costUsd, 3)
                 + " summary=\"" + doneSummary_ + "\"");
            flushRoundTalk();
            // The summary is a transcript line only when it says something the round's talk did not.
            finishSession (State::Done, (doneSummary_.isNotEmpty() && doneSummary_ != roundTalk_.trim()) ? doneSummary_ : juce::String());
            break;
        }
        case FrameKind::Error:
        {
            errorMessage_ = f.errorMessage.isNotEmpty() ? f.errorMessage : (f.errorCode.isNotEmpty() ? f.errorCode : juce::String ("The agent reported an error."));
            errorRetryable_ = f.retryable;
            log ("frame error code=" + f.errorCode + " retryable=" + juce::String (f.retryable ? 1 : 0) + " \"" + errorMessage_ + "\"");
            flushRoundTalk();
            markUnfinishedStopped();
            finishSession (State::Failed, "The agent stopped: " + errorMessage_);
            break;
        }
        default:
            log ("frame unknown event=\"" + f.eventName + "\" skipped");
            break;
    }
}

void EJAgentClient::onStreamEnded (int generation, bool sawTerminal, int statusCode, const juce::String& error)
{
    if (generation != generation_) return;
    if (sawTerminal) return;                   // await / done / error already moved the state
    if (state_ != State::Connecting && state_ != State::Streaming && state_ != State::Posting) return;
    errorMessage_ = error.isNotEmpty() ? error : juce::String ("The agent's stream ended early.");
    errorRetryable_ = (statusCode == 0 || statusCode >= 500 || statusCode == 429);
    log ("stream ended without a terminal frame status=" + juce::String (statusCode) + " \"" + errorMessage_ + "\" retryable=" + juce::String (errorRetryable_ ? 1 : 0));
    flushRoundTalk();
    markUnfinishedStopped();
    finishSession (State::Failed, "The agent stopped: " + errorMessage_);
}

void EJAgentClient::onBytes (int generation)
{
    if (generation != generation_) return;
    lastBytesMs_ = nowMs();
}

// =============================================================================
//  the round
// =============================================================================
int EJAgentClient::nextRunnable (const std::vector<Step>& steps, int round, bool planSubmitted)
{
    for (const auto& s : steps)
        if (s.round == round && s.status == Step::Status::Running) return -1;
    // CONTRACT 3: "free calls in the same round run before the card is shown". So every free (Pending) step runs
    // first, in wire order; only then does an undecided ask_first step raise the card (-2); once the plan is
    // submitted the approved lines run in wire order.
    for (int i = 0; i < (int) steps.size(); ++i)
    {
        const auto& s = steps[(size_t) i];
        if (s.round == round && s.status == Step::Status::Pending) return i;
    }
    for (int i = 0; i < (int) steps.size(); ++i)
    {
        const auto& s = steps[(size_t) i];
        if (s.round == round && s.status == Step::Status::AwaitingApproval) return planSubmitted ? i : -2;
    }
    return -1;
}

void EJAgentClient::beginExecuting()
{
    setState (State::Executing);
    changed();
    advance();
}

void EJAgentClient::advance()
{
    if (state_ != State::Executing && state_ != State::AwaitingApproval && state_ != State::AwaitingAsk && state_ != State::Listening)
        return;
    const int idx = nextRunnable (steps_, round_, planSubmitted_);
    if (idx == -1)
    {
        for (const auto& s : steps_) if (s.round == round_ && s.status == Step::Status::Running) return;   // something is still running
        postStepIfComplete();
        return;
    }
    if (idx == -2)
    {
        if (state_ != State::AwaitingApproval)
        {
            setState (State::AwaitingApproval);
            log ("plan card: " + juce::String ((int) planLines().size()) + " line(s) await a decision");
            changed();
        }
        return;
    }
    if (state_ != State::Executing) { setState (State::Executing); }
    runStep (steps_[(size_t) idx]);
}

void EJAgentClient::runStep (Step& s)
{
    const int gen = generation_;
    const juce::String id = s.id;
    const ToolCall call = s.call;

    if (s.status == Step::Status::AwaitingApproval && ! s.approved)
    {
        s.status = Step::Status::Declined; s.detail = "skipped";
        s.result = juce::var();
        log ("step " + id + " declined by the user");
        changed();
        advance();
        return;
    }
    if (s.isDo() && roundDoFailed_)
    {
        s.status = Step::Status::NotRun; s.detail = "not run: an earlier change in this round failed";
        log ("step " + id + " not run (an earlier do failed this round)");
        changed();
        advance();
        return;
    }

    s.status = Step::Status::Running;
    s.detail.clear();
    log ("step " + id + " RUN " + call.name + " \"" + s.label + "\"");
    changed();

    auto done = [this, gen, id] (ToolOutcome o) { completeStep (gen, id, std::move (o)); };

    if (isPlaybackWait (call))            { beginPlayback (s); return; }
    if (call.name == "talk")
    {
        const auto ask = talkAskOf (call);
        const auto text = argString (call, "text").trim();
        if (ask.present)
        {
            pendingAsk_ = ask;
            if (text.isNotEmpty() && ! ask.question.startsWith (text)) pendingAsk_.question = (text + "\n" + ask.question).trim();
            pendingAskId_ = id;
            setState (State::AwaitingAsk);
            log ("ask card: \"" + pendingAsk_.question + "\" choices=" + pendingAsk_.labels.joinIntoString ("|"));
            changed();
            return;                       // completes in answerAsk
        }
        if (text.isNotEmpty())
        {
            liveText_ += (liveText_.isEmpty() ? "" : "\n") + text;
            roundTalk_ += (roundTalk_.isEmpty() ? "" : "\n") + text;
            listeners_.call ([&] (Listener& l) { l.agentTextDelta (text); });
        }
        done (ToolOutcome::success (juce::var (new juce::DynamicObject())));
        return;
    }
    if (call.name == "look")  { executor_.look  (call, done); return; }
    if (call.name == "do")    { executor_.doOp  (call, done); return; }
    if (call.name == "check") { executor_.check (call, done); return; }
    done (ToolOutcome::failure ("unknown_tool", "this client knows look, do, check, talk and wait_for_playback; not \"" + call.name + "\""));
}

void EJAgentClient::completeStep (int generation, const juce::String& id, ToolOutcome o)
{
    if (generation != generation_) return;    // the session this ran for is gone
    auto* s = stepById (id);
    if (s == nullptr || s->isFinal()) return;
    if (o.ok)
    {
        s->status = Step::Status::Done;
        s->detail = o.landedLine;
        s->undoToken = o.undoToken;
        s->result = o.result;
        log ("step " + id + " DONE" + (o.undoToken.isNotEmpty() ? " undo=" + o.undoToken : juce::String()) + (o.landedLine.isNotEmpty() ? " \"" + o.landedLine + "\"" : juce::String()));
    }
    else
    {
        s->status = Step::Status::Failed;
        s->detail = o.errorMessage.isNotEmpty() ? o.errorMessage : o.errorCode;
        s->result = juce::var();
        if (s->isDo()) roundDoFailed_ = true;
        log ("step " + id + " FAILED code=" + o.errorCode + " \"" + o.errorMessage + "\"");
    }
    // The outcome's error travels on the step; keep the code where postStepIfComplete reads it.
    if (! o.ok)
    {
        auto* eo = new juce::DynamicObject();
        eo->setProperty ("code", o.errorCode);
        eo->setProperty ("message", o.errorMessage);
        s->result = juce::var (eo);
    }
    changed();
    advance();
}

void EJAgentClient::postStepIfComplete()
{
    if (! awaitSeen_) return;
    std::vector<ToolResult> results;
    for (const auto& id : awaitIds_)
    {
        const auto* s = stepById (id);
        if (s == nullptr)
        {
            results.push_back (failedResult (id, "unknown_id", "no tool_call frame carried this id"));
            continue;
        }
        if (! s->answered()) return;          // not yet: something is pending, awaiting, running, or the user stopped
        switch (s->status)
        {
            case Step::Status::Done:     results.push_back (okResult (id, s->result)); break;
            case Step::Status::Declined: results.push_back (declinedResult (id)); break;
            case Step::Status::NotRun:   results.push_back (notRunResult (id)); break;
            case Step::Status::Failed:
            {
                juce::String code = "failed", msg = s->detail;
                if (auto* eo = s->result.getDynamicObject())
                {
                    if (eo->getProperty ("code").toString().isNotEmpty()) code = eo->getProperty ("code").toString();
                    if (eo->getProperty ("message").toString().isNotEmpty()) msg = eo->getProperty ("message").toString();
                }
                results.push_back (failedResult (id, code, msg));
                break;
            }
            default: return;
        }
    }
    awaitSeen_ = false;                       // one step per await, never two
    flushRoundTalk();
    for (const auto& r : results)             // contract 4: a result over 8 KB is rejected with result_too_large
        if (r.ok && juce::JSON::toString (r.result, true).getNumBytesAsUTF8() > 8192)
            log ("WARNING result " + r.id + " exceeds 8 KB - the server will reject it (the executor must send the summary shape)");
    log ("STEP round=" + juce::String (round_) + " results=" + juce::String ((int) results.size()));
    post ("/api/agent/step", buildStepBody (sessionId_, round_, results));
}

void EJAgentClient::flushRoundTalk()
{
    if (roundTalkFlushed_) return;
    roundTalkFlushed_ = true;
    const auto t = roundTalk_.trim();
    if (t.isEmpty()) return;
    listeners_.call ([&] (Listener& l) { l.agentTranscript ("assistant", t); });
}

void EJAgentClient::finishSession (State s, const juce::String& line)
{
    playback_.active = false; playbackStepId_.clear();
    pendingAsk_ = {}; pendingAskId_.clear();
    stopTimer();
    setState (s);
    if (line.isNotEmpty())
        listeners_.call ([&] (Listener& l) { l.agentTranscript ("assistant", line); });
    changed();
}

// =============================================================================
//  the plan card
// =============================================================================
std::vector<const EJAgentClient::Step*> EJAgentClient::planLines() const
{
    std::vector<const Step*> out;
    if (planSubmitted_) return out;
    for (const auto& s : steps_)
        if (s.round == round_ && s.status == Step::Status::AwaitingApproval) out.push_back (&s);
    return out;
}

void EJAgentClient::setLineApproved (const juce::String& id, bool approved)
{
    if (auto* s = stepById (id))
        if (s->status == Step::Status::AwaitingApproval) { s->approved = approved; changed(); }
}

void EJAgentClient::approveAll()
{
    for (auto& s : steps_) if (s.round == round_ && s.status == Step::Status::AwaitingApproval) s.approved = true;
    submitPlan();
}

void EJAgentClient::declineAll()
{
    for (auto& s : steps_) if (s.round == round_ && s.status == Step::Status::AwaitingApproval) s.approved = false;
    submitPlan();
}

void EJAgentClient::submitPlan()
{
    if (state_ != State::AwaitingApproval) return;
    int yes = 0, no = 0;
    for (const auto& s : steps_) if (s.round == round_ && s.status == Step::Status::AwaitingApproval) (s.approved ? yes : no)++;
    planSubmitted_ = true;
    log ("plan submitted approved=" + juce::String (yes) + " declined=" + juce::String (no));
    setState (State::Executing);
    changed();
    advance();
}

// =============================================================================
//  the ask card
// =============================================================================
void EJAgentClient::answerAsk (const juce::String& choiceIn, bool typed)
{
    if (! askPending()) return;
    const auto choice = choiceIn.trim();
    if (choice.isEmpty()) return;
    const auto id = pendingAskId_;
    if (! typed && ! pendingAsk_.labels.contains (choice)) return;   // a tap is one of the offered labels, nothing else
    auto answer = askAnswerVar (choice, typed);                      // {tapped:"<label>"} or {typed:"<text>"}
    log ("ask answered" + juce::String (typed ? " (typed)" : " (tapped)") + ": \"" + choice + "\"" + (id == "tc_keep_going" ? " [soft cap]" : ""));
    listeners_.call ([&] (Listener& l) { l.agentTranscript ("user", choice); });
    pendingAsk_ = {}; pendingAskId_.clear();
    setState (State::Executing);
    if (auto* s = stepById (id)) s->detail = choice;
    completeStep (generation_, id, ToolOutcome::success (answer));
}

// =============================================================================
//  wait_for_playback - the client runs it, and it NEVER hangs
// =============================================================================
void EJAgentClient::beginPlayback (Step& s)
{
    playbackStepId_ = s.id;
    playback_ = {};
    playback_.active = true;
    playback_.minSeconds = playbackMinSeconds (s.call);
    playback_.startedMs = nowMs();
    playback_.timeoutSeconds = playbackTimeoutS;
    playback_.line = "Play the chorus or the loudest section.";
    s.detail = playback_.line;
    executor_.beginPlaybackWindow();
    setState (State::Listening);
    log ("playback wait begins min=" + juce::String (playback_.minSeconds, 1) + "s timeout=" + juce::String (playback_.timeoutSeconds) + "s");
    startTimer (playbackTickMs);
    changed();
}

void EJAgentClient::tickPlayback()
{
    if (! playback_.active) return;
    const auto r = executor_.readPlayback();
    const double heard = r.heardAboveSeconds;
    const double elapsed = (double) (nowMs() - playback_.startedMs) / 1000.0;
    playback_.heardSeconds = heard;

    if (heard >= playback_.minSeconds)
    {
        playback_.reached = true;
        playback_.line = "Got it, you can stop.";
        endPlayback ("min reached");
        return;
    }
    if (r.transportKnown && ! r.playing && heard >= 1.0 && elapsed > 2.0)
    {
        playback_.line = "Playback stopped at " + juce::String (heard, 1) + " s - using what was heard.";
        endPlayback ("transport stopped");
        return;
    }
    if (elapsed >= (double) playback_.timeoutSeconds)
    {
        playback_.line = playbackTimeoutResult (playback_.timeoutSeconds).getProperty ("sentence", {}).toString();
        endPlayback ("timeout");
        return;
    }
    const juce::String next = heard > 0.0
        ? "Listening... " + juce::String ((int) std::floor (heard)) + " of " + juce::String ((int) std::ceil (playback_.minSeconds)) + " s"
        : juce::String ("Play the chorus or the loudest section.");
    if (next != playback_.line)
    {
        playback_.line = next;
        if (auto* s = stepById (playbackStepId_)) s->detail = next;
        changed();
    }
}

void EJAgentClient::playbackGotIt()
{
    if (! playback_.active) return;
    playback_.line = playback_.heardSeconds >= 1.0
        ? "Got it - using the " + juce::String (playback_.heardSeconds, 1) + " s heard."
        : "Stopped listening before any audio was heard.";
    endPlayback ("user");
}

void EJAgentClient::endPlayback (const juce::String& reason)
{
    if (! playback_.active) return;
    playback_.active = false;
    const auto r = executor_.readPlayback();
    const double heard = r.heardAboveSeconds;
    const bool played = reason != "timeout" && heard >= 1.0;
    auto* o = new juce::DynamicObject();
    if (reason == "timeout")
    {   // contract 2.3 / 6: after the plugin's 60 s the answer is this shape and this sentence, verbatim
        auto v = playbackTimeoutResult (playback_.timeoutSeconds);
        o->setProperty ("played", false);
        o->setProperty ("waited", v.getProperty ("waited", {}));
        o->setProperty ("sentence", v.getProperty ("sentence", {}));
    }
    else o->setProperty ("played", played);
    if (played)
    {
        o->setProperty ("seconds", juce::String (heard, 1).getDoubleValue());
        setIfFinite (o, "integratedLufs", r.integratedLufs);
        setIfFinite (o, "loudestLufs", r.loudestShortTermLufs);
        setIfFinite (o, "peakDbtp", r.truePeakDbtp);
        if (heard < playback_.minSeconds) o->setProperty ("short", true);   // less than asked for; the model can ask again
    }
    else if (reason != "timeout")
        o->setProperty ("waited", juce::String ((double) (nowMs() - playback_.startedMs) / 1000.0, 1).getDoubleValue());
    o->setProperty ("endedBy", reason);
    log ("playback wait ends (" + reason + ") heard=" + juce::String (heard, 1) + "s played=" + juce::String (played ? 1 : 0)
         + " line=\"" + playback_.line + "\"");
    const auto id = playbackStepId_;
    playbackStepId_.clear();
    stopTimer();
    startTimer (1000);                        // back to the watchdog cadence
    setState (State::Executing);
    if (auto* s = stepById (id)) s->detail = playback_.line;
    completeStep (generation_, id, ToolOutcome::success (juce::var (o), {}, playback_.line));
}

void EJAgentClient::timerCallback()
{
    if (playback_.active) { tickPlayback(); return; }
    if (! isActive()) { stopTimer(); return; }
    if ((state_ == State::Connecting || state_ == State::Streaming || state_ == State::Posting)
        && nowMs() - lastBytesMs_ > (juce::int64) stallTimeoutMs)
    {
        log ("stall: no frame for " + juce::String (stallTimeoutMs / 1000) + " s in state " + stateName (state_) + " - giving up");
        if (cancel_) cancel_->cancel();
        ++generation_;
        errorMessage_ = "The connection went quiet for " + juce::String (stallTimeoutMs / 1000) + " s.";
        errorRetryable_ = true;
        flushRoundTalk();
        markUnfinishedStopped();
        finishSession (State::Failed, "The agent stopped: " + errorMessage_);
    }
}

// =============================================================================
//  undo
// =============================================================================
bool EJAgentClient::undoAllowedNow() const noexcept
{
    if (undoInFlight_ > 0) return false;
    return state_ == State::Done || state_ == State::Stopped || state_ == State::Failed
        || state_ == State::AwaitingApproval || state_ == State::AwaitingAsk;
}

bool EJAgentClient::canUndoAll() const noexcept
{
    if (checkpoint_.isEmpty()) return false;
    for (const auto& s : steps_) if (s.canUndo()) return true;
    return false;
}

void EJAgentClient::undoStep (const juce::String& id)
{
    if (! undoAllowedNow()) return;
    auto* s = stepById (id);
    if (s == nullptr || ! s->canUndo()) return;
    const int gen = generation_;
    const auto token = s->undoToken;
    ++undoInFlight_;
    log ("undo step " + id + " token=" + token);
    changed();
    executor_.undoStep (token, [this, gen, id] (ToolOutcome o)
    {
        --undoInFlight_;
        if (gen != generation_) { changed(); return; }
        if (auto* st = stepById (id))
        {
            if (o.ok) { st->status = Step::Status::Undone; st->detail = "undone"; st->undoToken.clear(); }
            else        st->detail = "undo failed: " + (o.errorMessage.isNotEmpty() ? o.errorMessage : o.errorCode);
            log ("undo step " + id + (o.ok ? " ok" : " FAILED \"" + o.errorMessage + "\""));
        }
        changed();
    });
}

void EJAgentClient::undoAll()
{
    if (! undoAllowedNow() || ! canUndoAll()) return;
    const int gen = generation_;
    ++undoInFlight_;
    log ("undo all -> checkpoint " + checkpoint_);
    changed();
    executor_.undoToCheckpoint (checkpoint_, [this, gen] (ToolOutcome o)
    {
        --undoInFlight_;
        if (gen != generation_) { changed(); return; }
        if (o.ok)
        {
            for (auto& s : steps_) if (s.canUndo()) { s.status = Step::Status::Undone; s.detail = "undone"; s.undoToken.clear(); }
            refusedTypedLine_ = "Everything the agent changed has been undone.";
        }
        else
            refusedTypedLine_ = "Undo all failed: " + (o.errorMessage.isNotEmpty() ? o.errorMessage : o.errorCode);
        log ("undo all " + juce::String (o.ok ? "ok" : "FAILED \"" + o.errorMessage + "\""));
        changed();
    });
}
