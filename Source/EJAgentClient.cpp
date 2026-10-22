#include "EJAgentClient.h"
#include "EJRefusalLine.h"   // the one refusal composer (20 Oct)
#include "EJAgentFraming.h"
#include "EJParamPlainNames.h"   // plainifyLine: a line the user reads carries words, not ids
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
    ++runsStarted_;                 // one per START: the only honest "this is a new run" before a sessionId
    if (cancel_) cancel_->cancel();
    cancel_.reset();
    steps_.clear();
    awaitIds_.clear();
    awaitSeen_ = false; planSubmitted_ = false; roundDoFailed_ = false;
    liveText_.clear(); roundTalk_.clear(); roundTalkFlushed_ = false;
    // THE LEAK B'S SECTION 92 FOUND: none of these three was cleared here, so a session's SECOND
    // run began believing the first run's closing was still up - and refused its own round 1.
    closingAskSeen_ = false; userAskedForCheck_ = false; moveLandedThisRun_ = false;
    pendingAsk_ = {}; pendingAskId_.clear();
    playback_ = {}; playbackStepId_.clear();
    errorMessage_.clear(); doneSummary_.clear(); errorRetryable_ = false; costUsd_ = 0.0;
    // A NEW RUN OWNS NO OLD BODY (17 Oct 2026). These held the last posted body so Retry could
    // resend it; left standing across a start they are a loaded gun pointing at the next message,
    // which is the second half of what Sean saw - his next message failing with the PREVIOUS run's
    // tool id.
    lastBody_.clear(); lastPath_.clear(); lastErrorCode_.clear();
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
    // His 09:49: the auto-dial-only toggle rides BOTH agent requests now, under the key agent off
    // uses. Asked of the executor fresh rather than cached, because a toggle he flips mid-round
    // means it.
    const auto body = buildStartBody (goal, executor_.startContext(), t.appVersion, chatId,
                                      approvalModeWord(), executor_.autoDialOnly(),
                                      executor_.genreName());
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
    // THE REASON IS THE CARD'S CLOSING LINE (his ruling, 20 Oct 2026): *"its own card says 'Stopped:
    // you moved on to <channel>'"*. A bare "Stopped." is what every other caller means, and is what
    // a Stop button press should say.
    const auto line = stopReason_.isNotEmpty() ? "Stopped: " + stopReason_ : juce::String ("Stopped.");
    stopReason_.clear();
    listeners_.call ([&line] (Listener& l) { l.agentTranscript ("assistant", line); });
    changed();
}

void EJAgentClient::stopWithReason (const juce::String& reason)
{
    // Set BEFORE stop(), which consumes it - so a reason can never outlive the stop it describes and
    // turn up on the next one.
    stopReason_ = reason.trim();
    stop();
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
    // ---- NEVER RESEND THE BYTES THAT WERE REFUSED FOR BEING THOSE BYTES (17 Oct 2026) ----------
    //
    // Sean's screenshot: Retry failed with the SAME tool id as the original, and so did his next
    // message. This function re-posts `lastBody_` verbatim, which is exactly right for a 500 or a
    // timeout - the server never saw it - and exactly wrong when the server DID see it and rejected
    // its content. `result_too_large` is that case: the same bytes will be refused forever, and each
    // press looks to him like the plugin not listening.
    //
    // A content rejection is not retryable by repetition. The body is dropped and so is the run's
    // claim on it; he is told the honest thing instead, and the next message starts clean.
    if (lastErrorCode_ == "result_too_large" || lastErrorCode_ == "too_large_to_send"
        || lastErrorCode_ == "invalid_result")
    {
        log ("RETRY refused: the last failure was \"" + lastErrorCode_ + "\", which is about the CONTENT "
             "of that body - resending the same bytes would be refused the same way. Dropping it.");
        lastBody_.clear(); lastPath_.clear();
        errorRetryable_ = false;
        errorMessage_ = "That answer was too long to send, so trying again would fail the same way. "
                        "Ask me again and I will fetch less at a time.";
        changed();
        return;
    }
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

/** The choice a typed answer MEANS, or "" when it means none of them. Exact first
    (trimmed, case-insensitive), then an unambiguous prefix, then an unambiguous
    containment - so "MTransient" answers "MTransient (free)" and "mt" does not
    answer anything when two choices begin with it. */
juce::String EJAgentClient::matchTypedToChoice (const juce::String& typedIn) const
{
    const auto t = typedIn.trim();
    if (t.isEmpty()) return {};
    for (const auto& l : pendingAsk_.labels) if (l.trim().equalsIgnoreCase (t)) return l;
    juce::String hit; int hits = 0;
    for (const auto& l : pendingAsk_.labels)
        if (l.trim().startsWithIgnoreCase (t)) { hit = l; ++hits; }
    if (hits == 1) return hit;
    hit.clear(); hits = 0;
    for (const auto& l : pendingAsk_.labels)
        if (l.containsIgnoreCase (t)) { hit = l; ++hits; }
    return hits == 1 ? hit : juce::String();
}

/** ---- IS THIS TYPED LINE AN ANSWER, OR A NEW INSTRUCTION? (16 Oct 2026) ------------------------

    Sean gave two rules this morning and they meet in one state. The blocker: *"an ask always shows
    its choices AND accepts a typed answer; a typed reply must never be silently dropped."* His #21:
    *"A new request typed while a run waits on an ask got merged into that run. A new instruction
    closes the waiting run (marked stopped) and starts a new run."* When the ask has nothing to tap,
    those two say opposite things about the same keystrokes.

    The joint live pass made it concrete: the real server asks "Pick a transient shaper for the kick"
    with `choices: []`, and B4-live typed "Set the pre-gain to 2 dB" into it five times - and all five
    were swallowed as the answer to the question, which is the exact fault #21 names.

    So the two are told apart the way a person tells them apart: AN ANSWER IS A NAME, AN INSTRUCTION
    IS A SENTENCE. A line that OPENS with one of this product's own command verbs and runs to three
    words or more is an instruction ("Set the pre-gain to 2 dB", "Add Pro-C 2" - his own example).
    "MTransient", "the EchoJay Transient", "elysia museq" are answers: no leading verb, or too short
    to be a sentence. Deliberately narrow in both directions - a bare plugin name can never be read
    as an instruction, and a verb-led sentence is never quietly consumed as an answer.                */
/** Does this typed text NAME a plugin this machine has? (17 Oct 2026, Sean's item 5.)

    Fuzzy, because the chips spell a product's full name ("Tube-Tech CL 1B") and a person types what
    they call it ("cl1b"). Both sides are reduced to letters and digits, lower case, and a hit is one
    containing the other. Three characters minimum, so "a" or "eq" cannot claim to be a name; and
    the comparison is against what is INSTALLED, not against the chips, which is the whole point -
    the chips are the server's guess and the inventory is the fact.

    Returns the installed name it matched, so the log can say which, or empty.                     */
/** The server's error, in words a person can act on (17 Oct 2026).

    One sentence: what happened, then what to do about it. The server's own text is kept for Details
    by the caller - this never throws it away, it just stops it being the headline. Anything with a
    tool id, a byte count or a contract reference in it is replaced rather than dressed up; anything
    unrecognised falls back to the server's sentence with the machinery stripped out of it. */
juce::String EJAgentClient::plainStopLine (const juce::String& code, const juce::String& serverText)
{
    // ---- B'S SECTION 85(1), 19 Oct 2026: THE SERVER'S OWN MESSAGE IS THE HEADLINE -----------
    //
    // *"Show the server's error frame message as it comes, never 'I could not reach the assistant
    // just then'."*
    //
    // This function was written on 17 Oct for the opposite case: a server whose error text carried
    // tool ids and contract references, which is not a sentence for this window. B now writes the
    // sentence, so ours stops being the headline and becomes the FALLBACK for an error frame that
    // carries no words at all. The code-specific sentences below are kept for exactly that.
    //
    // ONE EXCEPTION, AND IT IS SEAN'S OWN RULE OF 17 OCT, WHICH STILL HOLDS: a sentence carrying a
    // tool id, a byte limit or a contract reference is not a sentence for this window
    // (agent_client_guard SL1 and SL3 hold both directions of it). B's section 85 is about a real
    // message being MASKED by a generic one, not about machinery, so the two rulings meet here:
    // the server's words are the headline whenever they are words, and machinery still never is.
    {
        const auto fromServer = serverText.trim();
        const bool machinery = fromServer.contains ("toolu_")
                            || fromServer.containsIgnoreCase ("CONTRACT_")
                            || fromServer.containsIgnoreCase ("8 KB")
                            || fromServer.contains ("_too_")
                            || fromServer.contains ("{");
        if (fromServer.isNotEmpty() && ! machinery) return fromServer;
    }
    const auto c = code.trim().toLowerCase();
    if (c == "result_too_large" || c == "too_large_to_send")
        return "That answer was too big to send in one go. Ask me again and I will fetch less at a time.";
    if (c == "round_failed")        return "I could not reach the assistant just then. Try again.";
    if (c == "model_unavailable")   return "The assistant is busy. Try again in a moment.";
    if (c == "agent_mode_off")      return "Agent mode is switched off on the server, so I cannot run this.";
    if (c == "rate_limited")        return "Too many requests just now. Try again in a moment.";
    if (c == "invalid_result" || c == "bad_request")
        return "I sent something the assistant could not read. Try again.";
    if (c == "stopped")             return "Stopped.";

    // An unknown code: use the server's words, but never its machinery. A sentence carrying a tool
    // id, a byte limit or a document reference is not a sentence for this window.
    auto t = serverText.trim();
    const bool hasMachinery = t.contains ("toolu_") || t.containsIgnoreCase ("CONTRACT_")
                           || t.containsIgnoreCase ("8 KB") || t.contains ("_too_") || t.contains ("{");
    if (t.isEmpty() || hasMachinery)
        return "Something went wrong at the assistant's end and I stopped. Try again.";
    return t;
}

juce::String EJAgentClient::typedNamesAnInstalledPlugin (const juce::String& typed) const
{
    auto squash = [] (const juce::String& in)
    {
        juce::String out;
        for (auto c : in) if (juce::CharacterFunctions::isLetterOrDigit (c)) out << juce::String::charToString (c).toLowerCase();
        return out;
    };
    const auto want = squash (typed);
    if (want.length() < 3) return {};
    // A sentence is not a name, however many plugin words it happens to contain: "add a compressor
    // to the kick" squashes to something that contains "compressor", and taking that as a name would
    // undo #21. The sentence test runs first and wins.
    if (juce::StringArray::fromTokens (typed.trim(), " ", "").size() >= 4) return {};
    for (const auto& installed : const_cast<EJAgentClient*> (this)->executor_.installedPluginNames())
    {
        const auto have = squash (installed);
        if (have.length() < 3) continue;
        if (have.contains (want) || want.contains (have)) return installed;
    }
    return {};
}

bool EJAgentClient::typedLooksLikeNewInstruction (const juce::String& typedIn)
{
    const auto t = typedIn.trim();
    if (t.isEmpty()) return false;
    const auto words = juce::StringArray::fromTokens (t, " \t", "");
    if (words.size() < 3) return false;               // a name, not a sentence
    static const char* kVerbs[] = {
        "set", "add", "remove", "delete", "bypass", "unbypass", "build", "make", "change",
        "raise", "lower", "turn", "undo", "redo", "open", "close", "clear", "swap", "replace",
        "put", "move", "mute", "unmute", "solo", "reset", "match", "level", "dial", "show"
    };
    // ---- THE VERB MAY NOT BE THE FIRST WORD (17 Oct 2026, the rig's B4) ----------------------
    //
    // This read words[0] alone, and B4 types "rig-row-marker: set the pre-gain" - the rig's own
    // marker, then the instruction. The first word is not a verb, so with item 5's change in place
    // the sentence was answered INTO the waiting ask instead of starting its own run, and the row
    // that exists to hold #21 went red. It was right to.
    //
    // A lead-in ending in a colon is not part of the instruction, for the rig's marker and for a
    // person who types "ok: add a compressor" alike. So the verb is looked for at the start AND
    // after the first colon, and nowhere else - scanning every word would make "I don't want you to
    // add anything" an instruction to add.
    auto verbAt = [] (const juce::StringArray& ws) -> bool
    {
        if (ws.isEmpty()) return false;
        const auto first = ws[0].toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz");
        for (const char* v : kVerbs) if (first == v) return true;   // kVerbs is static: no capture
        return false;
    };
    if (verbAt (words)) return true;
    if (t.containsChar (':'))
    {
        const auto after = t.fromFirstOccurrenceOf (":", false, false).trim();
        const auto rest  = juce::StringArray::fromTokens (after, " \t", "");
        if (rest.size() >= 3 && verbAt (rest)) return true;
    }
    return false;
}

bool EJAgentClient::interceptTyped (const juce::String& typed)
{
    if (askPending())
    {
        // THE SENTENCE TEST FIRST, and before the chip match, because "Add Pro-C 2" could otherwise
        // be matched against a choice called "Pro-C 2" and answered as a tap - which is the swallow
        // #21 is about, in its politest form.
        if (typedLooksLikeNewInstruction (typed))
        {
            log ("typed text \"" + typed.trim() + "\" opens with a command verb and is a sentence, not a "
                 "name - taking it as a NEW INSTRUCTION: this run is stopped and the text starts its own "
                 "(Sean's #21, and the live pass's B4)");
            stop();
            return false;
        }
        // ---- SEAN'S BLOCKER, 16 Oct 2026: A TYPED ANSWER IS NEVER DROPPED --------------------------
        //
        // "an ask always shows its choices AND accepts a typed answer; a typed reply must never be
        // silently dropped." The 09:19 run was a dead end: B sent an ask with NO choices and
        // allowFreeText false, so this function answered "Tap one of the choices to answer" about
        // choices that did not exist, and the run hung until Stop. Three roads out, in order:
        //
        //   1. the text NAMES one of the choices -> answer it as a TAP, which is the shape the server
        //      asked for ("MTransient" for a choice labelled "MTransient");
        //   2. free text is allowed (and talkAskOf now forces it on when there is nothing to tap)
        //      -> answer as typed;
        //   3. the server said tap-only and the text matches nothing -> STILL answer, as typed, and
        //      log that the server will be getting free text it said it did not want. Sean's rule is
        //      explicit, and a hung run is worse than an answer the server has to parse.
        const auto asTap = matchTypedToChoice (typed);
        if (asTap.isNotEmpty())
        {
            log ("typed answer \"" + typed.trim() + "\" matched the choice \"" + asTap + "\" - answering as a tap");
            answerAsk (asTap, false);
            return true;
        }
        // ---- SEAN'S #21 (16 Oct 2026): A NEW INSTRUCTION IS NOT AN ANSWER ---------------------------
        //
        // *"A new request typed while a run waits on an ask got merged into that run ('Add Pro-C 2'
        // continued the previous GR run, with its check-gr steps). A new instruction closes the
        // waiting run (marked stopped) and starts a new run."*
        //
        // The two rules meet here, and they do not conflict once the question is asked properly:
        //
        //   there ARE choices on screen and the text matches none of them  -> it is a new instruction.
        //        The run is STOPPED (so the transcript says so) and this returns false, which sends the
        //        text down the ordinary road and starts a run of its own.
        //   there are NO choices at all                                    -> typing is the only way to
        //        answer, so the text IS the answer (Sean's blocker, same morning).
        //
        // Either way the words are acted on. Nothing is dropped, which is the rule both of his
        // instructions share.
        // ---- THE SERVER'S OWN FLAG DECIDES, AND IT WAS RIGHT ALL ALONG (16 Oct 2026) ---------------
        // My first cut of #21 made ANY unmatched text a new instruction while chips were on screen,
        // and the gate caught it: contract 2.4 says `allowFreeText: true` means typing is an answer,
        // so "the museq" against ["EchoJay EQ", "elysia museq"] stopped the run instead of answering
        // it. The two rules do not conflict once the flag is read:
        //
        //   allowFreeText TRUE   typing is an answer, whether or not it names a chip.
        //   allowFreeText FALSE  the server asked for a tap; text that names no chip is therefore a
        //                        NEW INSTRUCTION, which is Sean's #21 exactly ("What now?" with
        //                        [Stop here, Try once more] and "Add Pro-C 2" typed into it).
        //   no choices at all    talkAskOf has already forced the flag true: typing is the only road.
        // ---- A NAME IS AN ANSWER; ONLY A SENTENCE IS A NEW INSTRUCTION (17 Oct 2026, item 5) -----
        //
        // This branch used to read: choices on screen + allowFreeText false + no chip matched =>
        // NEW INSTRUCTION, stop the run. It cost him a run this morning on the clearest possible
        // answer. The server asked "Which CL 1B do you have loaded - which one should I add?" with
        // three chips, he typed **cl1b**, and the run was stopped and restarted on that text as a
        // goal. His ruling: *"A name answers the ask (fuzzy match to installed plugins); only a
        // sentence is a new instruction."*
        //
        // So the test is the one he named, and in his order:
        //   * the text names a plugin this machine HAS -> it is an answer, whatever the flag says
        //     (the chips spelled it "Tube-Tech CL 1B"; he typed what a person types);
        //   * the text is a SENTENCE (verb-led, three words or more) -> a new instruction, which is
        //     #21 intact: "Add Pro-C 2" typed into "What now?" still stops the run;
        //   * anything else -> an answer, because a reply that is not an instruction is a reply, and
        //     a dropped answer is the one outcome both of his rulings forbid.
        if (const auto named = typedNamesAnInstalledPlugin (typed); named.isNotEmpty())
        {
            log ("typed text \"" + typed.trim() + "\" names an installed plugin (\"" + named
                 + "\") - that is an ANSWER to this ask, not a new instruction, even though it matches "
                   "none of the " + juce::String (pendingAsk_.labels.size()) + " chip(s) (Sean's item 5)");
            answerAsk (typed.trim(), true);
            return true;
        }
        if (! pendingAsk_.labels.isEmpty() && typedLooksLikeNewInstruction (typed))
        {
            log ("typed text \"" + typed.trim() + "\" is a SENTENCE and matches none of the "
                 + juce::String (pendingAsk_.labels.size()) + " choice(s) - taking it as a NEW "
                   "INSTRUCTION: this run is stopped and the text starts its own (Sean's #21)");
            stop();
            return false;
        }
        answerAsk (typed.trim(), true);
        return true;
    }
    if (isActive())
    {
        // ---- #21 APPLIES TO ANY WAITING RUN, NOT ONLY AN ASK (16 Oct 2026, Sean's step 7.4) -------
        //
        // His report: while a run waited on Apply ("build 3 slots") he typed "set the pre-gain to
        // 2 dB" and got "The agent is still working on ... Stop it first, or wait for it to finish."
        // His ruling: *"Rule #21 applies to ANY waiting run, not only an ask: a new instruction marks
        // the waiting run stopped and starts its own run."*
        //
        // A run WAITING ON HIM is not a run that is working: it is a question in another shape, and
        // the answer to a question nobody wants to answer is a new instruction. The states below are
        // exactly the ones where the next move is his - a plan awaiting Apply, an ask, and a wait for
        // playback - and the refusal stays for the states where the agent is actually mid-flight
        // (connecting, streaming, executing, posting), where two conversations really would race the
        // rack.
        //
        // Guarded by the same sentence test as the ask road: a NAME is not an instruction, so a
        // one-word reply typed into a waiting plan still reads as a reply and is still refused.
        const bool waitingOnTheUser = state_ == State::AwaitingApproval
                                   || state_ == State::AwaitingAsk
                                   || state_ == State::Listening;
        if (waitingOnTheUser && typedLooksLikeNewInstruction (typed))
        {
            log ("typed text \"" + typed.trim() + "\" is a new instruction and this run is WAITING ("
                 + juce::String (stateName (state_)) + ") - stopping it and letting the text start its "
                 "own run (Sean's #21, step 7.4: it applies to any waiting run, not only an ask)");
            stop();
            return false;
        }
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

        // FIX 2 (Sean, 14 Oct), THE SECOND HALF: A SLOW ROUND IS NOT A BROKEN CONNECTION.
        // Both outcomes arrive here as "connect returned false, status 0", and both used to say "check your
        // internet connection" - which sent Sean to look at his wifi while the server was still thinking.
        // The one fact that separates them is HOW LONG we waited: a refused or unreachable socket fails in
        // milliseconds, and a round that ran out the bound took the bound. 70% of it is far outside the
        // range a real connection failure occupies and well inside a timeout, so the two cannot be confused.
        const juce::uint32 tStart = juce::Time::getMillisecondCounter();
        const bool connected = ws.connect (nullptr);
        const int status = connected ? ws.getStatusCode() : 0;
        if (! connected || status == 0)
        {
            const juce::uint32 waitedMs = juce::Time::getMillisecondCounter() - tStart;
            const bool timedOut = waitedMs > (juce::uint32) ((connectTimeout * 7) / 10);
            const int secs = (int) ((waitedMs + 500) / 1000);
            cancel->detach();
            // The words differ AND so does the code: the panel offers Retry on a timeout (the round may
            // simply have been long), and says what to check on a connection failure.
            juce::String why = timedOut
                ? ("That round took longer than " + juce::String (secs) + " seconds and I stopped waiting. "
                   "Nothing is wrong with your connection - try again, or ask for something smaller.")
                : juce::String ("Connection failed. Please check your internet connection.");
            log (juce::String ("POST ") + path + (timedOut ? " TIMED OUT" : " could not connect")
                 + " after " + juce::String (waitedMs) + " ms (bound " + juce::String (connectTimeout) + " ms)");
            dispatch ([this, gen, why] { onStreamEnded (gen, false, 0, why); });
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
            // HIS WORDS ARE "WHEN THE NEXT STEP APPEARS", and that is HERE - a step appears the
            // moment its tool_call arrives, not when execution begins (leg SF8 caught the
            // difference: the line sat under a card that already had a new row on it).
            statusLine_.clear();
            Step s;
            s.call = f.call;
            s.id = f.call.id.isNotEmpty() ? f.call.id : "tc_local_" + juce::String ((int) steps_.size() + 1);
            // ---- WORDS, NOT IDS, WHOEVER WROTE THE LINE (16 Oct 2026) --------------------------
            // The live pass drew "set slot 1: attack_ms 20, release_ms 60 - on 88_7even Kick_2_02"
            // from the server's own summary, and invariant I4 caught it. The ids are the server's to
            // stop sending (FOR_B), but a line the user reads is this build's responsibility whoever
            // composed it. plainifyLine is deliberately conservative: it rewrites only what is
            // unmistakably an id, so a track called "88_7even Kick_2_02" comes through untouched.
            s.label = echojay::params::plainifyLine (describeCall (f.call));
            // ITEM 2: the card names the track. Appended rather than built into describeCall because only the
            // executor can turn a uid into a name, and describeCall is a pure function the guards rely on.
            if (const auto other = executor_.describeOtherChannel (f.call); other.isNotEmpty())
                s.label += " - on " + other;
            s.round = round_;
            s.approved = true;
            // ---- AUTO MODE'S ONE DECISION (Sean's ruling, 14 Oct 2026) -------------------------------
            // In Auto an ask_first step runs without waiting, UNLESS it cannot be undone - and that
            // question is answered by the executor's undoabilityOf, the same function that decides whether
            // the op hands back an undo token. The reason it gives is what the line shows; the plugin never
            // invents one. A question (talk/ask) is unaffected: it waits in both modes, because the run
            // genuinely needs an answer rather than permission.
            bool autoRan = false;
            if (f.call.askFirst() && approvalMode_ == ApprovalMode::Auto)
            {
                const auto u = executor_.undoabilityOf (f.call, goal_);
                autoRan = u.mayRunInAuto();
                s.askReason = u.whyNot;
            }
            // ---- AND A TAPPED PICK NEEDS NO SECOND PRESS (16 Oct 2026, Sean's step 7) -------------
            //
            // He tapped "Transient Master" from the chips and was then asked to press Apply on "add
            // Transient Master - on 88_7even Kick_2_01". Two gates, each right on its own - the
            // server marks a cross-track add ask_first, Ask mode waits on ask_first - and between
            // them he answered the same question twice. His ruling: "The tap IS the approval,
            // including on another track."
            //
            // So the tap runs the step that carries out THAT pick, and records itself as the
            // cross-channel confirmation at the same moment (the same reasoning as item 2's
            // "the press IS the confirmation", one layer up). Matched on the plugin NAME the call
            // carries, so the tap can only ever cover the thing it chose, and spent immediately so
            // it cannot cover a second step.
            if (! autoRan && f.call.askFirst() && tappedPick_.isNotEmpty())
            {
                const auto named = argString (f.call, "name").trim();
                if (named.isNotEmpty()
                    && (named.equalsIgnoreCase (tappedPick_)
                        || named.containsIgnoreCase (tappedPick_)
                        || tappedPick_.containsIgnoreCase (named)))
                {
                    autoRan = true;
                    if (auto* o = s.call.args.getDynamicObject()) o->setProperty ("confirm", true);
                    else { auto* o2 = new juce::DynamicObject(); o2->setProperty ("confirm", true);
                           s.call.args = juce::var (o2); }
                    log ("the tapped pick \"" + tappedPick_ + "\" IS the approval for this step (\""
                         + named + "\"), including on another channel - no second press, and the tap is "
                         "recorded as the cross-channel confirmation (Sean's step 7)");
                    tappedPick_.clear();
                }
            }
            s.autoApplied = autoRan;
            s.status = (f.call.askFirst() && ! autoRan) ? Step::Status::AwaitingApproval : Step::Status::Pending;
            log ("frame tool_call id=" + s.id + " name=" + f.call.name + " approval=" + f.call.approval
                 + " mode=" + juce::String (approvalModeWord())
                 + (autoRan ? juce::String (" AUTO-APPLIED")
                            : (f.call.askFirst() ? " waits (" + s.askReason + ")" : juce::String()))
                 + " \"" + s.label + "\"");
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
                if (item.summary.isNotEmpty()) s->label = echojay::params::plainifyLine (item.summary);
                // ---- FIX 1(d) (Sean, 14 Oct): A PLAN LINE MUST NEVER SHOW AN EMPTY DESCRIPTION ----------
                //
                // Sean's card read "set slot 2:  - on 88_7even Kick_2_01" - a `set` with NOTHING between the
                // colon and the dash - beside a fully described EQ line, and he pressed Apply all. The words
                // are the server's (this is item.summary), and the gap was where its settings description
                // should have been; B is fixing the summary. But the user should never have to notice a
                // missing description to be warned, so the card says so itself: a summary that describes no
                // values on a line that CARRIES values is marked, before the press, not after.
                if (s->label.trim().endsWith (":") || s->label.contains (":  ") || s->label.contains (": -"))
                {
                    const auto nm = s->call.name;
                    if (nm == "do")
                    {
                        s->label = s->label.replace (":  ", ": (settings I can't read) ")
                                           .replace (": -", ": (settings I can't read) -");
                        if (s->label.trim().endsWith (":")) s->label += " (settings I can't read)";
                        log ("plan line had an EMPTY settings description - marked it for the user (fix 1(d)): \""
                             + s->label + "\"");
                    }
                }
                // ITEM 2 again, for the card's own wording: the server's summary wins on the WORDS, but the
                // track is not the server's to omit - a line the user approves must say what it acts on.
                if (const auto other = executor_.describeOtherChannel (s->call);
                    other.isNotEmpty() && ! s->label.containsIgnoreCase (other))
                    s->label += " - on " + other;
                if (item.why.isNotEmpty())     s->call.why = item.why;
                s->call.approval = "ask_first";
                // AUTO MODE, THE SAME DECISION AS THE TOOL_CALL FRAME. The plan frame marks every item
                // ask_first by contract, so without this a plan card in Auto would wait for a press on
                // steps Auto had already been told to run - the mode would appear to do nothing on exactly
                // the turns that show a plan. One decider, asked in both places.
                if (approvalMode_ == ApprovalMode::Auto)
                {
                    const auto u = executor_.undoabilityOf (s->call, goal_);
                    s->askReason = u.whyNot;
                    if (u.mayRunInAuto())
                    {
                        s->autoApplied = true;
                        if (s->status == Step::Status::AwaitingApproval) s->status = Step::Status::Pending;
                    }
                    else if (s->status == Step::Status::Pending)
                        s->status = Step::Status::AwaitingApproval;
                }
                // ---- THE PLAN FRAME MUST NOT UNDO THE TAP (17 Oct 2026, Sean's item 2, third time) --
                //
                // This line read `else if (s->status == Pending) s->status = AwaitingApproval;` and that
                // is the whole defect. The tool_call frame had just set the step to Pending BECAUSE THE
                // TAP APPROVED IT ("the tapped pick IS the approval", a few lines above, which logged
                // itself and set autoApplied) - and then the plan frame, which marks every item ask_first
                // by contract, flipped it straight back to waiting. The flag said auto-applied and the
                // status said "ask him again", so the card drew Apply and he pressed it, three nights
                // running, on a fix that was working.
                //
                // Found by replaying the real session rather than by reading: B2b's scripted add carries
                // NO plan frame, so this line never ran in the rig and the row passed every time. The
                // real round 5 carries tool_call + plan + await together, and that is the difference.
                //
                // An auto-applied step keeps its status, in both modes. The Auto branch above already
                // said that for its own reason; this is the same sentence for the tap.
                else if (s->status == Step::Status::Pending && ! s->autoApplied)
                    s->status = Step::Status::AwaitingApproval;
                else if (s->autoApplied && s->status == Step::Status::AwaitingApproval)
                {
                    // ...and if the plan frame arrived FIRST, the tap's approval still wins when the
                    // tool_call catches up: the status is corrected here rather than left stale.
                    s->status = Step::Status::Pending;
                    log ("the plan frame would have put the auto-applied step \"" + s->label.substring (0, 50)
                         + "\" back to waiting - it keeps the tap's approval (Sean's item 2)");
                }
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
            // ---- A STOP IN PLAIN WORDS, NEVER CONTRACT TEXT OR A TOOL ID (17 Oct 2026) ---------
            //
            // What Sean read on his screen tonight: "The agent stopped: result
            // toolu_01VtSXwQ7CTqLmVPLGMUao8n is over 8 KB; send the compact summary shape
            // (CONTRACT_AGENT_TOOLS section 5)". Three things wrong with it in a musician's chat -
            // a tool id, a byte limit, and a reference to a document he has never seen - and not one
            // word about what he can do next.
            //
            // The server's own sentence is kept, verbatim, under Details (rawError below), because
            // the next person debugging this needs it. The LINE says what happened and what to do.
            errorMessage_ = plainStopLine (f.errorCode, f.errorMessage);
            rawErrorDetail_ = f.errorMessage;
            errorRetryable_ = f.retryable;
            lastErrorCode_  = f.errorCode;   // so retry() can tell "the server never saw it" from
                                             // "the server saw it and refused its content"
            log ("frame error code=" + f.errorCode + " retryable=" + juce::String (f.retryable ? 1 : 0) + " \"" + errorMessage_ + "\"");
            flushRoundTalk();
            markUnfinishedStopped();
            finishSession (State::Failed, "The agent stopped: " + errorMessage_);
            break;
        }
        case FrameKind::Status:
        {
            // ---- B'S STATUS LINE (his 15:28 request, 21 Oct 2026) ---------------------------
            //
            // *"show the latest status text B sends during the wait ... replacing the previous one
            // as each arrives."*
            //
            // THE LATEST WINS and nothing accumulates: this is the current word about a wait, not an
            // entry in the run's history. A frame carrying no text clears nothing and shows nothing -
            // it is ignored, and said, because an empty status is a frame B did not mean to send.
            // A frame stamped with an OLD round is dropped: B sends `round` on every frame precisely
            // so a late one cannot be drawn over a newer phase.
            const auto t = f.text.trim();
            if (t.isEmpty())
            { log ("frame status with no text - ignored"); break; }
            if (f.round > 0 && round_ > 0 && f.round < round_)
            { log ("frame status \"" + t + "\" is from round " + juce::String (f.round)
                   + " and we are on " + juce::String (round_) + " - dropped"); break; }
            statusLine_ = t;
            log ("frame status \"" + t + "\"");
            changed();
            break;
        }
        default:
            // ---- WHAT IT SAID, NOT JUST THAT IT CAME (17 Oct 2026) ------------------------------
            // Sean's method for this round is to replay a real session's frames into the runner, and
            // the one thing I could not replay was the PROSE: `message` frames are skipped here (the
            // chat stream draws them, the agent parser has no use for them) and the line recorded
            // only that an unknown frame arrived. Two of his items this morning are about the WORDS
            // in those bubbles - a run saying "Added Renaissance Axx to the Mix Bus" while the add
            // was still waiting for Apply, and a bubble repeating the closing - and neither is in
            // any log I keep. A defect I cannot replay is a defect I have to take on trust.
            //
            // So the text comes with it, trimmed to 160 characters: enough to recognise the sentence
            // in a transcript, short enough that a chatty round does not drown the log.
            log ("frame unknown event=\"" + f.eventName + "\" skipped"
                 + (f.text.isNotEmpty() ? " text=\"" + f.text.substring (0, 160).replace ("\n", " / ") + "\""
                                        : juce::String()));
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
    // THE STATUS DESCRIBES A WAIT, AND THE WAIT IS OVER (his 15:28): a step is about to run, so
    // the line goes rather than sitting under a card that has moved on.
    statusLine_.clear();
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

    // ---- A CHECK AFTER A MOVE IS THE USER'S TO ASK FOR (19 Oct 2026, B's section 86(1)) -------
    //
    // *"Only 'Run a check' listens again and verifies. Under the contract, a check(playback) after
    // the move is refused in process unless the user tapped 'Run a check'."*  And Sean: *"your
    // level-match loop is exempt, it's not the agent asking."*
    //
    // The exemption needs no flag: the loudness loop reads the chain tallies in-process and never
    // comes through here. This is only the AGENT asking him to play again, which is what his change
    // removes.
    // ---- AFTER A MOVE, NEVER BEFORE ONE (19 Oct 2026, B's section 92, Sean first) -------------
    // *"at 17:20 your check(playback) refusal fired in round 1 with nothing changed, and B no longer
    // draws a step for it... refuse a playback check only after a move has landed in this run."*
    // Keyed on the landed move, which is the fact the rule is about. See moveLandedThisRun_ for what
    // closingAskSeen_ was getting wrong, and for why it was true in round 1 at all.
    // BOTH, and the guards settled which: keyed on the landed move ALONE, eight legs went red with
    // this refusal's own words in them (PW1/PW3/W3/W4/L4...) - runs that make a change and then
    // measure it IN THE SAME ROUND, which is an ordinary thing for a run to do and not a second
    // listen the user never asked for. Section 92 says never refuse BEFORE a move; section 86(1)
    // says the CLOSING is what offers "Run a check". So a move must have landed AND the closing must
    // be up. Round 1 with nothing changed runs (92). A mid-run measurement runs. Only a listen asked
    // for after the run has closed is declined, which is the one case he was complaining about.
    if (isPlaybackWait (call) && moveLandedThisRun_ && closingAskSeen_ && ! userAskedForCheck_)
    {
        log ("playback wait REFUSED: a change has landed in this run, the closing is up, and the user "
             "has not tapped \"Run a check\" (B's section 86(1)/92, Sean's no-second-listen change)");
        completeStep (generation_, s.id, ToolOutcome::failureFor ("check_not_requested",
                "a listen after a change needs the user's own \"Run a check\" - the closing offers it. "
                "Nothing has been measured and nothing is waiting. Do not retry: close with the "
                "result you have.",
                "I've made the change - tap \"Run a check\" if you'd like me to listen again."));
        return;
    }
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
            // ---- B'S OWN MARKER FOR THE CLOSING (19 Oct 2026, section 86(1)) ------------------
            // *"The closing is a talk ask whose text is the closing, with 'How's it sounding?' and
            // the chips 'Sounds good' / 'Run a check'. Its id starts with tc_check."*  So the
            // closing is identifiable without reading prose, and a playback wait AFTER it is the
            // second listen his change removes. Keyed on the id B names, rather than on "has
            // anything been applied": my first cut used the latter and refused the waits that come
            // BEFORE a closing, which is every wait a run legitimately makes.
            if (id.startsWithIgnoreCase ("tc_check"))
            {
                closingAskSeen_ = true;
                log ("the closing ask is up (" + id + "): only \"Run a check\" listens again now");
            }
            statusLine_.clear();   // waiting on HIM now, not on the server (his 15:28)
    setState (State::AwaitingAsk);
            log ("ask card: \"" + pendingAsk_.question + "\" choices=" + pendingAsk_.labels.joinIntoString ("|")
                 + "  freeText=" + juce::String (pendingAsk_.allowFreeText ? "y" : "n")
                 + (pendingAsk_.choiceKey.isNotEmpty() ? "  key=" + pendingAsk_.choiceKey : juce::String())
                 + (pendingAsk_.labelKey.isNotEmpty()  ? "  labelKey=" + pendingAsk_.labelKey : juce::String()));
            if (pendingAsk_.labels.isEmpty())
                // THE EVIDENCE FOR B, not a guess about B: the ask object exactly as it arrived, so
                // tomorrow's answer is "the choices were under <key>" or "there were none".
                log ("ask card has NOTHING TO TAP - the ask object as it arrived: "
                     + juce::JSON::toString (call.args.getDynamicObject() != nullptr
                                                 ? call.args.getDynamicObject()->getProperty ("ask") : juce::var(), true)
                     + (pendingAsk_.freeTextForced ? "  [allowFreeText was false and has been overridden: typing is the only road out]"
                                                   : juce::String()));
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
        // ---- SEAN'S #10 (16 Oct 2026): THE ROW IS NOT WHERE THE MODEL IS ADDRESSED ------------------
        // "Internal model instructions leak into the step details." They did, through the SUCCESS door:
        // item 3 split the failure path on 13 Oct and left this line reading the model's string whole.
        // The user half on the row, the whole string behind Details and in the result the model gets.
        s->detail      = echojay::params::plainifyLine (o.landedForTheUser());
        s->modelDetail = o.landedLine != s->detail ? o.landedLine : juce::String();
        s->undoToken = o.undoToken;

        s->result = o.result;
        // B's section 92: this run has now moved something, which is what arms the no-second-listen
        // refusal. A `do` that FAILED moved nothing and leaves it alone, so a refused empty set
        // cannot arm it either.
        if (s->isDo()) moveLandedThisRun_ = true;
        if (s->modelDetail.isNotEmpty())
            log ("step " + id + " landed line split: the row shows \"" + s->detail
                 + "\" and the model's sentences stay off it");
        log ("step " + id + " DONE" + (o.undoToken.isNotEmpty() ? " undo=" + o.undoToken : juce::String()) + (o.landedLine.isNotEmpty() ? " \"" + o.landedLine + "\"" : juce::String()));
    }
    else
    {
        s->status = Step::Status::Failed;
        // ---- ITEM 3 (Sean, 13 Oct 2026) ------------------------------------------------------------------
        // This line was `s->detail = o.errorMessage`, and the panel paints detail in red. o.errorMessage is
        // written FOR THE MODEL - it carries imperatives ("repeat this same call with confirm: true") and, by
        // item 6(d)'s design, the full list of channels and uids. All of it went straight to the user.
        // The model's copy still travels untouched on s->result below; the user gets one plain line.
        s->detail      = echojay::params::plainifyLine (o.forTheUser());
        // ---- AND DETAILS IS NOT THE ACK EITHER (20 Oct 2026, his 09:12 run) --------------------
        // *"A refused step shows one plain line ... with the detail behind Details, never the ack
        // text."*  Item 3 split the LINE from the model's string and sent the model's string to
        // Details, which was right about the line and wrong about Details: what he opened was
        //
        //     the rack's ack: status=not_applied, reason=op 1 invalid: unknown operation
        //     "set_level", slots now 2
        //     op 1 invalid: unknown operation "set_level"
        //
        // - the edit protocol talking to whoever sent the ops. A person has no "op 1", no status
        // field and no seq. So what is DISCLOSED is plainified by the same function that writes the
        // line, and the raw ack still reaches the MODEL untouched on s->result below, which is the
        // audience it was written for.
        s->modelDetail = o.errorMessage.isNotEmpty()
                            ? echojay::params::plainifyLine (echojay::plainRefusal (o.errorMessage))
                            : o.errorCode;
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
    // ---- AN OVERSIZED RESULT IS SHRUNK, NOT SENT AND REGRETTED (17 Oct 2026) -------------------
    //
    // This used to LOG A WARNING and post it anyway - "the server will reject it" - and on Sean's
    // machine it did, in round 1 of every run: look(inventory) came back over 8 KB because 357 of
    // his 1,323 plugins cannot load and 17b put every one of them in the result. The warning was
    // right and useless. A line in a log that predicts the failure it is about to cause is not a
    // guard; it is a comment.
    //
    // So the last gate before the wire actually acts: the executor compacts the result (the same
    // fitUnder8K it uses everywhere else), the compacted bytes go out, and the log says what it did
    // and by how much. The run continues with a smaller answer, which is the whole point of the
    // contract's shape.
    // ---- EXCEPT A CENSUS ANSWER, WHICH THE SERVER ASKED FOR WHOLE (18 Oct 2026, B's FOR_A 76) --
    //
    // The server's own inventory sweep stamps its calls `tc_census_<n>` and does not trim their
    // answers at 8 KB - the whole point is that the list reaches the model in ONE step instead of
    // eight. This gate ran before the wire and would have halved it right back, so the cap is
    // raised for those ids only, to the ceiling the executor built the page under. Every other
    // result is measured and compacted exactly as it was.
    for (auto& r : results)
        if (r.ok)
        {
            const int cap = executor_.maxResultBytes (r.id);
            const auto before = (int) juce::JSON::toString (r.result, true).getNumBytesAsUTF8();
            if (before <= cap) continue;
            r.result = executor_.compactResult (r.result);
            const auto after = (int) juce::JSON::toString (r.result, true).getNumBytesAsUTF8();
            log ("result " + r.id + " was " + juce::String (before) + " bytes, over the "
                 + juce::String (cap / 1024) + " KB this call is allowed - compacted to "
                 + juce::String (after) + " before sending (it is the ANSWER that gets smaller, never "
                 "the run that dies)");
            if (after > cap)
            {
                // Still too big: send the failure the model can act on rather than bytes the server
                // will refuse. It names the tool, not the limit, because the model chooses the next
                // move from the sentence.
                r = failedResult (r.id, "too_large_to_send",
                                  "that answer was too long to send even after shortening - ask for less of "
                                  "it at a time (a narrower match, or a smaller limit)");
                log ("result " + r.id + " is STILL over " + juce::String (cap / 1024) + " KB after "
                     "compacting - sending a short failure instead, so the round can continue");
            }
        }
    log ("STEP round=" + juce::String (round_) + " results=" + juce::String ((int) results.size()));
    post ("/api/agent/step", buildStepBody (sessionId_, round_, results, executor_.autoDialOnly()));
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
    statusLine_.clear();   // the run is over; a status cannot outlive its wait

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
    // ---- ITEM 2 (Sean, 13 Oct 2026): ONE OK, NOT TWO ----------------------------------------------------
    //
    // Sean pressed Apply on the plan and the executor refused with "This is on the kick's channel, go ahead?".
    // Both gates are right on their own: the card asks the user, and doOp refuses a cross-channel edit without
    // confirm:true. Nobody connected them, so the user's press was thrown away and the same question was asked
    // again, in worse words, by a layer below.
    //
    // The press IS the confirmation, and it is recorded on the call here - at the moment the decision becomes
    // final - rather than inside doOp, because doOp must keep failing closed for a call that NO user approved.
    // A declined line gets nothing, so a later re-run of it still has to ask.
    for (auto& s : steps_)
    {
        if (s.round != round_ || s.status != Step::Status::AwaitingApproval || ! s.approved) continue;
        if (auto* o = s.call.args.getDynamicObject()) o->setProperty ("confirm", true);
        else { auto* o2 = new juce::DynamicObject(); o2->setProperty ("confirm", true); s.call.args = juce::var (o2); }
    }
    planSubmitted_ = true;
    log ("plan submitted approved=" + juce::String (yes) + " declined=" + juce::String (no));
    setState (State::Executing);
    changed();
    advance();
}

// =============================================================================
//  the ask card
// =============================================================================
// ---- A TAP IS NEVER DROPPED IN SILENCE (18 Oct 2026, Sean's item 4) ----------------------------
//
// *"Never drop a tap silently."*  Every one of the three refusals below used to be a bare `return`:
// the chip went down, nothing moved on screen, and a press that does nothing is indistinguishable
// from a broken build - which is how an afternoon gets spent on the wrong suspect. The BEHAVIOUR is
// unchanged (a tap that is not one of the offered labels still sends nothing, which is leg A5b's
// rule), and each refusal now says what it found, in the amber line the row already draws.
void EJAgentClient::answerAsk (const juce::String& choiceIn, bool typed)
{
    const auto choice = choiceIn.trim();
    // 19 Oct 2026 (B's 86(1)): the one chip that buys another listen, matched on the words the
    // contract names and on nothing else - a tap meaning "sounds good" must not open a window.
    if (choice.equalsIgnoreCase ("Run a check"))
    {
        userAskedForCheck_ = true;
        log ("the user tapped \"Run a check\": one more listen is allowed");
    }
    if (! askPending())
    {
        log ("tap \"" + choice + "\" arrived with no question waiting - saying so, not dropping it");
        refusedTypedLine_ = "That question is already answered.";
        changed();
        return;
    }
    if (choice.isEmpty())
    {
        log ("an EMPTY answer arrived (" + juce::String (typed ? "typed" : "tapped") + ") - saying so");
        refusedTypedLine_ = "I did not catch that - tap one of the choices or type your answer.";
        changed();
        return;
    }
    const auto id = pendingAskId_;
    if (! typed && ! pendingAsk_.labels.contains (choice))
    {
        // The server's own chips are the only taps that mean anything; a label from an older card
        // (a redraw, a reused row) is not an answer to THIS question.
        log ("tap \"" + choice + "\" is none of the " + juce::String (pendingAsk_.labels.size())
             + " choice(s) on the card - refused, and said so");
        refusedTypedLine_ = "That choice is not on this question any more - tap one of the buttons, "
                            "or type your answer.";
        changed();
        return;
    }
    auto answer = askAnswerVar (choice, typed);                      // {tapped:"<label>"} or {typed:"<text>"}
    // ---- THE TAP IS THE APPROVAL (16 Oct 2026, Sean's step 7) ---------------------------------
    // Remembered here, spent on the step that carries out this pick (see the ToolCall frame). A
    // TYPED answer is deliberately not remembered: he ruled on the tap, and free text can say
    // anything, so it goes on asking.
    if (! typed) tappedPick_ = choice;
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
    // The listen is happening, so the tap is spent: one tap buys one listen.
    userAskedForCheck_ = false;
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
        // ---- ONE SURFACE AT A TIME (19 Oct 2026, Sean at 11:06) -------------------------------
        // *"'Listening... 3 of 8 s' draws twice (step + line under it)."*  It did: this wrote the
        // progress text into the STEP's detail as well as into playback_.line, and the card draws
        // both - the step row, and the playback block with its ring and its progress arc.
        //
        // The block is the richer surface and the one he watches, so it owns the line WHILE the
        // playback is live. The step's detail is what the record keeps afterwards, so it is written
        // at the END, with the outcome, by endPlayback - not with a progress tick that stops being
        // true the moment it is drawn.
        playback_.line = next;
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
    // 19 Oct 2026: the step's detail is written HERE, once, with what actually happened - so the
    // card's record of the wait says how it ended rather than whatever the last tick had drawn.
    // (Set below, after the sentence for this reason is known.)
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
        // ---- EACH FIGURE WITH ITS OWN WINDOW, AND NEVER AN IMPOSSIBLE PAIR (19 Oct 2026) -------
        //
        // *"check playback still says loudest 3 s -17.5 LUFS under integrated -14.1... put both on
        // the same window, or name each window in the payload so B can label it truthfully."*
        //
        // This is the THIRD site that reports a loudest-3 s - check(level) and analysisVar were
        // fixed this morning and this one was missed, which is why he saw it again. Same rule: a max
        // below the mean cannot be true of one span, so it is withheld with its reason; and the
        // windows are named either way, so B can label what it does show.
        {
            const bool bothKnown = std::isfinite (r.loudestShortTermLufs) && std::isfinite (r.integratedLufs);
            const bool impossible = bothKnown && r.loudestShortTermLufs < r.integratedLufs - 0.5f;
            if (! impossible) setIfFinite (o, "loudestLufs", r.loudestShortTermLufs);
            else
                o->setProperty ("loudestUnavailable",
                                "the loudest-3 s figure on that track is measured over a different span "
                                "from its integrated level, so the two cannot be compared and it is not "
                                "reported. The integrated figure is sound.");
            if (r.loudestWindowSeconds > 0.0f)
                o->setProperty ("loudestWindowSeconds",
                                juce::String (r.loudestWindowSeconds, 1).getDoubleValue());
            o->setProperty ("integratedWindow", r.integratedIsLongerWindow
                                ? "longer than this listen - that track's EchoJay keeps its integrated "
                                  "level since its rack last changed"
                                : "this listen");
            if (impossible)
                log ("playback: WITHHELD loudestLufs " + juce::String (r.loudestShortTermLufs, 1)
                     + " - below the integrated " + juce::String (r.integratedLufs, 1)
                     + " LUFS, which cannot be true of one span (his 12:33)");
        }
        setIfFinite (o, "peakDbtp", r.truePeakDbtp);
        // ---- B'S SECTION 86(2): THE PROFILE'S CURVE WANTS PLAIN RMS ---------------------------
        // *"check(playback) sends LUFS only, so the server uses the loudest-3-second LUFS as a
        // stand-in and logs [agent-first-listen] proxy: true. Please add the fields below."*
        // Sent only when they are genuinely known: a profile fed a stand-in it believes is a
        // measurement is worse off than one that knows it has none.
        if (r.plainRmsKnown)
        {
            o->setProperty ("loudestRmsDbfs", juce::String (r.loudestRmsDbfs, 1).getDoubleValue());
            o->setProperty ("typicalRmsDbfs", juce::String (r.typicalRmsDbfs, 1).getDoubleValue());
        }
        setIfFinite (o, "loudestPeakDbfs", r.loudestPeakDbfs);
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
        // ---- HIS SENTENCE, AT HIS BOUND (18 Oct 2026, item 4) ---------------------------------
        // *"30 s with nothing back = a plain sentence with Retry."*  The wait was 90 s and the line
        // read "The connection went quiet for 90 s", which is a diagnosis of the network rather
        // than a thing he can act on. Retry was already offered (errorRetryable_), and it stays.
        errorMessage_ = "Nothing came back for " + juce::String (stallTimeoutMs / 1000)
                      + " seconds, so I stopped waiting.";
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
    // ---- AND IT HAS TO BE THE LAST UNDOABLE STEP (16 Oct 2026) ---------------------------------
    // The sink pops the plugin-wide history once, so undoing anything but the top entry would
    // revert a different change and report it against this line. The view only draws the button on
    // the last undoable step; this is the same rule at the door, so a stale snapshot or a rig press
    // cannot get round it. "Undo all" is the road for anything earlier.
    {
        int undoable = 0;
        for (const auto& st : steps_) if (st.canUndo()) ++undoable;
        if (undoable != 1)
        {
            log ("undo step " + id + " REFUSED: this run has " + juce::String (undoable)
                 + " undoable steps and the plugin's history COALESCES consecutive edits, so one pop "
                   "would revert more than this line - \"Undo all\" is the road back");
            return;
        }
    }
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
