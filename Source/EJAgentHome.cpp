// =============================================================================
//  EJAgentHome.cpp — ITEM 0b: the agent's session, executor and record, owned by
//  the PROCESSOR so a closing window cannot end a run. See EJAgentHome.h for what
//  moved, what did not, and why.
//
//  The eight sinks below are the editor's own, moved VERBATIM apart from three
//  mechanical changes, each of which is the point of the move:
//
//    processorRef.                 ->  proc_.
//    api.                          ->  proc_.getApi().
//    SafePointer<EchoJayEditor>    ->  an alive flag on this object
//
//  Nothing about what a sink DOES changed. A rewritten sink would have made this
//  commit a behaviour change wearing a refactor's clothes, and every sentence in
//  them was settled by one of Sean's rulings between 13 and 15 October.
// =============================================================================
#include "EJAgentHome.h"
#include "PluginProcessor.h"
#include "EJAgentExecutorDo.h"
#include "ChainHost.h"
#include "EJStateRoot.h"
#include "NativeClip.h"

// =============================================================================================
EJAgentHome::EJAgentHome (EchoJayProcessor& proc) : proc_ (proc)
{
auto sources = echojay::agent::bindToProcessor (proc_);
echojay::agent::DoSinks sinks;
// ONE OP VOCABULARY. The executor translates a `do` into the chain channel's own `edit` array and
// sends it through the path the editor already uses for a Link, so an agent's change takes exactly
// the route a user's Apply takes - including the staleness guard and the ack.
//
// ---- ITEM 1 (Sean, 13 Oct 2026): THIS SINK USED TO REPORT A FILE WRITE AS A LANDING -----------
//
// It was `cb (seq >= 0, "sent to the rack")`. seq >= 0 means writeChainEditCommand wrote a JSON
// file. It says nothing about whether the Link read it, accepted it, or exists. Sean approved a
// build for the Kick, the agent reported it applied, the Kick's Link had nothing on it, and the
// agent then read the empty rack for 18 rounds - every one of those rounds downstream of this
// sentence being false.
//
// And the V2's OWN rack never went anywhere at all: writeChainEditCommand refuses an empty uid,
// so every agent `do` on the open rack returned -1 and "that rack would not take the change".
// Two separate silent failures behind one line.
sinks.applyChainOps = [this] (const juce::String& uid, const juce::var& ops,
                              std::function<void (bool, juce::String, juce::String)> cb)
{
    // ---- THE V2'S OWN RACK: applied in-process, through the same applier a user's Apply uses --
    if (uid.isEmpty())
    {
        // ---- ITEM 1 (Sean, 14 Oct 2026): THE ENVELOPE, NOT A BARE ARRAY ----------------------
        //
        // parseChainEditOps takes the chain channel's whole COMMAND object and reads its "edit"
        // member: `auto* o = v.getDynamicObject(); if (o == nullptr) return out;`. Handed a bare
        // array it gets a null object and returns zero ops - which is precisely what Sean saw
        // ("add EchoJay Compressor" -> "that change carried no operation this rack understands",
        // twice, then the agent stopped). The Link arm below was never wrong about this because
        // writeChainEditCommand builds the envelope itself; this arm called the parser directly
        // and skipped it. The parse now gets what it documents.
        auto* envelope = new juce::DynamicObject();
        envelope->setProperty ("edit", ops);
        auto parsed = ChainHost::parseChainEditOps (juce::JSON::toString (juce::var (envelope), true));
        if (parsed.empty())
        {
            EchoJay_NSLog (("EJAgent: the own rack parsed NO ops from " + juce::JSON::toString (ops, true)
                            + " - the op names are the chain channel's, and this one is not among them").toRawUTF8());
            if (cb) cb (false, "that change carried no operation this rack understands",
                        "parseChainEditOps returned 0 ops from: " + juce::JSON::toString (ops, true));
            return;
        }
        // deliberateLocalEdit = TRUE (Sean's ruling, 15 Oct 2026). This is the agent acting on the
        // user's instruction on the rack THIS window has open - as deliberate as a hand on the knob.
        // It is what makes the pre-gain count as user-set, so the level-match loop leaves it alone and
        // an undo restores the figure the user had rather than the AUTO value. A batch that arrives
        // from another instance keeps the default (false), which is the remote move the op's own
        // comment defends.
        proc_.getChainHost().applyChainEdits (
            std::move (parsed), /*expectedRevision*/ -1, /*baseSlots*/ {},
            [this, cb] (const juce::StringArray& results, int appliedCount, bool abortedBeforeStart)
            {
                // SEAN'S #3, the own-rack arm: applyChainEdits bumps the revision, which the panel's
                // own signature watches - but the agent can change a rack this window is NOT viewing
                // (it has a Link selected while working on the open rack), and then nothing rebuilt.
                if (! abortedBeforeStart && appliedCount > 0 && proc_.onAgentChangedRack)
                    proc_.onAgentChangedRack ({});
                if (! cb) return;
                if (abortedBeforeStart || appliedCount <= 0)
                    cb (false, results.isEmpty() ? juce::String ("the rack refused the change")
                                                 : results.joinIntoString ("; "),
                        // the TECHNICAL account: every per-op result, one per line, plus whether the
                        // batch was refused before it started - which is a different failure from
                        // "every op was tried and none applied".
                        (abortedBeforeStart ? juce::String ("the batch was refused before it started\n")
                                            : juce::String())
                        + results.joinIntoString ("\n"));
                else
                    cb (true, results.joinIntoString ("; "), {});
            },
            /*onProgress*/ {}, /*deliberateLocalEdit*/ true);
        return;
    }
    // ---- A LINK: written, then WAITED FOR, then reported ------------------------------------
    const int seq = proc_.writeChainEditCommand (uid, ops, juce::var(), "agent", {});
    if (seq < 0) { if (cb) cb (false, "that rack would not take the change", {}); return; }
    proc_.awaitChainAck (uid, seq, [this, uid, cb] (bool ok, juce::String sentence, juce::String technical, int)
    {
        // ---- SEAN'S #3 (16 Oct 2026): TELL THE WINDOW THE RACK MOVED ---------------------------
        // The 14 Oct refresh sits on the BUILD path; an `add` takes this one, so nothing told the
        // view - "stays empty / Reading 0 plugin(s) until I switch racks". Raised only on success,
        // and only here, where the Link has actually acknowledged the change.
        if (ok && proc_.onAgentChangedRack) proc_.onAgentChangedRack (uid);
        if (cb) cb (ok, sentence, technical);
    });
};
// ---- ITEM 1 (Sean, 14 Oct 2026): A BUILD TAKES THE SAME ROAD THE BUILD BUTTON TAKES --------
//
// Two roads, one per target, and they are the SAME two the user's own Build press uses (the
// router at the chainBuildBtns onClick): loadChainFromJson for the open rack, sendChainToLink
// for a Link. Not re-implemented here - an agent build that took a private road would be a
// second build path, and this codebase already recorded what that costs: "two build paths that
// looked identical in the log is what let a whole class of failure sit undiagnosed".
//
// replaceConfirmed = TRUE, deliberately. A build replaces the rack, and loadChainFromJson would
// otherwise raise its own replace ask - a second question about a build the user has already
// approved on the plan card, which is item 2's complaint arriving by another door.
//
// The callback reports DISPATCH only. Whether the build landed is settled by the executor's
// read-back of the target rack, because that is the only thing that knows.
sinks.buildChain = [this] (const juce::String& uid, const juce::var& chainArray,
                           std::function<void (bool, juce::String)> cb)
{
    buildChain (uid, chainArray, std::move (cb));
};
// ITEM 1: the executor's clock. A structural change is re-read until it appears or the wait runs
// out; the executor holds no timer of its own so a guard can drive the same code with a fake one.
sinks.afterMs = [] (int ms, std::function<void()> fn)
{
    if (fn) juce::Timer::callAfterDelay (ms, std::move (fn));
};
// READ LIVE, both of them. A user who turns agent mode off mid-round means it, and a cached
// answer would let the rest of the round through.
sinks.agentModeOn = [] { return echojay::agentmode::on(); };
sinks.echoJayOnly = [this] { return proc_.getApi().getEchoJayOnly(); };
// ---- ITEM 10 (Sean, 13 Oct 2026): THE RACK'S OUT GAIN ------------------------------------------
// Two different controls behind one op, because "the rack's OUT" is a different object on each
// side: this instance's bus gain, or the Link's mixer-strip GAIN. Both are the control the
// levelling loop's `rack_out` stage already writes, so an agent's change and a levelling landing
// move the SAME gain - which is what makes the OUT caption able to describe either of them.
sinks.setRackOutDb = [this] (const juce::String& uid, double db, bool absolute,
                             std::function<void (bool, juce::String)> cb)
{
    const auto say = [] (double v) { return (v >= 0.0 ? juce::String ("+") : juce::String())
                                          + juce::String (v, 1) + " dB"; };
    if (uid.isEmpty())
    {
        const double now = (double) proc_.getBusGainDb();
        const double next = juce::jlimit (-24.0, 24.0, absolute ? db : now + db);
        proc_.setBusGainDb ((float) next);
        // READ IT BACK, even here. This is an in-process setter and the write cannot plausibly
        // fail - but "cannot plausibly fail" is the sentence that shipped every bug in this
        // family, and the gain is clamped internally, so the figure the user is told should be
        // the figure the rack HAS rather than the one we asked for.
        const double got = (double) proc_.getBusGainDb();
        if (std::abs (got - next) > 0.05)
            EchoJay_NSLog (("EJAgent: the rack OUT was asked for " + say (next) + " and reads "
                            + say (got) + " - the clamp or another writer moved it").toRawUTF8());
        if (cb) cb (true, "the rack's OUT is now " + say (got)
                          + (absolute ? juce::String() : " (was " + say (now) + ")"));
        return;
    }
    // A LINK: the registry row carries its current gain, so a delta has something to be a delta
    // of. Without the row there is no reading to add to, and a delta cannot be honoured - said
    // plainly rather than silently treated as absolute, which would jump the fader.
    double now = 0.0; bool found = false;
    for (const auto& li : proc_.getLinkSlotInfos())
        if (li.uid == uid) { now = (double) li.gainDb; found = true; break; }
    // No row means no address AND no reading - so neither an absolute set nor a delta can be
    // honoured, and refusing says so once instead of jumping the fader from an assumed 0 dB.
    if (! found) { if (cb) cb (false, "that Link is not in the registry, so its OUT cannot be set"); return; }
    const double next = juce::jlimit (-24.0, 24.0, absolute ? db : now + db);
    proc_.writeLinkCtrlCommand (uid, "gainDb", juce::var (next));
    // ---- THE DEAD-PATH SWEEP CAUGHT THIS ONE IN MY OWN CODE (14 Oct 2026) -------------------
    //
    // It said "that Link's OUT is now +2.0 dB" the instant writeLinkCtrlCommand returned. That
    // function writes a control file; it is not an acknowledgement, and the Link may be gone,
    // refusing, or slow. This is the same sentence-shaped fault as item 1's "sent" reported as
    // "applied", written one day later in the fix for item 10.
    //
    // So it polls the registry - which is where the Link PUBLISHES its own gain - until the
    // figure appears, and reports what it reads. Bounded, and if it never appears the user is
    // told that instead of a number nothing confirmed.
    {
        // ---- ITEM 0b: THE POLL NO LONGER DIES WITH THE WINDOW -----------------------------
        // It used to hold a SafePointer to the EDITOR and give up when that went null - so
        // closing the window mid-poll meant the agent never learned whether the Link took the
        // gain, and the step hung on a callback that would never come. The only thing that must
        // not outlive its owner is the POLL, and its owner is now this object, which lives as
        // long as the processor. An alive flag, not an address (fix 6's rule).
        auto alive = alive_;
        auto* self = this;
        auto attempt = std::make_shared<int> (0);
        auto step = std::make_shared<std::function<void()>>();
        *step = [alive, self, uid, next, now, absolute, say, cb, attempt, step]
        {
            if (! alive->load()) return;                     // the plugin went: nothing to claim
            double seen = 0.0; bool found = false;
            for (const auto& li : self->proc_.getLinkSlotInfos())
                if (li.uid == uid) { seen = (double) li.gainDb; found = true; break; }
            if (found && std::abs (seen - next) <= 0.1)
            {
                if (cb) cb (true, "that Link's OUT is now " + say (seen)
                                  + (absolute ? juce::String() : " (was " + say (now) + ")"));
                return;
            }
            if (++*attempt >= 12)                            // 12 x 250 ms = 3 s
            {
                if (cb) cb (false, "that Link did not confirm the new OUT within 3 s - it still reads "
                                   + (found ? say (seen) : juce::String ("nothing I can read"))
                                   + ", so nothing is confirmed");
                return;
            }
            juce::Timer::callAfterDelay (250, *step);
        };
        juce::Timer::callAfterDelay (250, *step);
    }
};
sinks.undoOne = [this] (const juce::String& token, std::function<void (bool, juce::String)> cb)
{
    // ---- "UNDO ALL" IS A WALK BACK TO THE CHECKPOINT, NOT ONE POP (16 Oct 2026) -----------------
    //
    // Found by the matrix's D3: an agent add, then Undo all, and the slot was still there - the step
    // said "undone" and the rack said six slots. The reason is here: undoToCheckpoint handed the
    // CHECKPOINT token to this sink, and this sink ignored the token and popped the history once.
    // One pop is right for one step; for a whole run it undid whatever happened to be on top -
    // which could be the dial rather than the add, and then reported success about it.
    //
    // The checkpoint names a POINT, so the plugin side has to remember where that point was: the
    // history's depth when the run started. From there it is a bounded walk back, and each step is
    // judged the same way a single undo is (the stack moving is not the change being reversed).
    if (client_ != nullptr && token.isNotEmpty() && token == client_->checkpoint()
        && checkpointDepth_ >= 0)
    {
        auto& h = proc_.undoHistory();
        int steps = 0;
        juce::StringArray labels;
        while (h.undoDepth() > checkpointDepth_ && steps < 32)
        {
            const auto label = h.undoLabel();
            if (! h.undo()) break;
            if (h.lastApplyOk()) labels.add (label.isNotEmpty() ? label : juce::String ("a change"));
            ++steps;
        }
        EchoJay_NSLog (("EJAgent: undo all walked the history back to the run's checkpoint - "
                        + juce::String (steps) + " step(s), depth now " + juce::String (h.undoDepth())
                        + " (the run started at " + juce::String (checkpointDepth_) + ")").toRawUTF8());
        if (cb) cb (steps > 0 && ! labels.isEmpty(),
                    labels.isEmpty() ? juce::String ("there was nothing left to undo")
                                     : "undone: " + labels.joinIntoString (", "));
        return;
    }
    // ONE UNDO BUTTON FOR EVERYTHING. The plugin-wide history already walks local and remote
    // edits in one order (stage 5), so an agent's undo is that same step and not a second stack.
    // WHAT undo() RETURNS IS "THE STACK WAS NOT EMPTY", NOT "THE CHANGE WAS REVERSED" (14 Oct
    // 2026). An entry whose target has gone is popped, skipped with a status line and still
    // returns true, so this reported a skipped undo as a success. lastApplyOk() is the fact.
    auto& hist = proc_.undoHistory();
    const juce::String label = hist.undoLabel();
    const bool stepped = hist.undo();
    const bool applied = stepped && hist.lastApplyOk();
    if (cb) cb (applied, applied ? ("undone: " + (label.isNotEmpty() ? label : juce::String ("the last change")))
                       : stepped ? proc_.lastUndoStatus()
                                 : juce::String ("there was nothing to undo"));
};
// RULING 4 (Sean, 14 Oct 2026): the agent's {"op":"undo"}. The DEPTH is handed back either side of
// the step because that is the executor's read-back - it refuses to call an undo a success over a
// history that did not move, however confidently this sink answers.
sinks.undoHistoryStep = [this] (std::function<void (bool, juce::String, int, int)> cb)
{
    auto& hist = proc_.undoHistory();
    const int depthBefore    = hist.undoDepth();
    const juce::String label = hist.undoLabel();
    if (depthBefore <= 0) { if (cb) cb (false, "there was nothing to undo", 0, 0); return; }
    const bool stepped = hist.undo();
    const bool applied = stepped && hist.lastApplyOk();
    const int depthAfter = hist.undoDepth();
    if (cb) cb (applied,
                applied ? label
                        : (proc_.lastUndoStatus().isNotEmpty() ? proc_.lastUndoStatus()
                                                                      : "that step could not be undone"),
                depthBefore, depthAfter);
};
auto exec = std::make_unique<echojay::agent::ExecutorDo> (std::move (sources), std::move (sinks));
read_ = exec.get();   // the typed pointer syncAgentTarget() writes through
executor_ = std::move (exec);

    client_ = std::make_unique<EJAgentClient> (
        [this] { const auto t = proc_.getApi().agentTransport();
                 return EJAgentClient::Transport { t.baseUrl, t.authToken, t.extraHeaders, t.appVersion }; },
        *executor_,
        [] (const juce::String& line) { EchoJay_NSLog (line.toRawUTF8()); });
    client_->addListener (this);
    EchoJay_NSLog ("EJAgentHome: the agent's client and executor now live on the PROCESSOR - a closing "
                   "window no longer ends a run (item 0b, 15 Oct 2026)");
}

EJAgentHome::~EJAgentHome()
{
    alive_->store (false);              // every pending poll gives up on its next tick
    if (client_ != nullptr) client_->removeListener (this);
    client_.reset();                    // before the executor it holds a reference to
    executor_.reset();
}

// =============================================================================================
//  THE BUILD ROAD, AND THE ONE THING THAT STILL NEEDS A WINDOW
// =============================================================================================
void EJAgentHome::setBuildRoad (std::function<void (const juce::String&, const juce::String&)> road)
{
    buildRoad_ = std::move (road);
    if (buildRoad_ && parkedBuild_ != nullptr) servicePark();   // a window opened: run what was waiting
}

void EJAgentHome::buildChain (const juce::String& uid, const juce::var& chainArray,
                              std::function<void (bool, juce::String)> cb)
{
    // ---- ITEM 1 (Sean, 14 Oct 2026): A BUILD TAKES THE SAME ROAD THE BUILD BUTTON TAKES ------
    // The road itself is still the editor's (loadChainFromJson / sendChainToLink), handed over as
    // one function. Not re-implemented here: "two build paths that looked identical in the log is
    // what let a whole class of failure sit undiagnosed".
    if (chainArray.getArray() == nullptr || chainArray.getArray()->isEmpty())
    { if (cb) cb (false, "that build carried no chain"); return; }
    auto* o = new juce::DynamicObject();
    o->setProperty ("chain", chainArray);
    const auto chainJson = juce::JSON::toString (juce::var (o), true);
    const int n = chainArray.getArray()->size();

    if (buildRoad_)
    {
        EchoJay_NSLog (("EJAgent: build target uid=" + (uid.isEmpty() ? juce::String ("(local rack)") : uid)
                        + " slots=" + juce::String (n) + " - through the window's own build road").toRawUTF8());
        buildRoad_ (uid, chainJson);
        if (cb) cb (true, "the build was sent to " + (uid.isEmpty() ? juce::String ("this rack")
                                                                   : juce::String ("that Link"))
                          + " (" + juce::String (n) + " slot(s))");
        return;
    }

    // ---- NO WINDOW: PARKED, NOT LOST, AND NOT PRETENDED ABOUT -------------------------------
    //
    // This is the one part of a run that does not yet survive a closed window, and it says so
    // rather than failing quietly or claiming a landing. The step waits for a window; if one
    // opens it takes the user's own road, and if none does it fails with that reason in plain
    // words. Moving the build road itself to the processor is its own piece of work - it is ~750
    // lines of the most load-bearing path in the plugin, and it drives the chain panel, the tab,
    // the failed-plugin prompt and the chat's own follow-ups.
    if (parkedBuild_ != nullptr)
    {
        if (parkedBuild_->cb) parkedBuild_->cb (false, "a second build arrived while the first was still "
                                                       "waiting for this plugin's window to open");
        parkedBuild_.reset();
    }
    parkedBuild_ = std::make_unique<ParkedBuild>();
    parkedBuild_->uid = uid;
    parkedBuild_->chainJson = chainJson;
    parkedBuild_->slots = n;
    parkedBuild_->sinceMs = juce::Time::currentTimeMillis();
    parkedBuild_->cb = std::move (cb);
    EchoJay_NSLog (("EJAgent: a build of " + juce::String (n) + " slot(s) is PARKED - no window is open, "
                    "and building a rack is the one step that still needs one (item 0b)").toRawUTF8());
    // poll rather than wait on a signal: a window attaching calls setBuildRoad, which services the
    // park at once, and this is only the bound that stops it waiting for ever.
    auto alive = alive_;
    auto* self = this;
    auto step = std::make_shared<std::function<void()>>();
    *step = [alive, self, step]
    {
        if (! alive->load()) return;
        if (self->parkedBuild_ == nullptr) return;              // already serviced
        self->servicePark();
        if (self->parkedBuild_ != nullptr)
            juce::Timer::callAfterDelay (500, *step);
    };
    juce::Timer::callAfterDelay (500, *step);
}

void EJAgentHome::servicePark()
{
    if (parkedBuild_ == nullptr) return;
    if (buildRoad_)
    {
        auto p = std::move (parkedBuild_);
        EchoJay_NSLog (("EJAgent: the parked build is going now - a window opened after "
                        + juce::String ((juce::Time::currentTimeMillis() - p->sinceMs) / 1000) + " s").toRawUTF8());
        buildRoad_ (p->uid, p->chainJson);
        if (p->cb) p->cb (true, "the build was sent to " + (p->uid.isEmpty() ? juce::String ("this rack")
                                                                            : juce::String ("that Link"))
                                + " (" + juce::String (p->slots) + " slot(s)), once this plugin's window "
                                  "was opened - building a rack still needs it");
        return;
    }
    if (juce::Time::currentTimeMillis() - parkedBuild_->sinceMs < parkedBuildWaitMs) return;
    auto p = std::move (parkedBuild_);
    if (p->cb) p->cb (false, "building a rack needs this plugin's window open, and it was not opened - so "
                             "nothing was built. Everything else in this run went through.");
    EchoJay_NSLog ("EJAgent: the parked build gave up - no window within its bound, and it says so");
}

// =============================================================================================
//  THE RECORD: one author, whether a window is attached or not
// =============================================================================================
void EJAgentHome::agentChanged()
{
    // ---- WHERE THE RUN STARTED, WRITTEN DOWN ONCE (16 Oct 2026) --------------------------------
    // The checkpoint arrives from the server a moment after the run begins. The history's depth at
    // that moment is what "undo everything this run did" means on this side, and it has to be taken
    // while it is still true - the first time this run's checkpoint is seen, and not again until
    // the next run brings a new one.
    if (client_ != nullptr)
    {
        const auto cp = client_->checkpoint();
        if (cp.isNotEmpty() && cp != lastCheckpoint_)
        {
            lastCheckpoint_  = cp;
            checkpointDepth_ = proc_.undoHistory().undoDepth();
            EchoJay_NSLog (("EJAgent: run checkpoint " + cp + " - the undo history is "
                            + juce::String (checkpointDepth_) + " deep, which is where \"undo all\" "
                            "walks back to").toRawUTF8());
        }
    }
    writeSnapshot();
}

void EJAgentHome::agentTranscript (const juce::String& role, const juce::String& text)
{
    // ---- THE RUN'S PROSE, AND WHO OWNS IT -----------------------------------------------------
    //
    // ONE AUTHOR. This object writes the run's record - its snapshot AND its prose - whether a
    // window is attached or not, and the editor's row is a MIRROR of that record. The first cut had
    // both of them creating a record and minting a key, which produced two chat-history rows for one
    // run and would have shown the run twice in a reopened window. One run is one message is the
    // mockup's first rule; it has to be true of the record as well as of the view.
    //
    //   role "user"       the GOAL at start(), or the user's ANSWER to a question mid-run. The two
    //                     are told apart by isActive(), exactly as the editor's bridge does: at
    //                     start() the state is still the previous run's, in answerAsk() it is
    //                     AwaitingAsk. hasSession() cannot tell them apart.
    //   role "assistant"  this run's prose: the opening sentence while it is live (later rounds
    //                     append), the CLOSING sentence once it is over.
    const bool live = client_ != nullptr && client_->isActive();
    writeSnapshot();                 // the row exists before anything is written to it
    const bool windowAttached = (bool) buildRoad_;

    if (role == "user")
    {
        if (live) recordAnswerOnRow (text);           // mid-run: it belongs on the question's row
        else if (! windowAttached) parkLine (role, text, live);   // the goal, for the next window
        // with a window attached the editor appends the goal's own bubble, as it always has
        return;
    }

    // the prose goes on the record. The editor mirrors it; a window that is not there misses nothing.
    for (auto& e : proc_.chatHistory)
    {
        if (e.agentJson.isEmpty()) continue;
        if (echojay::agentui::fromJson (juce::JSON::parse (e.agentJson)).runKey != runKey_) continue;
        if (live)
            e.content = e.content.isEmpty() ? text : (e.content + "\n\n" + text);
        else
        {
            auto v = echojay::agentui::fromJson (juce::JSON::parse (e.agentJson));
            v.closing = text;
            e.agentJson = juce::JSON::toString (echojay::agentui::toJson (v), true);
            // ...and into content when there is no opening prose at all, so a DOWNGRADED build is
            // never left with an empty bubble where a run used to be.
            if (e.content.isEmpty()) e.content = text;
        }
        break;
    }
    if (! windowAttached) parkLine (role, text, live);
}

void EJAgentHome::parkLine (const juce::String& role, const juce::String& text, bool live)
{
    // PARKED ONLY WHEN THERE IS NO WINDOW. With one attached the editor renders the line as it
    // arrives, and parking it as well would render it twice when the next window drained the queue.
    auto* o = new juce::DynamicObject();
    o->setProperty ("role", role);
    o->setProperty ("text", text);
    o->setProperty ("sessionId", sessionId());
    o->setProperty ("runKey", runKey_);
    o->setProperty ("active", live);
    o->setProperty ("atMs", (double) juce::Time::currentTimeMillis());
    parked_.add (juce::var (o));
    // A bound, so a long unattended session cannot grow without limit. 200 lines is far more than
    // any run produces and still a fixed cost.
    while (parked_.size() > 200) parked_.remove (0);
}

void EJAgentHome::recordAnswerOnRow (const juce::String& answer)
{
    using namespace echojay::agentui;
    for (auto& e : proc_.chatHistory)
    {
        if (e.agentJson.isEmpty()) continue;
        auto v = fromJson (juce::JSON::parse (e.agentJson));
        if (v.runKey != runKey_) continue;
        // ---- THE ROW RECORDS A TYPED ANSWER TOO (16 Oct 2026) -----------------------------------
        // This looked for a step WITH CHIPS, which is most asks and not the one that mattered: an ask
        // with no choices at all (Sean's blocker) has nothing on it to match, so his typed answer
        // reached the run - the pre-gain moved - and the row never said "You said: ...". The question
        // is the step that was WAITING for an answer, chips or no chips.
        bool recorded = false;
        for (auto it = v.steps.rbegin(); it != v.steps.rend() && ! recorded; ++it)
            if (! it->chips.isEmpty() && it->answered.isEmpty())
            { it->answered = "You said: " + answer; recorded = true; }
        if (! recorded)
            for (auto it = v.steps.rbegin(); it != v.steps.rend() && ! recorded; ++it)
                if (it->answered.isEmpty()
                    && (it->right == "your answer?" || it->id == "ask"
                        || it->status == (int) EJAgentClient::Step::Status::Running))
                { it->answered = "You said: " + answer; recorded = true; }
        e.agentJson = juce::JSON::toString (toJson (v), true);
        return;
    }
}

juce::Array<juce::var> EJAgentHome::takeParkedLines()
{
    auto out = parked_;
    parked_.clearQuick();
    return out;
}

juce::String EJAgentHome::ensureRunKey()
{
    using namespace echojay::agentui;
    if (client_ == nullptr) return runKey_;
    const auto sid = client_->sessionId();
    // The same new-run test writeSnapshot makes, so the two cannot disagree about whether this is a
    // new run: a sessionId that moved means the previous row is finished with.
    if (sid.isNotEmpty() && lastSession_.isNotEmpty() && sid != lastSession_) runKey_.clear();
    if (runKey_.isEmpty())
    {
        runKey_ = juce::Uuid().toDashedString();
        RunView seed; seed.runKey = runKey_;
        EchoJayProcessor::ChatEntry ce;
        ce.role = "assistant";
        ce.agentJson = juce::JSON::toString (toJson (seed), true);
        proc_.chatHistory.push_back (std::move (ce));
        EchoJay_NSLog (("EJAgentHome: opened the run's record, key=" + runKey_
                        + " (asked for by the window's row - one record, one run)").toRawUTF8());
    }
    if (sid.isNotEmpty()) lastSession_ = sid;
    return runKey_;
}

void EJAgentHome::writeSnapshot()
{
    using namespace echojay::agentui;
    if (client_ == nullptr || ! client_->hasSession()) return;
    const auto sid = client_->sessionId();

    // Which row is this? The sessionId moving means a NEW run, and a new run gets a new row - the
    // same rule the editor's row follows, for the same reason: without it the next run would be
    // written over the last one's record and a whole run would vanish from the transcript.
    if (sid.isNotEmpty() && lastSession_.isNotEmpty() && sid != lastSession_) runKey_.clear();
    if (runKey_.isEmpty())
    {
        runKey_ = juce::Uuid().toDashedString();
        RunView seed; seed.runKey = runKey_;
        EchoJayProcessor::ChatEntry ce;
        ce.role = "assistant";
        ce.agentJson = juce::JSON::toString (toJson (seed), true);
        proc_.chatHistory.push_back (std::move (ce));
        EchoJay_NSLog (("EJAgentHome: opened the run's record, key=" + runKey_).toRawUTF8());
    }
    if (sid.isNotEmpty()) lastSession_ = sid;

    for (auto& e : proc_.chatHistory)
    {
        if (e.agentJson.isEmpty()) continue;
        const auto prev = fromJson (juce::JSON::parse (e.agentJson));
        if (prev.runKey != runKey_) continue;
        auto v = runViewFrom (*client_, {}, false);
        v.runKey  = runKey_;
        v.opening = prev.opening;
        // NEVER LOSE A SENTENCE WE WERE GIVEN: a stopped run has no doneSummary, and the answers
        // recorded on a question's row are not in the client's model at all.
        if (v.closing.isEmpty() && prev.closing.isNotEmpty()) v.closing = prev.closing;
        for (auto& ns : v.steps)
            for (const auto& os : prev.steps)
                if (os.id == ns.id && ns.answered.isEmpty() && os.answered.isNotEmpty()) ns.answered = os.answered;
        e.agentJson = juce::JSON::toString (toJson (v), true);
        return;
    }
}
