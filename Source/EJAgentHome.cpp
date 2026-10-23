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
// ITEM 2 (19 Oct 2026): the rule lives in EJAgentHome.h as echojay::agentIntentForOps, where a
// guard can reach it by name. This wrapper only adds the log line, because the reason an arm was
// WITHHELD is evidence and must not be silent. A local lambda rather than a file-static function:
// this is all inside the constructor, where a function definition is not allowed.
auto intentForOps = [] (const juce::var& ops)
{
    juce::String whyNot;
    const auto intent = echojay::agentIntentForOps (ops, &whyNot);
    if (intent.isEmpty() && whyNot.containsIgnoreCase ("slot OUT trim"))
        EchoJay_NSLog (("EJAgentHome: the level match is NOT armed for this batch - " + whyNot
                        + " (Sean's item 2, 19 Oct: B's value wins, don't fight it)").toRawUTF8());
    return intent;
};

sinks.applyChainOps = [this, intentForOps] (const juce::String& uid, const juce::var& ops,
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
        // Computed BEFORE the apply, because the callback below cannot see `ops` and the one thing
        // the editor needs from this batch is whether it put a plugin in.
        // ---- level_match:false ON THE OWN RACK TOO (B's contract of 20 Oct 2026) -------------
        // The Link reads the field off the wire; the open rack has no wire, so it is read here, from
        // the same ops. `putAPluginIn` is the only thing the editor uses to decide whether to arm the
        // post-add match, so withdrawing it is how the field is honoured on this road.
        bool lmOff = false;
        if (auto* oa = ops.getArray())
            for (const auto& ov : *oa)
                if (auto* oo = ov.getDynamicObject())
                    if (const auto lm = oo->getProperty ("level_match"); lm.isBool() && ! (bool) lm)
                    { lmOff = true; break; }
        if (lmOff)
            EchoJay_NSLog ("EJLoudness: this own-rack batch carried level_match:false - no post-add "
                           "match is armed, the slot's OUT stays at 0 dB, and the limiter keeps the "
                           "level it set (B's contract of 20 Oct)");
        const bool putAPluginIn = ! lmOff && (intentForOps (ops) == "build");
        // ---- B'S DIAL ON THE OPEN RACK (his 11:5x item 1, 21 Oct 2026) -----------------------
        //
        // *"Arm your limiter target loop whenever B's add carries dial_on_playback target_lufs, even
        // after a heard build: measure after the limiter on the next playback and correct the input
        // to the target (input only, ceiling kept). The build hit about -5.7 against -8 because
        // nothing measured the result."*
        //
        // THE FIELD WAS NEVER READ ON THIS ROAD AT ALL. dial_on_playback is handled in
        // LinkProcessor's chain-cmd apply, which is the Link's road; his mix bus is the V2's OWN
        // rack, where the agent's ops come through this in-process applier - so the target travelled
        // with the add, was parsed by nobody, and the limiter kept whatever input gain the build
        // wrote open-loop. -5.7 against -8 is exactly what "nothing measured the result" looks like.
        //
        // AND IT IS INDEPENDENT OF BOTH GATES ABOVE, deliberately:
        //   - level_match:false withdraws the slot-OUT MATCH and nothing else. B sends it on the
        //     same adds that carry a target, precisely because the LIMITER owns the level - so
        //     reading it as "don't measure anything" is how a loudness build ended up unmeasured;
        //   - postAddMatchRefusal (mix bus / master / a chain with a limiter) refuses a match that
        //     would write a slot's OUT after the limiter. A limiter target writes the limiter's
        //     INPUT, which is the one correction his ruling of 20 Oct actually asks for there.
        // Neither may gate this, and the log says so where someone would be tempted to add it.
        const auto dials      = LoudnessLoop::dialsOnPlaybackFromOps (ops);
        // ---- B'S KEEP-PLAYING CONTRACT: WHO THIS RUN MUST KEEP THE PLAYBACK FOR -------------
        // Recorded as the dials are read, before anything is applied, so the client's question
        // ("may he stop yet?") can be answered from the moment the first dial is armed. The dials'
        // own state answers the other half of it; this is only the list of who counts.
        for (const auto& d : dials)
            if (d.keepPlaying && d.name.isNotEmpty())
            {
                proc_.keepPlayingFor.addIfNotAlreadyThere (d.name);
                EchoJay_NSLog (("EJLoudness: \"" + d.name + "\" carries keep_playing - this run will not "
                                "say \"you can stop\" until its dial lands (B's contract of 22 Oct)").toRawUTF8());
            }
        const auto addedNames = LoudnessLoop::addedNamesFromOps (ops);
        proc_.getChainHost().applyChainEdits (
            std::move (parsed), /*expectedRevision*/ -1, /*baseSlots*/ {},
            [this, cb, putAPluginIn, dials, addedNames] (const juce::StringArray& results, int appliedCount, bool abortedBeforeStart)
            {
                // SEAN'S #3, the own-rack arm: applyChainEdits bumps the revision, which the panel's
                // own signature watches - but the agent can change a rack this window is NOT viewing
                // (it has a Link selected while working on the open rack), and then nothing rebuilt.
                if (! abortedBeforeStart && appliedCount > 0 && proc_.onAgentChangedRack)
                    proc_.onAgentChangedRack ({}, putAPluginIn);
                // ...AND B'S DIAL, AFTER that hook on purpose: the hook may have armed the inferred
                // post-add match, and B's target is EXPLICIT. One loop, so the later arm replaces the
                // earlier one, and the precedence is the right way round - we can only infer that a
                // build is a loudness build; B says it.
                // ---- ONE PER SLOT (his 17:3x item 3) --------------------------------------
                // Every dial in the batch, each resolved to ITS OWN slot by the name that op added,
                // so three compressors in one build are three independent dials and not one.
                if (! abortedBeforeStart && appliedCount > 0 && ! dials.empty())
                    armDialsOnPlaybackOnOwnRack (dials, addedNames);
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
    const auto intent = intentForOps (ops);
    if (intent.isNotEmpty())
        EchoJay_NSLog (("EJAgentHome: this change puts a plugin in, so it goes out as intent=\"" + intent
                        + "\" - the Link arms the same silent slot-OUT level match the Build button gets "
                          "(Sean's item 2, 19 Oct)").toRawUTF8());
    const int seq = proc_.writeChainEditCommand (uid, ops, juce::var(), "agent", {}, intent);
    if (seq < 0) { if (cb) cb (false, "that rack would not take the change", {}); return; }
    proc_.awaitChainAck (uid, seq, [this, uid, cb] (bool ok, juce::String sentence, juce::String technical, int)
    {
        // ---- SEAN'S #3 (16 Oct 2026): TELL THE WINDOW THE RACK MOVED ---------------------------
        // The 14 Oct refresh sits on the BUILD path; an `add` takes this one, so nothing told the
        // view - "stays empty / Reading 0 plugin(s) until I switch racks". Raised only on success,
        // and only here, where the Link has actually acknowledged the change.
        if (ok && proc_.onAgentChangedRack) proc_.onAgentChangedRack (uid, false);
        if (cb) cb (ok, sentence, technical);
    });
};
// ---- AND THE BUILD ROAD KEEPS A HANDLE ON THAT SAME FUNCTION (his 22 Oct ruling) -----------
// Not a copy of its body: the function itself, so "apply these ops to that rack" has ONE
// implementation whether the ops came from an add or from a build with no window open.
applyOps_ = sinks.applyChainOps;

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
// HIS "DON'T USE" MARKS, read live for the same reason (his 11:5x item 3). The name and the reason
// he gave; the inventory withholds them either way, and this is what lets the model answer when he
// asks for one by name instead of leaving a silent hole where a plugin used to be.
sinks.dontUsePlugins = [this] () -> juce::var
{
    juce::Array<juce::var> out;
    for (const auto& n : proc_.getApi().getDontUsePlugins())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", n);
        if (const auto why = proc_.getApi().dontUseReasonFor (n); why.isNotEmpty())
            o->setProperty ("why", why);
        out.add (juce::var (o));
    }
    return out.isEmpty() ? juce::var() : juce::var (out);
};
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
    // NOTHING WAITS ON THIS ANY MORE (his 22 Oct ruling). It used to service a parked build the
    // moment a window attached, which was the only way a windowless build ever landed; a build
    // now applies when it is made, so a window arriving afterwards has nothing to collect but the
    // card - which the processor has been holding all along.
    buildRoad_ = std::move (road);
}

void EJAgentHome::buildChain (const juce::String& uid, const juce::var& chainArray,
                              std::function<void (bool, juce::String)> cb)
{
    // ---- ITEM 1 (Sean, 14 Oct 2026): A BUILD TAKES THE SAME ROAD THE BUILD BUTTON TAKES ------
    // With a window, that road is the editor's own (loadChainFromJson / sendChainToLink), handed
    // over as one function. With none, it is the road every agent `add` takes (his 22 Oct ruling,
    // below). Neither is re-implemented here: "two build paths that looked identical in the log is
    // what let a whole class of failure sit undiagnosed".
    if (chainArray.getArray() == nullptr || chainArray.getArray()->isEmpty())
    { if (cb) cb (false, "that build carried no chain"); return; }
    auto* o = new juce::DynamicObject();
    o->setProperty ("chain", chainArray);
    // ---- THE BUILD SAYS WHOSE IT IS (his 15:30 ruling, 21 Oct 2026) -------------------------
    //
    // *"...and make it impossible after any agent build."*
    //
    // IN THE BUILD ITSELF, not in a flag beside it. The road is shared with the Build button on
    // purpose ("two build paths that looked identical in the log is what let a whole class of
    // failure sit undiagnosed"), so the thing that differs has to TRAVEL with the build, and it
    // cannot be left set from a previous build, which a member flag could. Read by
    // loadChainFromJson (no auto input staging) and by finishChainBubbleWhenDialSettled (no
    // limiter inserted) - BOTH of them the window's, which is why the windowless road below needs
    // no marker to honour the same ruling: neither of its readers exists there.
    o->setProperty ("from", "agent");
    const auto chainJson = juce::JSON::toString (juce::var (o), true);
    const int n = chainArray.getArray()->size();

    if (buildRoad_)
    {
        EchoJay_NSLog (("EJAgent: build target uid=" + (uid.isEmpty() ? juce::String ("(local rack)") : uid)
                        + " slots=" + juce::String (n) + " - through the window's own build road").toRawUTF8());
        lastBuildRoad_   = "window";
        lastBuildRunKey_ = runKey_;
        buildRoad_ (uid, chainJson);
        if (cb) cb (true, "the build was sent to " + (uid.isEmpty() ? juce::String ("this rack")
                                                                   : juce::String ("that Link"))
                          + " (" + juce::String (n) + " slot(s))");
        return;
    }

    // ---- NO WINDOW: IT APPLIES, BY THE ROAD THE AGENT'S OWN ADDS TAKE -----------------------
    //
    // Sean, 22 Oct 2026: *"If the user has already pressed Go, a run with no window open finishes
    // its build and applies it. The card, its chips and Undo all are there when a window reopens."*
    // The reasoning, and what this road deliberately does NOT do, is in EJAgentHome.h.
    //
    // ONE THING TO SAY OUT LOUD ABOUT HIS 15:30 RULING: an agent build must not stage the input
    // trim and must insert no limiter. On this road neither can happen BY CONSTRUCTION rather than
    // by a flag being read correctly - computePreGainAtBuild belongs to loadChainFromJson and the
    // limiter to finishChainBubbleWhenDialSettled, and both are the window's. The rig's N2 asserts
    // it on the road a user actually has.
    if (! applyOps_)
    {
        // Cannot happen with a constructed home - the sink and this road are assigned together in
        // the constructor - and it is still said rather than crashed through, because the one
        // thing this round is about is a build that reports what it did.
        if (cb) cb (false, "that build could not be applied: this plugin has no road to its own rack");
        return;
    }
    int nOld = 0;
    if (uid.isEmpty())
        nOld = proc_.getChainHost().getNumSlots();
    else if (auto it = proc_.linkRackCache.find (uid); it != proc_.linkRackCache.end())
        if (it->second.valid) nOld = (int) it->second.rack.slots.size();
    // ---- THE OLD RACK IS ARCHIVED FIRST, AS THE WINDOW'S ROAD DOES ---------------------------
    // loadChainFromJson archives the live rack before it clears it, "so a build over a populated
    // rack is recoverable from chain_archive/" - protection that sits BESIDE undo, not instead of
    // it. A build with no window has the same user and the same risk, so it gets the same belt;
    // without this line the windowless road would be the one road that could not be recovered
    // from, and nobody would find that out until they needed it. No-ops on an empty rack.
    if (uid.isEmpty() && nOld > 0)
        proc_.getChainHost().archiveCurrentRack ("replaced by an agent build with no window open");
    const auto wire = ChainHost::chainArrayToBuildWireOps (*chainArray.getArray(), nOld);
    lastBuildRoad_   = "processor";
    lastBuildRunKey_ = runKey_;
    EchoJay_NSLog (("EJAgent: build target uid=" + (uid.isEmpty() ? juce::String ("(local rack)") : uid)
                    + " slots=" + juce::String (n) + " - NO WINDOW IS OPEN, so it goes down the road the "
                      "agent's own adds take: " + juce::String (nOld) + " removed, " + juce::String (n)
                    + " added (his 22 Oct ruling)").toRawUTF8());
    applyOps_ (uid, wire, [cb, n, uid] (bool ok, juce::String sentence, juce::String technical)
    {
        juce::ignoreUnused (technical);
        if (! cb) return;
        if (! ok)
        {
            cb (false, sentence.isNotEmpty() ? sentence
                                             : juce::String ("that rack would not take the build"));
            return;
        }
        cb (true, "the build was applied to " + (uid.isEmpty() ? juce::String ("this rack")
                                                               : juce::String ("that Link"))
                  + " (" + juce::String (n) + " slot(s)) with no window open - its card, its chips "
                    "and Undo are in the chat when you open one");
    });
}

// =============================================================================================
//  THE RECORD: one author, whether a window is attached or not
// =============================================================================================
/** ---- WHO NEEDS THAT RACK (his 08:44 fault, 23 Oct 2026) ------------------------------------
    *"opening a Link's window to look must not stop the V2's run or take its rack away. If the Link
     does take control while its window is open, closing that window hands control straight back to
     the V2, the strip reconnects, and Undo all works again."*

    The 20 Oct ruling made the borrow DEMAND-DRIVEN - "viewing a rack must not engage it; engage
    only when something actually needs the hosted copy" - which is right, and it left the agent's own
    run without a voice: his vocal run was dialling on that rack when the Link took it back, and
    nothing afterwards asked for it again ("EJBorrow: not engaging 4814a16004 - nothing has asked to
    engage this rack yet", 08:45:36). So the run says so here, and the tick asks.

    A FINISHED run still counts while its Undo all is live: that button walks the rack back to the
    run's checkpoint, and "Undo all showed but could not be pressed" is what it looks like when the
    rack it would walk back is not held. */
juce::String EJAgentHome::runTargetUid() const
{
    return executor_ != nullptr ? executor_->targetUid() : juce::String();
}

bool EJAgentHome::runNeedsItsRack() const
{
    if (client_ == nullptr) return false;
    return client_->isActive() || client_->canUndoAll();
}

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

// ---- B'S dial_on_playback, ON THE RACK THIS WINDOW HAS OPEN (his 11:5x item 1, 21 Oct 2026) ----
//
// *"Arm your limiter target loop whenever B's add carries dial_on_playback target_lufs, even after a
// heard build: measure after the limiter on the next playback and correct the input to the target
// (input only, ceiling kept). The build hit about -5.7 against -8 because nothing measured the
// result."*
//
// His 11:5x build was on the MIX BUS - the V2's own rack - and every line of dial_on_playback
// handling in this build lived on the Link's road (LinkProcessor's chain-cmd apply). So the field
// arrived, was read by nobody, and the limiter kept the input gain the build had written open-loop.
// Nothing measured it, which is the whole of why it landed 2.3 dB short.
//
// ONE SHAPE, TWO JOBS, told apart by the key B sends - the same split the Link's road makes:
//   target_lufs   a LIMITER's loudness target -> the loudness loop, aimed at that limiter's INPUT
//   target_gr_db  a COMPRESSOR's GR target    -> a calibration block, the road agent-off takes
// Nothing else about the two is alike: one drives level into a ceiling, the other hunts a GR band.
// ---- EVERY DIAL IN THE BATCH, EACH ON ITS OWN SLOT (his 17:3x item 3, 21 Oct 2026) ------------
//
// *"B will send dial_on_playback on every compressor after the first in a build, so you set it from
// its slot's real input. Confirm that works for several slots in one build (not just the last), each
// dialled independently."*
//
// IT DID NOT WORK, for two reasons, and only one of them was the reading:
//   1. every reader stopped at the FIRST op carrying a dial (fixed in dialsOnPlaybackFromOps);
//   2. calibStart stores ONE loop per rack uid, so arming the second dial would have replaced the
//      first - the last compressor dialled and the rest dropped without a word. calibStartMany is
//      the road built for "ONE HOLD PER COMPRESSOR" (30 Sep) and it has to be given all of them AT
//      ONCE, which is why this takes the whole batch rather than being called in a loop.
//
// Each dial's slot is resolved from ITS OWN op's name, by identity, so the dials do not depend on
// their order in the batch and a build whose plugins land out of order still dials the right ones.
void EJAgentHome::armDialsOnPlaybackOnOwnRack (const std::vector<LoudnessLoop::DialOnOp>& dials,
                                               const juce::StringArray& addedNames)
{
    if (dials.empty()) return;
    auto& host = proc_.getChainHost();
    std::vector<echojay::CalibLoop::Config> cfgs;
    int limiterTargets = 0;

    for (const auto& d : dials)
    {
        const auto names = d.name.isNotEmpty() ? juce::StringArray { d.name } : addedNames;
        // A LIMITER TARGET takes the loudness road and there is at most one: a rack has one place
        // where loudness is decided. A second is reported rather than silently preferred.
        if (LoudnessLoop::dialIsLimiterTarget (d.dial))
        {
            if (++limiterTargets > 1)
            {
                EchoJay_NSLog (("EJLoudness: this batch carries a SECOND limiter target (\"" + d.name
                                + "\") - ignored. One rack has one loudness decision; the first one's "
                                  "target stands and this is said rather than silently preferred.").toRawUTF8());
                continue;
            }
            armDialOnPlaybackOnOwnRack (d.dial, names);
            continue;
        }
        // A COMPRESSOR'S GR TARGET: its own slot, its own block, its own config.
        const int at = LoudnessLoop::slotTheBatchAdded (host, names);
        if (at < 0)
        {
            EchoJay_NSLog (("EJThreshold: a dial_on_playback names \"" + d.name + "\" but no slot on "
                            "this rack answers to it - NOT dialled, and said so (his 17:09 lesson: a "
                            "slot is an identity, not a position)").toRawUTF8());
            continue;
        }
        juce::String why;
        const auto block = echojay::CalibLoop::blockFromDialOnPlayback (d.dial, at, why);
        if (block.getDynamicObject() == nullptr)
        {
            EchoJay_NSLog (("EJThreshold: slot " + juce::String (at + 1) + " (\""
                            + host.getSlotInfo (at).name + "\") carried dial_on_playback but it could "
                            "not be read as a calibration block - " + why + ". Nothing is dialled, and "
                            "nothing claims to have been.").toRawUTF8());
            continue;
        }
        echojay::CalibLoop::Config cfg;
        juce::String whyCfg;
        if (! echojay::CalibLoop::configFromBlock (block, host.getNumSlots(), /*bus*/ false,
                                                   host.getSlotInfo (at).name, cfg, whyCfg))
        {
            EchoJay_NSLog (("EJThreshold: slot " + juce::String (at + 1) + "'s block is not usable - "
                            + whyCfg.trim()).toRawUTF8());
            continue;
        }
        if (whyCfg.isNotEmpty())
            EchoJay_NSLog (("EJThreshold: BLOCK NOT AS CONTRACTED (slot " + juce::String (at + 1)
                            + ") - " + whyCfg.trim()).toRawUTF8());
        cfg.slot    = at;                    // ITS OWN slot, whatever the block said
        cfg.plugin  = host.getSlotInfo (at).name;
        cfg.purpose = echojay::CalibLoop::Purpose::buildHold;   // one window, one write, no promise
        cfgs.push_back (cfg);
    }

    if (cfgs.empty()) return;
    juce::StringArray named;
    for (const auto& c : cfgs) named.add (c.plugin + " (slot " + juce::String (c.slot + 1) + ")");
    EchoJay_NSLog (("EJThreshold: the OPEN RACK starts " + juce::String ((int) cfgs.size())
                    + " dial_on_playback hold(s) from one build - " + named.joinIntoString (", ")
                    + " - each on its own slot's real input, through calibStartMany so one does not "
                      "replace another (his 17:3x item 3)").toRawUTF8());
    const int started = proc_.calibStartMany ({}, cfgs);
    if (started != (int) cfgs.size())
        EchoJay_NSLog (("EJThreshold: " + juce::String (started) + " of " + juce::String ((int) cfgs.size())
                        + " hold(s) actually started - the rest were refused by calibStart's own "
                          "rules (a bus target, a switch as the actuator); their lines are above "
                          "this one").toRawUTF8());
}

void EJAgentHome::armDialOnPlaybackOnOwnRack (const juce::var& dial, const juce::StringArray& addedNames)
{
    auto* dp = dial.getDynamicObject();
    if (dp == nullptr) return;
    auto& host = proc_.getChainHost();

    // WHICH SLOT, BY IDENTITY. The shared rule, so this and the Link's nameTheMatchTarget cannot
    // drift apart; -1 means no slot on the rack answers to anything this batch added, and then
    // nothing is armed and the log says why rather than dialling a plugin nobody named.
    const int added = LoudnessLoop::slotTheBatchAdded (host, addedNames);
    if (added < 0)
    {
        EchoJay_NSLog (("EJLoudness: this own-rack batch carried dial_on_playback but no slot matches "
                        "what it added (" + addedNames.joinIntoString (", ")
                        + ") - NOT arming anything. A target needs the slot it belongs to, and the "
                          "last slot is an index standing in for an identity (his 17:09 lesson).").toRawUTF8());
        return;
    }
    const auto slotName = host.getSlotInfo (added).name;

    // ---- (a) A LIMITER'S LOUDNESS TARGET ----------------------------------------------------
    float targetLufs = 0.0f;
    if (LoudnessLoop::dialIsLimiterTarget (dial, &targetLufs))
    {
        const auto ctl   = dp->getProperty ("control").toString().trim();
        const auto ceilV = dp->getProperty ("ceiling");
        const float ceil = (ceilV.isDouble() || ceilV.isInt() || ceilV.isInt64())
                             ? (float) (double) ceilV : -0.1f;
        // B'S LIMITING BUDGET, read the same way on this road as on the Link's (his 21 Oct ruling):
        // absent is "that half is not bounded", never a number nobody sent.
        const auto numOr = [dp] (const char* key)
        {
            const auto v = dp->getProperty (key);
            return (v.isDouble() || v.isInt() || v.isInt64())
                     ? (float) (double) v : std::numeric_limits<float>::quiet_NaN();
        };
        const float avgLim  = numOr ("avg_limit_db");
        const float peakLim = numOr ("peak_limit_db");
        // THE ARM RESETS THE CHAIN TALLIES (LoudnessLoop::arm), which is what makes "even after a
        // heard build" mean something: the measurement is of the chain AS IT NOW STANDS, not of an
        // integrated figure that predates the limiter. His -5.7 was never measured at all; this
        // measures after the limiter on the next playback and corrects its INPUT.
        if (proc_.loudnessLoop().armTargetFromDial (targetLufs, added, ctl, ceil, avgLim, peakLim))
            EchoJay_NSLog (("EJLoudness: the OPEN RACK armed B's limiter target - "
                            + juce::String ((double) targetLufs, 1) + " LUFS on slot "
                            + juce::String (added + 1) + " (\"" + slotName + "\"), control \""
                            + (ctl.isNotEmpty() ? ctl : juce::String ("(none named)"))
                            + "\", ceiling " + juce::String ((double) ceil, 2) + " dBTP KEPT. It drives "
                              "that slot's INPUT and nothing after it (his correction of 20 Oct), "
                              "silently, on the next playback. NOT gated by level_match:false or by "
                              "postAddMatchRefusal: both govern the slot-OUT match, and this is the "
                              "limiter's input - the one correction his 20 Oct ruling asks for on a "
                              "mix bus (his 11:5x item 1).").toRawUTF8());
        else
            EchoJay_NSLog ("EJLoudness: the open rack could NOT arm B's limiter target - see the arm "
                           "line above for the reason. The limiter keeps the input gain the build "
                           "wrote, which is what a build that does not know the field does.");
        return;   // one road each: a limiter target is not a compressor dial
    }

    // ---- (b) A COMPRESSOR'S GR TARGET -------------------------------------------------------
    // Translated into a calibration block rather than given a mechanism of its own, for the reason
    // B's own contract gives: *"Agent off already sends a target block (gr_target_db with
    // calibration), which the plugin dials during playback. No change is needed there."*
    juce::String why;
    const auto block = echojay::CalibLoop::blockFromDialOnPlayback (dial, added, why);
    if (block.getDynamicObject() == nullptr)
    {
        EchoJay_NSLog (("EJThreshold: this own-rack add carried dial_on_playback but it could not be "
                        "read as a calibration block - " + why + ". Nothing is dialled, and nothing "
                        "claims to have been.").toRawUTF8());
        return;
    }
    echojay::CalibLoop::Config cfg;
    juce::String whyCfg;
    if (! echojay::CalibLoop::configFromBlock (block, host.getNumSlots(), /*bus*/ false, slotName, cfg, whyCfg))
    {
        EchoJay_NSLog (("EJThreshold: the own-rack dial_on_playback block is not usable - "
                        + whyCfg.trim()).toRawUTF8());
        return;
    }
    if (whyCfg.isNotEmpty())
        EchoJay_NSLog (("EJThreshold: BLOCK NOT AS CONTRACTED - " + whyCfg.trim()).toRawUTF8());
    // A BUILD HOLDS ONCE AND CLOSES: one window, one write, no promise opened and no rung hunted -
    // the same purpose the Link stamps on a block that arrives with a build, and the thing that
    // keeps this silent (Purpose::buildHold leaves askOwed empty).
    cfg.purpose = echojay::CalibLoop::Purpose::buildHold;
    EchoJay_NSLog (("EJThreshold: the OPEN RACK starts B's dial_on_playback hold - \"" + slotName
                    + "\" slot " + juce::String (added + 1) + ", band " + juce::String (cfg.lo, 1)
                    + "-" + juce::String (cfg.hi, 1) + " dB, dialling "
                    + (cfg.params.isEmpty() ? juce::String ("the drive") : cfg.params.joinIntoString (" + "))
                    + " (his 11:5x item 1: this field was read on the Link's road only)").toRawUTF8());
    proc_.calibStart ({}, cfg);
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

// ---- IS THIS A NEW RUN? (18 Oct 2026, Sean in Logic) -------------------------------------------
//
// *"The closing shows twice (a bubble above the ASK label and again inside the card)... On a second
// ask, 'You said: cl1b' appears under the NEW pending ask."*  His log, 12:05:
//
//     12:05:07  EJAgent: START goal="Add a compressor"
//     12:05:07  EJAgentRow: opened run row #2 key=b2ae1f1b-...      <- run #1's key
//     12:05:08  EJAgentHome: opened the run's record, key=136dc0a6-...
//
// Run #2's row asked for a key one second before the server answered /start. BOTH places that mint
// one tested "is this a new run" by the sessionId MOVING - and at that moment the sessionId is empty,
// because start() clears it and the first frame had not arrived. So the test could not fire, the new
// row was handed run #1's key, and syncAgentRunMessage then correctly adopted the record under that
// key: run #1's closing became run #2's bubble, and run #1's recorded answers came with it.
//
// The sessionId is the wrong fact: it arrives AFTER the window in which both the row and the record
// are created. runSerial() is +1 per start() and nothing else, so it is already different by the time
// anything asks. One test, one function, called by both minting sites - they cannot disagree again.
bool EJAgentHome::newRunSinceLastKey()
{
    if (client_ == nullptr) return false;
    const int serial = client_->runSerial();
    if (serial == lastSerial_) return false;
    lastSerial_ = serial;
    // ---- A NEW RUN KEEPS NOBODY'S PLAYBACK YET (B's keep-playing contract, 22 Oct) ----------
    // Cleared HERE because this is the one predicate both minting roads consult (ensureRunKey and
    // writeSnapshot), and it is true exactly once per run - so a dial from the last run cannot hold
    // this one's listen open, and neither road has to remember to do it.
    proc_.keepPlayingFor.clear();
    proc_.agentDialResults.clear();   // ...and what the last run's dials said (his 08:43 ruling)
    return true;
}

juce::String EJAgentHome::ensureRunKey()
{
    using namespace echojay::agentui;
    if (client_ == nullptr) return runKey_;
    const auto sid = client_->sessionId();
    if (newRunSinceLastKey()) runKey_.clear();
    if (runKey_.isEmpty())
    {
        runKey_ = juce::Uuid().toDashedString();
        RunView seed; seed.runKey = runKey_;
        EchoJayProcessor::ChatEntry ce;
        ce.role = "assistant";
        ce.agentJson = juce::JSON::toString (toJson (seed), true);
        // stage B: through the one door, with the RUN'S KEY as the entry's identity and the run's
        // own chat as its home - never "the chat on screen", which is what his 19:51 card took.
        runChatId_  = client_ != nullptr ? client_->runChatId() : juce::String();
        runEntryId_ = runKey_;          // the row's identity IS the run's key (stage B)
        proc_.appendChatEntry (std::move (ce), runChatId_, runKey_);
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

    // Which row is this? A NEW run gets a new row - without it the next run would be written over the
    // last one's record and a whole run would vanish from the transcript. By the run SERIAL, not by
    // the sessionId: see newRunSinceLastKey above for the hour that rule cost.
    if (newRunSinceLastKey()) runKey_.clear();
    if (runKey_.isEmpty())
    {
        runKey_ = juce::Uuid().toDashedString();
        RunView seed; seed.runKey = runKey_;
        EchoJayProcessor::ChatEntry ce;
        ce.role = "assistant";
        ce.agentJson = juce::JSON::toString (toJson (seed), true);
        // stage B: through the one door, with the RUN'S KEY as the entry's identity and the run's
        // own chat as its home - never "the chat on screen", which is what his 19:51 card took.
        runChatId_  = client_ != nullptr ? client_->runChatId() : juce::String();
        runEntryId_ = runKey_;          // the row's identity IS the run's key (stage B)
        proc_.appendChatEntry (std::move (ce), runChatId_, runKey_);
        EchoJay_NSLog (("EJAgentHome: opened the run's record, key=" + runKey_).toRawUTF8());
    }
    if (sid.isNotEmpty()) lastSession_ = sid;

    // ---- THE ROW IS FOUND BY THE RUN, NOT BY A LIST THE WINDOW CAN EMPTY -------------------
    //
    // *"Its card is stuck on 'Thinking...' with 'Hide steps' and NO steps showing"* (his test 2,
    // 08:28 on 23 Oct, after switching Links several times while the run was live).
    //
    // THE CAUSE, and it is one line: this walked proc_.chatHistory for a row whose key matched, and
    // a chat or Link switch CLEARS that list - loadChatFromWorkspace, "a switch never carries one
    // chat's messages into another" (his rule 2 of 18 Oct, which is right). From his first switch
    // onwards the loop fell through without writing, so the run's card stopped being recorded
    // anywhere while the run itself carried on perfectly - the log has the Kick run reaching its
    // question at 08:29:57 with nothing on screen saying so. The comment this replaces even said
    // the point was to render "from the STORE rather than from a list a chat switch can drop", and
    // then gated the write on that very list.
    //
    // So the previous snapshot is taken from the row if it is still on the list, ELSE from the
    // STORE, which is the authority either way; the list is updated when it is there, and the store
    // is written always. The address comes from the home's own runChatId_/runEntryId_, taken when
    // the row was minted, so no list and no window decides whether a run is recorded.
    const auto chatId  = runChatId_.isNotEmpty() ? runChatId_
                                                 : (client_ != nullptr ? client_->runChatId() : juce::String());
    const auto entryId = runEntryId_.isNotEmpty() ? runEntryId_ : runKey_;
    EchoJayProcessor::ChatEntry* row = nullptr;
    for (auto& e : proc_.chatHistory)
    {
        if (e.agentJson.isEmpty()) continue;
        if (fromJson (juce::JSON::parse (e.agentJson)).runKey != runKey_) continue;
        row = &e;
        break;
    }
    const auto prevJson = row != nullptr ? row->agentJson
                                         : proc_.storedAgentCardJson (chatId, entryId);
    if (row == nullptr && prevJson.isEmpty() && chatId.isEmpty())
        return;                       // no row anywhere and nowhere to put one: nothing to write
    const auto prev = fromJson (juce::JSON::parse (prevJson));
    auto v = runViewFrom (*client_, {}, false);
    v.runKey  = runKey_;
    v.opening = prev.opening;
    // NEVER LOSE A SENTENCE WE WERE GIVEN: a stopped run has no doneSummary, and the answers
    // recorded on a question's row are not in the client's model at all.
    if (v.closing.isEmpty() && prev.closing.isNotEmpty()) v.closing = prev.closing;
    carryAnswersForward (v, prev);   // by position and id, never the id alone (18 Oct 2026)
    const auto json = juce::JSON::toString (toJson (v), true);
    if (row != nullptr) row->agentJson = json;
    proc_.storeAgentCard (row != nullptr && row->chatId.isNotEmpty() ? row->chatId : chatId,
                          row != nullptr && row->entryId.isNotEmpty() ? row->entryId : entryId,
                          json, v.closing.trim());
}
