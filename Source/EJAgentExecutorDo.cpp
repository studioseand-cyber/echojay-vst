#include "EJAgentExecutorDo.h"
#include "ChainHost.h"   // check 5 part 2: the plugins that did not load here
#include "EJParamPlainNames.h"   // Sean, 15 Oct: a line a person reads names parameters in words

namespace echojay::agent
{

namespace {
double argNum (const ToolCall& c, const char* key, bool& present)
{
    present = false;
    if (auto* o = c.args.getDynamicObject())
        if (o->hasProperty (key))
        {
            const auto v = o->getProperty (key);
            if (v.isDouble() || v.isInt() || v.isInt64()) { present = true; return (double) v; }
        }
    return 0.0;
}
bool argBool (const ToolCall& c, const char* key, bool& present)
{
    present = false;
    if (auto* o = c.args.getDynamicObject())
        if (o->hasProperty (key)) { present = true; return (bool) o->getProperty (key); }
    return false;
}
int argInt (const ToolCall& c, const char* key, bool& present)
{
    bool p = false; const double d = argNum (c, key, p); present = p; return (int) d;
}
juce::var argVar (const ToolCall& c, const char* key)
{
    if (auto* o = c.args.getDynamicObject()) return o->getProperty (key);
    return {};
}
} // namespace

ExecutorDo::ExecutorDo (Sources s, DoSinks sinks)
    : ExecutorRead (std::move (s)), sinks_ (std::move (sinks)) {}

juce::var ExecutorDo::startContext()
{
    auto base = ExecutorRead::startContext();
    if (auto* o = base.getDynamicObject())
    {
        // TWO FACTS AN AGENT MUST KNOW BEFORE IT PLANS, and both are read LIVE rather than captured once:
        //
        //   can_act      - agent mode. Sean's standing rule is that this stays a Settings switch until he
        //                  signs off, and OFF means today's behaviour: chat plus Apply. An agent told it can
        //                  act when it cannot will plan a round it cannot execute and then report failures
        //                  that are not failures.
        //   echojay_only - the AI may only pick EchoJay's own built-in devices. SEPARATE from Dial only, and
        //                  both can be on (Sean's 10 Oct ruling): Dial only is about whether a plugin's
        //                  values can be dialled, this is about WHICH PLUGINS ARE OFFERED AT ALL. A model
        //                  that does not know this proposes third-party plugins and every one is refused.
        o->setProperty ("can_act", sinks_.agentModeOn && sinks_.agentModeOn());
        o->setProperty ("echojay_only", sinks_.echoJayOnly && sinks_.echoJayOnly());

        // ---- SEAN'S CHECK 5, PART 2 (15 Oct 2026): WHAT WILL NOT LOAD HERE -------------------------
        //
        // The inventory already withholds these, so they cannot be picked. They are NAMED here as well
        // because the model needs to be able to answer "why not kHs Transient Shaper?" without guessing,
        // and because a user who asks for one by name must be told what happened rather than watching it
        // fail a third time. A named plugin is still used as named; this is an account, not a ban.
        if (const auto wontLoad = ChainHost::loadFailedPluginNames(); ! wontLoad.isEmpty())
        {
            juce::Array<juce::var> arr;
            for (const auto& n : wontLoad)
            {
                auto* e = new juce::DynamicObject();
                e->setProperty ("name", n);
                const auto why = ChainHost::loadFailureReasonFor (n);
                if (why.isNotEmpty()) e->setProperty ("why", why);
                arr.add (juce::var (e));
            }
            o->setProperty ("plugins_that_did_not_load", juce::var (arr));
            o->setProperty ("plugins_that_did_not_load_note",
                            "These are installed but did not instantiate on this machine (licence, or they "
                            "refuse to load). They are withheld from the inventory so you cannot offer them "
                            "in a pick. If the user names one, say what happened to it rather than trying "
                            "again. The mark clears by itself the moment the plugin loads.");
        }

        // THE UNDO CAPABILITY, DECLARED (14 Oct 2026). B gates the op on this and nothing else:
        // "the server sends it only when the start body's capabilities includes \"undo\"; otherwise it is
        // refused in process (undo_unavailable)". The op was built this morning and the guard proved it, and
        // the rig's row C10 still failed - the model never called it, because it was never told it existed.
        // Writing the op without declaring it is half a feature that looks like the model misbehaving.
        //
        // CONDITIONAL ON THE SINK, not hardcoded like "proposal" and "leveling" in the chat body. A
        // capability says what THIS BINARY CAN DO; a build whose undo route is not wired must not claim one,
        // or the server sends an op that comes straight back as not_in_phase and the model is made to look
        // like it invented the call.
        if (sinks_.undoHistoryStep)
            if (auto* caps = o->getProperty ("capabilities").getArray())
                if (! caps->contains (juce::var ("undo")))
                    caps->add ("undo");
    }
    return base;
}

juce::var ExecutorDo::translate (const ToolCall& call, juce::String& refusal) const
{
    const auto op = argString (call, "op").trim().toLowerCase();
    juce::Array<juce::var> edits;

    auto addOp = [&edits] (const juce::String& name) -> juce::DynamicObject*
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("op", name);
        edits.add (juce::var (o));
        return o;
    };
    // THE WIRE IS 1-BASED and the plugin's structs are 0-based. B's tool layer speaks the 1-based numbers a
    // model and a user see, and the chain channel's parser subtracts one - so a slot read here is passed
    // through UNCHANGED. Stated because getting it wrong twice in one day cost two cycles in remote_ops_guard.
    bool haveSlot = false;
    const int slot1 = argInt (call, "slot", haveSlot);
    auto needSlot = [&] (juce::DynamicObject* o) -> bool
    {
        if (! haveSlot) { refusal = "that needs a slot number and none was given"; return false; }
        o->setProperty ("slot", slot1);
        return true;
    };

    if (op == "set")
    {
        auto* o = addOp ("set");
        if (! needSlot (o)) return {};
        const auto ss = argVar (call, "settings_structured");
        if (ss.getDynamicObject() == nullptr)
        {
            refusal = "a set needs settings_structured with the values in the device's own units";
            return {};
        }
        o->setProperty ("settings_structured", ss);
    }
    else if (op == "set_wet")
    {
        auto* o = addOp ("set_wet");
        if (! needSlot (o)) return {};
        // ---- THE SWEEP CAUGHT THIS (14 Oct 2026): B SENDS wet_pct, THIS READ pct --------------------
        //
        // docs/CONTRACT_AGENT_TOOLS.md line 103: `set_wet` | `{slot: n, wet_pct: 0-100}`. This read "pct"
        // and refused everything else, so EVERY agent set_wet has failed with "a set_wet needs a pct from
        // 0 to 100" while the call in front of it carried exactly that figure under the contract's own
        // name. Nobody had tried it in a host yet, so nothing said so.
        //
        // The contract's name is read FIRST and "pct" is still accepted, because a refusal over a synonym
        // is the failure this is fixing and an extra accepted spelling cannot cause one.
        bool p = false;
        double pct = argNum (call, "wet_pct", p);
        if (! p) pct = argNum (call, "pct", p);
        if (! p) { refusal = "a set_wet needs wet_pct from 0 to 100"; return {}; }
        o->setProperty ("wet_pct", pct);
    }
    else if (op == "set_io")
    {
        // ONE `do`, TWO FRAMES, ONE ACK. The contract allows an op to carry both inDb and outDb, and a user
        // asking for "in up 3, out down 3" asked for ONE thing - so it is one command and one undo step.
        bool pin = false, pout = false;
        const double inDb  = argNum (call, "inDb",  pin);
        const double outDb = argNum (call, "outDb", pout);
        if (! pin && ! pout) { refusal = "a set_io needs inDb, outDb, or both"; return {}; }
        if (pin)  { auto* o = addOp ("slot_in");  if (! needSlot (o)) return {}; o->setProperty ("db", inDb); }
        if (pout) { auto* o = addOp ("slot_out"); if (! needSlot (o)) return {}; o->setProperty ("db", outDb); }
    }
    else if (op == "set_master_wet")
    {
        bool p = false; const double pct = argNum (call, "pct", p);
        if (! p) { refusal = "a set_master_wet needs a pct from 0 to 100"; return {}; }
        addOp ("master_wet")->setProperty ("pct", pct);
    }
    else if (op == "set_pre_gain")
    {
        bool p = false; const double db = argNum (call, "db", p);
        if (! p) { refusal = "a set_pre_gain needs a db"; return {}; }
        addOp ("pre_gain")->setProperty ("db", db);
    }
    else if (op == "bypass")
    {
        auto* o = addOp ("bypass");
        if (! needSlot (o)) return {};
        // THE SWEEP, again: the contract's name is `bypassed` (line 104: `{slot: n, bypassed: bool}`) and
        // this read `on`, so every agent bypass refused a call that carried the flag. The chain channel's
        // own op field IS `on`, which is why the mismatch was invisible from inside this function - the
        // name on the WIRE below and the name in the TOOL CALL above are different names.
        bool p = false;
        bool on = argBool (call, "bypassed", p);
        if (! p) on = argBool (call, "on", p);
        if (! p) { refusal = "a bypass needs bypassed: true or false"; return {}; }
        o->setProperty ("on", on);
    }
    else if (op == "remove" || op == "move")
    {
        auto* o = addOp (op);
        if (! needSlot (o)) return {};
        if (op == "move")
        {
            bool p = false; const int to = argInt (call, "to", p);
            if (! p) { refusal = "a move needs a `to` position"; return {}; }
            o->setProperty ("to", to);
        }
    }
    else if (op == "add" || op == "replace")
    {
        auto* o = addOp (op);
        const auto name = argString (call, "name");
        if (name.isEmpty()) { refusal = "an " + op + " needs the plugin's name"; return {}; }
        o->setProperty ("name", name);
        if (op == "replace") { if (! needSlot (o)) return {}; }
        else { bool p = false; const int after = argInt (call, "after", p); if (p) o->setProperty ("after", after); }
        if (const auto ss = argVar (call, "settings_structured"); ss.getDynamicObject() != nullptr)
            o->setProperty ("settings_structured", ss);
        // THE SWEEP: the contract's `add` carries `wet_pct?` (line 98) and this dropped it, so a plugin the
        // server asked for at 40% wet arrived at 100% and the agent had no way to know. parseChainEditOps
        // already reads wet_pct on any op, so passing it through is all that was ever needed.
        { bool pw = false; const double wet = argNum (call, "wet_pct", pw);
          if (pw) o->setProperty ("wet_pct", wet); }
    }
    // ---- ITEM 1 (Sean, 14 Oct 2026): "build" IS NOT HERE ANY MORE --------------------------------------
    //
    // It used to add an edit op named "build" carrying the chain array. No rack has ever had such an op:
    // ChainHost's validator ends `else return bad("unknown operation \"" + op.op + "\"")` and the Link said
    // exactly that to Sean ("op 1 invalid: unknown operation \"build\""), while the own rack's parser
    // returned zero ops and the editor said "that change carried no operation this rack understands".
    //
    // A build goes through buildChain, routed in doOp BEFORE translate runs, exactly as set_rack_out is. It
    // reaches this function only if that routing is missing, and then it must be a plain refusal rather than
    // an op that will be refused one layer lower in worse words.
    else if (op == "build")
    {
        refusal = "a build cannot be sent as an edit op - this build routes it through the rack's own build "
                  "path, and that route is not wired in this build";
        return {};
    }
    else if (op == "set_level")
    {
        // NOT A LEVEL SLOT ANY MORE. The 10 Oct ruling dropped it; levelling lives on the rack-level record,
        // which is the door findTarget reads. An executor that still wrote a Level slot would be addressing a
        // plugin that is not in the rack.
        auto* lv = new juce::DynamicObject();
        const auto option = argString (call, "option").trim().toLowerCase();
        bool pt = false; const double target = argNum (call, "target_lufs", pt);
        if (option.isEmpty() && ! pt)
        { refusal = "a set_level needs an option (match, commercial, pushed, dynamic) or a target_lufs"; return {}; }
        if (option.isNotEmpty()) lv->setProperty ("option", option);
        if (pt) lv->setProperty ("target_lufs", target);
        auto* o = addOp ("set_level");
        o->setProperty ("level", juce::var (lv));
    }
    else if (op == "open_editor")
    {
        auto* o = addOp ("open_editor");
        if (! needSlot (o)) return {};
        const auto where = argString (call, "where").trim().toLowerCase();
        o->setProperty ("where", where == "embed" ? "embed" : "float");
    }
    else
    {
        // NAMED, not a generic failure. A model that sent an op this build does not have should be told which
        // op it was, or it will try the same one again.
        // ITEM 10: set_rack_out is handled in doOp before translate runs, so it never reaches here - but it
        // is NAMED in the refusal, because a model told "that is not an operation this build can do" with no
        // list will try the next word it can think of (which is how output_db on the EQ got dialled).
        refusal = "\"" + op + "\" is not an operation this build can do. It knows: build, add, remove, replace, "
                  "move, set, set_wet, set_io, set_master_wet, set_pre_gain, set_rack_out, bypass, set_level, "
                  "open_editor, undo.";
        return {};
    }

    return juce::var (edits);
}

void ExecutorDo::doOp (const ToolCall& call, Done done)
{
    // AGENT MODE, CHECKED LIVE AND FIRST. Sean's standing rule: the switch stays until he signs off, and OFF
    // means today's behaviour - chat plus Apply. Checked on every call rather than cached, so a user who turns
    // it off mid-round stops the rest of the round.
    if (! (sinks_.agentModeOn && sinks_.agentModeOn()))
    {
        done (ToolOutcome::failure ("not_permitted",
                                    "Agent mode is off, so nothing was changed. Turn it on in Settings if you "
                                    "want EchoJay to make changes itself; otherwise propose the change and I "
                                    "will show it for Apply."));
        return;
    }
    // ---- ITEM 10 (Sean, 13 Oct 2026): THE RACK OUT IS NOT AN EDIT OP ------------------------------------
    // Taken before translate, because translate's whole output is a chain-channel `edit` array and the rack
    // OUT does not live on that channel. The cross-channel gate below applies to it just the same, so the
    // resolution and the confirmation are shared rather than re-implemented here.
    const auto opEarly = argString (call, "op").trim().toLowerCase();
    if (opEarly == "set_rack_out" || opEarly == "rack_out" || opEarly == "set_rack_output")
    {
        if (! sinks_.setRackOutDb)
        { done (ToolOutcome::failure ("not_in_phase", "this build cannot set the rack's out gain")); return; }
        bool haveDb = false, haveDelta = false;
        const double db    = argNum (call, "db",    haveDb);
        const double delta = argNum (call, "delta", haveDelta);
        if (! haveDb && ! haveDelta)
        { done (ToolOutcome::failure ("bad_request", "a set_rack_out needs db (the new gain) or delta (a change in dB)")); return; }
        // `absolute` is explicit when given; otherwise a `delta` argument means relative and a `db` argument
        // means absolute, which is what each word says.
        const bool absolute = argFlagPresent (call, "absolute") ? argFlag (call, "absolute") : haveDb;
        const double value  = haveDb ? db : delta;

        // ONE GATE, SHARED (14 Oct 2026). This branch had its own copy of the resolve-and-confirm block, and
        // a second copy is a second thing to forget: the build branch below needs the same gate, and three
        // hand-written copies of "ask before touching someone else's channel" is how one of them ends up
        // without the question.
        juce::String outTo;
        if (! resolveTargetWithConfirm (call, done, outTo)) return;
        sinks_.setRackOutDb (outTo, value, absolute, [done] (bool ok, juce::String sentence)
        {
            if (! ok) { done (ToolOutcome::failure ("not_applied", sentence.isNotEmpty() ? sentence
                                                                 : juce::String ("the rack's out gain was not changed"))); return; }
            auto* r = new juce::DynamicObject();
            r->setProperty ("applied", true);
            r->setProperty ("op", "set_rack_out");
            done (ToolOutcome::success (juce::var (r),
                                        "set_rack_out:" + juce::String (juce::Time::currentTimeMillis()), sentence));
        });
        return;
    }

    // ---- RULING 4 (Sean, 14 Oct 2026): AN UNDO WALKS THE PLUGIN'S HISTORY ONE STEP -------------------
    //
    // The rig's row C10: having added a compressor, set 3 dB of pre gain and removed slot 1, the agent was
    // asked "undo that" and answered "I've undone the pre-gain change" - it re-dialled an earlier parameter.
    // It had no undo op, so it had to CHOOSE what "that" meant and chose a change it knew how to reverse.
    //
    // Sean's ruling takes the choice away: there is an undo, it steps the one plugin-wide history (which
    // already carries local and remote edits in one order), and whatever that step was is what is reversed.
    //
    // Routed here with set_rack_out and build, before translate, because an undo is not an entry in the chain
    // channel's `edit` vocabulary either. No target resolution and no cross-channel question: the history is
    // the plugin's, one stack for every rack, and its own entries carry the target they belong to - asking
    // "which channel?" would invite an undo aimed at a rack whose step is not the one on top.
    if (opEarly == "undo")
    {
        if (! sinks_.undoHistoryStep)
        { done (ToolOutcome::failure ("not_in_phase", "this build has no undo history to walk")); return; }
        sinks_.undoHistoryStep ([done] (bool ok, juce::String label, int depthBefore, int depthAfter)
        {
            // THE READ-BACK. `ok` is what the history reported; the depth is what actually happened. Both have
            // to agree before this is a success - the rule the other nine sinks were brought under this round.
            if (depthAfter >= depthBefore)
            {
                done (ToolOutcome::failure ("not_applied",
                                            depthBefore <= 0
                                              ? juce::String ("there was nothing to undo")
                                              : "the undo history did not move (still " + juce::String (depthAfter)
                                                + " step" + (depthAfter == 1 ? "" : "s") + " deep)"
                                                + (label.isNotEmpty() ? ", and it reported: " + label : juce::String())));
                return;
            }
            if (! ok)
            {
                // The step came off the stack but its target had gone - the history's own rule, never applied
                // to a different target. That is a true answer and it names the step.
                done (ToolOutcome::failure ("not_applied",
                                            label.isNotEmpty() ? label
                                                               : juce::String ("that step could not be undone")));
                return;
            }
            auto* r = new juce::DynamicObject();
            r->setProperty ("undone",      true);
            r->setProperty ("op",          "undo");
            r->setProperty ("label",       label);
            r->setProperty ("depthBefore", depthBefore);
            r->setProperty ("depthAfter",  depthAfter);
            const juce::String sentence = label.isNotEmpty() ? "Undone: " + label + "."
                                                             : juce::String ("Undone the last change.");
            // No undo token: undoing an undo is a redo, and this op does not offer one.
            done (ToolOutcome::success (juce::var (r), {}, sentence));
        });
        return;
    }

    // ---- ITEM 1 (Sean, 14 Oct 2026): A BUILD TAKES THE RACK'S OWN BUILD ROAD ---------------------------
    //
    // Sean, on the open rack: "add EchoJay Compressor" -> "that change carried no operation this rack
    // understands". On the Kick's Link: "build 2 slots" -> "op 1 invalid: unknown operation \"build\"". The
    // same missing road, reported in two different voices by the two layers that each found an op they did
    // not have.
    //
    // Routed here, before translate, for the same reason set_rack_out is: a build is not an entry in the
    // chain channel's `edit` vocabulary and never was. The sink picks the road by target - the V2's own build
    // path for the open rack, sendChainToLink for a Link - and the LANDING is settled by confirmLanded's
    // read-back, not by this dispatch returning true.
    if (opEarly == "build")
    {
        if (! sinks_.buildChain)
        { done (ToolOutcome::failure ("not_in_phase", "this build cannot build a chain onto a rack")); return; }
        const auto chain = argVar (call, "chain");
        if (chain.getArray() == nullptr || chain.getArray()->isEmpty())
        { done (ToolOutcome::failure ("bad_request", "a build needs a chain array with at least one entry")); return; }

        juce::String buildTo;
        if (! resolveTargetWithConfirm (call, done, buildTo)) return;

        const int wantSlots = structuralWantSlots (call, "build");
        const auto aliveB   = this->alive();
        // `call` BY VALUE, for the reason fix 1(b) recorded on the edit path: the ack can arrive after
        // doOp has returned, and `call` is a reference parameter. A build's chain array is the thing
        // confirmSettings reads, so it has to survive the hop.
        auto callCopy = std::make_shared<ToolCall> (call);
        sinks_.buildChain (buildTo, chain, [this, aliveB, done, buildTo, wantSlots, callCopy] (bool ok, juce::String sentence)
        {
            if (! ok)
            {
                done (ToolOutcome::failure ("not_applied", sentence.isNotEmpty() ? sentence
                                                        : juce::String ("the build was not sent to the rack")));
                return;
            }
            if (aliveB.expired())
            { done (ToolOutcome::success (juce::var (new juce::DynamicObject()), {}, sentence)); return; }
            confirmLanded (buildTo, wantSlots, "build", sentence, done, 0, callCopy.get());
        });
        return;
    }

    if (! sinks_.applyChainOps)
    {
        done (ToolOutcome::failure ("not_in_phase", "this build has no way to send that change to a rack"));
        return;
    }

    juce::String refusal;
    const auto edits = translate (call, refusal);
    if (refusal.isNotEmpty() || edits.getArray() == nullptr || edits.getArray()->isEmpty())
    {
        // THE SENTENCE REACHES THE MODEL UNPARAPHRASED (contract section 4.3). Nothing rewrites it here.
        done (ToolOutcome::failure ("bad_request", refusal.isNotEmpty() ? refusal
                                                                       : juce::String ("that change carried nothing to do")));
        return;
    }

    const auto opName = argString (call, "op");

    // ---- ITEM 6(b), 12 Oct 2026: ACTING ON ANOTHER CHANNEL IS ASK-FIRST -------------------------------------
    //
    // This used to apply to this->target() and never read the call's `channel` at all, so "a touch more top end
    // on the kick" with the V2 on Mix Bus could not reach the kick even once the uid resolved. Routing it is half
    // the fix; the other half is that it must NOT be silent. Changing a channel the user is not looking at is the
    // one edit they cannot see happen, so it is gated on an explicit confirmation and the question names the
    // track in the words Sean asked for.
    //
    // The gate fails CLOSED: argFlag returns false for an absent, misspelled or unparseable confirm, so the
    // worst case is one extra question, never an unasked edit on someone else's channel.
    // The same gate as the two branches above, from the one implementation (see resolveTargetWithConfirm).
    juce::String applyTo;
    if (! resolveTargetWithConfirm (call, done, applyTo)) return;

    // ---- ITEM 1 (Sean, 13 Oct 2026): A LANDING IS WHAT A READBACK SHOWS, NOT WHAT A SEND RETURNED -------
    //
    // Sean approved a build for the Kick, was told it applied, and the Kick's Link had nothing on it. The
    // agent then read that empty rack for 18 rounds ("slot 2 is not on the rack (1..0)"). The sink's half of
    // this is fixed where it is written; this is the other half, and it belongs HERE because it is the half a
    // guard can drive: the readback goes through src_, so a fake source proves the refusal without a Link.
    //
    // The rule is Sean's: a build never reports success unless a readback shows the slots on the target rack.
    // Applied to every STRUCTURAL op, because "the op was acked but the rack is empty" is not special to
    // build - it is what a rack that threw the batch away looks like from here, whatever put it there.
    const int wantSlots = structuralWantSlots (call, opName);
    const auto aliveW   = this->alive();
    // `call` BY VALUE (fix 1(b)): this callback is asynchronous - the ack can arrive after doOp has
    // returned - and `call` is a reference parameter, so capturing it by reference would read a dead
    // ToolCall. A copy is cheap (ids, a name and a var) and cannot dangle.
    sinks_.applyChainOps (applyTo, edits, [this, aliveW, done, opName, applyTo, wantSlots, call]
                          (bool ok, juce::String sentence, juce::String technical)
    {
        if (! ok)
        {
            // ---- SEAN'S CHECK 5 (15 Oct 2026): TWO AUDIENCES, WRITTEN FOR SEPARATELY --------------
            // This was failure(code, sentence), so errorMessage and forTheUser() were the same string -
            // the model got the summary and Details disclosed the summary again. failureFor keeps the
            // rack's own plain sentence for the person and gives the model (and Details) the technical
            // account the rack actually sent. With nothing technical to add it falls back, and then no
            // Details control appears at all, which is correct: there is nothing behind it.
            const auto userLine = sentence.isNotEmpty() ? sentence
                                                        : juce::String ("that change was not applied");
            if (technical.isNotEmpty())
                done (ToolOutcome::failureFor ("not_applied", technical, userLine));
            else
                done (ToolOutcome::failure ("not_applied", userLine));
            return;
        }
        juce::ignoreUnused (technical);
        // The ack can arrive after the editor that owns this executor has closed. Checked before src_ is
        // touched, for the reason recorded at aliveToken_'s declaration.
        if (aliveW.expired())
        {
            done (ToolOutcome::success (juce::var (new juce::DynamicObject()), {}, sentence));
            return;
        }
        // ---- FIX 1(b) (Sean, 14 Oct): A `set` READS ITS VALUES BACK -----------------------------------
        //
        // "make the kick punchier" sent eight parameters in a shape the dial path could not read, applied
        // NONE of them, and this path reported success - because confirmLanded counts SLOTS, which a `set`
        // never changes, so the check was true before and after and could not fail. The closing text then
        // told Sean attack 18 ms, release 85 ms and makeup, while the compressor sat at 10 / 120 / 0.
        //
        // A structural read-back cannot answer a value question. So a `set` is confirmed against the VALUES.
        if (opName == "set")
        { confirmValues (call, applyTo, sentence, done); return; }
        confirmLanded (applyTo, wantSlots, opName, sentence, done, 0, &call);
    });
}

// ---- FIX 1(b): THE VALUE READ-BACK ---------------------------------------------------------------------
// Read every parameter the call asked for and compare it with what the slot now holds. Three outcomes, and
// the middle one is the one that was missing:
//   every value matches                    -> applied, naming them
//   some do not                            -> NOT APPLIED, naming each with asked vs actual
//   none of them can be read at all        -> applied-but-unconfirmed, said plainly rather than claimed
// The tolerance is per magnitude rather than absolute: a device stores 18 ms as 18.0 and a ratio as 4.0,
// but a dB value may round, and comparing 0.1 dB with == would make an honest apply look like a failure.
void ExecutorDo::confirmValues (const ToolCall& call, const juce::String& applyTo,
                                juce::String sentence, Done done)
{
    const auto ss = argVar (call, "settings_structured");
    auto* root = ss.getDynamicObject();
    // The asked values live either under "params" or (since fix 1(a)) flat at the top level.
    juce::DynamicObject* asked = root;
    if (root != nullptr)
    {
        // BOTH SHAPES, as checkSlotValues does (16 Oct 2026). A third party is dialled through
        // `controls`, keyed by the plugin's own control name, and reading only `params` meant a `set`
        // on one could never be confirmed - the same hole the add path had.
        if (auto* p = root->getProperty ("params").getDynamicObject())        asked = p;
        else if (auto* c = root->getProperty ("controls").getDynamicObject()) asked = c;
    }
    bool haveSlot = false;
    const int slot1 = argInt (call, "slot", haveSlot);

    if (asked == nullptr || ! haveSlot || ! src_.slotParam)
    {
        // Nothing to compare, or no way to compare it. Say which, rather than reporting a bare success.
        auto* r = new juce::DynamicObject();
        r->setProperty ("applied", true); r->setProperty ("confirmed", false);
        r->setProperty ("why", "this build could not read the slot's values back");
        done (ToolOutcome::success (juce::var (r), "set:" + juce::String (juce::Time::currentTimeMillis()),
                                    sentence + " I could not read the values back to confirm them."));
        return;
    }

    juce::StringArray matched, missed, unreadable, matchedPlain, missedPlain;
    for (auto& kv : asked->getProperties())
    {
        const auto id = kv.name.toString();
        const auto want = kv.value;
        if (! (want.isDouble() || want.isInt() || want.isInt64())) continue;   // a mode name, not a number
        const double wanted = (double) want;
        double got = 0.0;
        if (! src_.slotParam (applyTo, slot1, id, got)) { unreadable.add (id); continue; }
        const double tol = juce::jmax (0.05, std::abs (wanted) * 0.02);
        if (std::abs (got - wanted) <= tol)
        {
            matched.add (id + " " + juce::String (got, 2));
            matchedPlain.add (echojay::params::plainValuePhrase (id, got));   // Sean's #6
        }
        else
        {
            missed.add (id + " is " + juce::String (got, 2) + " (asked " + juce::String (wanted, 2) + ")");
            missedPlain.add (echojay::params::plainValuePhrase (id, got)
                             + " (asked " + echojay::params::plainValuePhrase (id, wanted) + ")");
        }
    }

    auto* r = new juce::DynamicObject();
    r->setProperty ("slot", slot1);
    r->setProperty ("applied_params",    matched.joinIntoString (", "));
    r->setProperty ("unapplied_params",  missed.joinIntoString (", "));
    r->setProperty ("unreadable_params", unreadable.joinIntoString (", "));

    if (! missed.isEmpty())
    {
        // THE WHOLE POINT. The model must not be able to report this as done - and the sentence names the
        // figures, so the closing text can only be built from what actually landed.
        const juce::String why = "the values did not take: " + missed.joinIntoString ("; ")
                               + (matched.isEmpty() ? juce::String()
                                                    : ". What did land: " + matched.joinIntoString (", "));
        auto o = ToolOutcome::failureFor ("not_applied", why,
                                          "The values did not take: " + missedPlain.joinIntoString (", ")
                                          + (matchedPlain.isEmpty() ? juce::String()
                                                                    : ". What did land: " + matchedPlain.joinIntoString (", ")));
        o.result = juce::var (r);
        done (o);
        return;
    }
    if (matched.isEmpty())
    {
        r->setProperty ("confirmed", false);
        done (ToolOutcome::success (juce::var (r), "set:" + juce::String (juce::Time::currentTimeMillis()),
                                    sentence + " I could not read these values back to confirm them"
                                    + (unreadable.isEmpty() ? juce::String() : " (" + unreadable.joinIntoString (", ") + ")")
                                    + "."));
        return;
    }
    r->setProperty ("confirmed", true);
    done (ToolOutcome::successFor (juce::var (r), "set:" + juce::String (juce::Time::currentTimeMillis()),
                                   "read back: " + matched.joinIntoString (", "),
                                   "read back " + matchedPlain.joinIntoString (", ")));   // Sean's #6
}


// =======================================================================================================
//  SEAN'S CHECK 5 (15 Oct 2026): SETTINGS CARRIED ON add / replace / build ARE READ BACK TOO
//
//  The complaint, in his words: *"It was added to the Link, but its Attack is still 0.00 dB, while the
//  closing said 'attack pushed up +6 dB and sustain pulled down -2 dB'. Last night's fix made SET read
//  values back; settings carried on ADD and BUILD are still confirmed by slot count only."*
//
//  The log is unambiguous about what happened to those settings:
//
//      EJDial: slot 1 ("MTransient") NO MAP for fp=3decf53d6bc0 ... -> noMap
//      EJApply: slot 1 "MTransient" hadState=Y seeded=Y -> withheld=N edited=N action=LEAVE-UNEDITED
//
//  No map, so nothing was dialled. The add's read-back counted ONE SLOT, found one, and said yes - which
//  it was right about. The slot is not the settings, and nothing was asking about the settings at all.
//
//  THREE OUTCOMES, AND NONE OF THEM IS "SUCCESS FOR THE SETTINGS" WITHOUT A READ:
//    applied        every asked value was read back and matches
//    not_applied    a value was read back and is NOT what was asked (or the rack dialled nothing)
//    not_confirmed  this build cannot read that plugin's values at all - a third party with no map
//
//  The op itself still SUCCEEDS when the slot landed, because it did: failing it would make the model add
//  the plugin a second time. What changes is that the settings are reported separately and honestly, so
//  the closing text can only be built from what was actually confirmed.
// =======================================================================================================
ExecutorDo::SettingsCheck ExecutorDo::checkSlotValues (const juce::String& applyTo, int slot1,
                                                       const juce::var& settingsStructured) const
{
    SettingsCheck out;
    auto* root = settingsStructured.getDynamicObject();
    if (root == nullptr || slot1 < 1) return out;
    // WHOSE KNOBS THESE ARE (Sean's #6): the row reads "Compressor: threshold -14 dB", so the line
    // needs the plugin's name. Read from the rack, which is the only place it exists.
    {
        const auto rack = applyTo.isNotEmpty() ? (src_.linkRack ? src_.linkRack (applyTo) : RackRead())
                                               : (src_.ownRack  ? src_.ownRack()          : RackRead());
        if (rack.valid)
            for (const auto& sl : rack.slots)
                if (sl.n == slot1) { out.slotName = sl.name; break; }
    }
    // ---- BOTH SHAPES, BECAUSE A THIRD PARTY USES THE OTHER ONE (16 Oct 2026) -------------------
    // This read `params` (and the flat form) only. A THIRD-PARTY slot is dialled through `controls`,
    // keyed by the plugin's own control name - the shape the contract names and the shape Sean's four
    // plugins arrive in - so the one case last night's read-back was written for was the one case it
    // could not see. Row T1 caught it on a real MTransient: the rack even said so itself, *"a 'params'
    // wrapper is a built-in device payload and reached the third-party map path"*.
    juce::DynamicObject* asked = root;
    if (auto* p = root->getProperty ("params").getDynamicObject())        asked = p;
    else if (auto* c = root->getProperty ("controls").getDynamicObject()) asked = c;
    for (auto& kv : asked->getProperties())
    {
        const auto id   = kv.name.toString();
        const auto want = kv.value;
        if (! (want.isDouble() || want.isInt() || want.isInt64())) continue;   // a mode name, not a number
        ++out.askedCount;
        const double wanted = (double) want;
        double got = 0.0;
        if (! src_.slotParam || ! src_.slotParam (applyTo, slot1, id, got)) { out.unreadable.add (id); continue; }
        // Per magnitude, not absolute: a device stores 18 ms as 18.0 and a ratio as 4.0, but a dB value
        // may round, and == would make an honest apply look like a failure.
        const double tol = juce::jmax (0.05, std::abs (wanted) * 0.02);
        if (std::abs (got - wanted) <= tol)
        {
            out.matched.add ("slot " + juce::String (slot1) + " " + id + " " + juce::String (got, 2));
            out.matchedIds.add (id);
            // SEAN'S #6: the same fact, in words. The model keeps the line above.
            out.matchedPlain.add (echojay::params::plainValuePhrase (id, got));
        }
        else
        {
            out.missed.add ("slot " + juce::String (slot1) + " " + id + " is " + juce::String (got, 2)
                            + " (asked " + juce::String (wanted, 2) + ")");
            out.missedIds.add (id);
            out.missedPlain.add (echojay::params::plainValuePhrase (id, got)
                                 + " (asked " + echojay::params::plainValuePhrase (id, wanted) + ")");
        }
    }
    return out;
}

void ExecutorDo::confirmSettings (const ToolCall& call, const juce::String& applyTo, const juce::String& opName,
                                  int slotsOnRack, juce::String landedSentence, Done done, int triesLeft,
                                  bool pendingBudget)
{
    // TWO AUDIENCES, ONE CALL (Sean's #10, 16 Oct 2026). `userLine` is the row's words; `modelLine`
    // is what the model is told, which may carry ids and instructions. Passing only one is the old
    // behaviour and means "this sentence is fit for both", which is true of the short ones.
    const auto finish = [this, opName, done] (juce::var result, const juce::String& userLine,
                                              const juce::String& modelLine = {})
    {
        const auto token = opName + ":" + juce::String (juce::Time::currentTimeMillis());
        if (modelLine.isEmpty())
            done (ToolOutcome::success (std::move (result), token, userLine));
        else
            done (ToolOutcome::successFor (std::move (result), token, modelLine, userLine));
    };

    // ---- which slots carry asked values, and where they landed ------------------------------------
    struct Ask { int slot1 = 0; juce::var settings; };
    std::vector<Ask> asks;
    // ---- SEVERAL COPIES OF THE SAME PLUGIN, AND NO WAY TO TELL THEM APART (16 Oct 2026) --------
    // Set when the rack holds more than one slot of this name, nothing matched the asked values, and
    // the server stated no placement. The read-back then genuinely does not know which slot this add
    // made, and the one thing it must not do is name a figure from one of the others.
    bool slotAmbiguous = false;
    int  sameNameCount = 0;
    juce::String ambiguousName;
    if (opName == "build")
    {
        // ---- BY NAME, BECAUSE A BUILD IS NOT INSTANT (16 Oct 2026, Sean's #3) --------------------
        //
        // "the chain block IS the whole rack, so slot i of the block is slot i+1 of the rack" is true
        // of the rack the build PRODUCES, and the read-back can run while the rack is still the one
        // it is replacing: the structural check is satisfied by a slot count, and a one-slot build
        // over a rack that already had slots is satisfied immediately - by the OLD first slot. Sean
        // saw the result as "Added it. Settings not confirmed: this build cannot read that plugin's
        // values back (the threshold and ratio)" over an EchoJay Compressor whose dial log says
        // `requested=2 applied=2 manual=0 readbackMiss=0 status=applied`. The device had taken both
        // values; the read was looking at another plugin.
        //
        // So each block entry finds ITS OWN slot by name, exactly as an add does, and falls back to
        // the positional answer when the rack cannot be read or the name is not there yet.
        const auto chain = argVar (call, "chain");
        const auto rack = applyTo.isNotEmpty() ? (src_.linkRack ? src_.linkRack (applyTo) : RackRead())
                                               : (src_.ownRack  ? src_.ownRack()          : RackRead());
        if (auto* arr = chain.getArray())
            for (int i = 0; i < arr->size(); ++i)
                if (auto* o = (*arr)[i].getDynamicObject())
                {
                    const auto ss = o->getProperty ("settings_structured");
                    if (ss.getDynamicObject() == nullptr) continue;
                    int slot1 = i + 1;
                    const auto wantName = o->getProperty ("name").toString().trim();
                    if (rack.valid && wantName.isNotEmpty())
                    {
                        std::vector<int> hits;
                        for (const auto& sl : rack.slots)
                            if (echojay::agent::namesMatchLoosely (sl.name, wantName)) hits.push_back (sl.n);
                        if (hits.size() == 1) slot1 = hits.front();
                        else if (hits.size() > 1)
                        {
                            // Several of the same plugin in one build: the i-th of them is this one.
                            int seen = 0;
                            for (int j = 0; j < i; ++j)
                                if (auto* oj = (*arr)[j].getDynamicObject())
                                    if (echojay::agent::namesMatchLoosely (oj->getProperty ("name").toString(), wantName))
                                        ++seen;
                            slot1 = hits[(size_t) juce::jlimit (0, (int) hits.size() - 1, seen)];
                        }
                    }
                    asks.push_back ({ slot1, ss });
                }
    }
    else
    {
        const auto ss = argVar (call, "settings_structured");
        if (ss.getDynamicObject() != nullptr)
        {
            bool haveSlot = false;
            int slot1 = argInt (call, "slot", haveSlot);
            // ---- SEAN'S #9 (16 Oct 2026): READ BACK THE SLOT IT LANDED IN, NOT THE LAST ONE --------
            //
            // His case: "Add Renaissance Axx to the kick with threshold at -20 dB" on a Link. The dial
            // worked - the plugin shows Thresh -20.0 - and the read-back still said "this build cannot
            // read that plugin's values back". It was reading the WRONG SLOT: an `add` carries `after`,
            // not `slot`, and the server placed this one at the FRONT ("before the EchoJay EQ"), while
            // this line assumed the end of the rack. The slot it read held a different plugin, which
            // has no control called Threshold, so every value came back "unreadable".
            //
            // BY NAME FIRST, because that is the only thing that cannot drift: the rack is read and the
            // slot holding the plugin this op just added is the slot to confirm. `after` is the
            // fallback (after + 1, the contract's placement), and the end of the rack the last resort.
            if (opName == "add" && (! haveSlot || slot1 < 1))
            {
                slot1 = juce::jmax (1, slotsOnRack);
                const auto wantName = argString (call, "name").trim();
                bool byAfter = false;
                const int after = argInt (call, "after", byAfter);
                if (byAfter && after >= 0) slot1 = after + 1;
                if (wantName.isNotEmpty())
                {
                    const auto rack = applyTo.isNotEmpty() ? (src_.linkRack ? src_.linkRack (applyTo) : RackRead())
                                                           : (src_.ownRack  ? src_.ownRack()          : RackRead());
                    if (rack.valid)
                    {
                        std::vector<int> hits;
                        for (const auto& sl : rack.slots)
                            if (echojay::agent::namesMatchLoosely (sl.name, wantName)) hits.push_back (sl.n);
                        // ONE match is the answer. Several means the rack holds two of the same plugin,
                        // and then the placement decides between them rather than a guess: the one at
                        // `after + 1` if that is one of them, else the last - which is where an add with
                        // no placement goes.
                        if (hits.size() == 1) slot1 = hits.front();
                        else if (hits.size() > 1)
                        {
                            // ---- TWO OF THE SAME PLUGIN ON ONE RACK (16 Oct 2026) -----------------
                            // The rig put a second EchoJay Compressor on a rack that already had one
                            // and the read-back answered about the OTHER one: "the settings did NOT
                            // take: the threshold - threshold -24 dB (asked -20 dB)" - a true
                            // sentence about the wrong slot. Position cannot settle it (an add
                            // appends, then the rack may walk it), so the VALUES do: the slot that
                            // best matches what was asked is the one this add produced. If none
                            // matches, the placement decides, as before, and the answer is honest
                            // about that slot.
                            // ---- AND A ZERO-MATCH "BEST" IS NOT AN ANSWER (16 Oct 2026) -------
                            //
                            // The live pass on a Link failed four attempts in five on this line. The
                            // Kick's rack already held a Renaissance Axx (never dialled, Threshold
                            // 0.00); the add appended a second one, which dialled to -18. Nothing
                            // matched the asked value at read time - the new slot had not published
                            // its landed figure yet - so EVERY hit scored zero, and the loop below
                            // nevertheless "improved" on its starting answer at the first
                            // iteration (0 > -1) and picked hits[0]: the OLD slot, sitting at 0.00.
                            // The step then said "the settings did NOT take: threshold 0 (asked
                            // -18)" - a true sentence about the wrong slot, which is the exact fault
                            // this block was written to end.
                            //
                            // So the PLACEMENT is the answer unless the values genuinely name a
                            // better one: an add with no placement appends, so the last hit is the
                            // one it just made; `after + 1` wins when the server asked for a
                            // position. A value match only overrides when something actually
                            // matched, and the LAST such slot wins a tie for the same reason.
                            int chosen = hits.back();
                            if (byAfter)
                                for (int h : hits) if (h == after + 1) chosen = h;
                            int bestMatched = 0;
                            for (int h : hits)
                            {
                                const auto c = checkSlotValues (applyTo, h, ss);
                                const int matched = c.matched.size();
                                if (matched > 0 && matched >= bestMatched) { bestMatched = matched; chosen = h; }
                            }
                            // THE HONEST ANSWER WHEN NEITHER THE VALUES NOR THE PLACEMENT DECIDES.
                            // The live pass, fourth attempt on a Link that had accumulated four
                            // Renaissance Axx: "chose slot 4 by the append position" and then "slot 4
                            // Threshold is -10.00 (asked -6.00)" - which was the THIRD attempt's
                            // value, read off the wrong copy. An add on a Link does not always land
                            // last (the Link's own apply may insert), so the append position is a
                            // guess, and a guess must not be reported as a measurement.
                            if (bestMatched == 0 && ! byAfter)
                            {
                                slotAmbiguous = true;
                                sameNameCount = (int) hits.size();
                                ambiguousName = wantName;
                            }
                            EchoJay_NSLog (("EJAgent: " + juce::String ((int) hits.size()) + " slots named \""
                                            + wantName + "\"; chose slot " + juce::String (chosen) + " by "
                                            + (bestMatched > 0 ? "value match (" + juce::String (bestMatched)
                                                                 + " of the asked values read back there)"
                                                               : juce::String (byAfter ? "the placement the server asked for"
                                                                                       : "the append position (the last of them)"))
                                              ).toRawUTF8());
                            slot1 = chosen;
                        }
                        EchoJay_NSLog (("EJAgent: add read-back targets slot " + juce::String (slot1)
                                        + " for \"" + wantName + "\" (" + juce::String ((int) hits.size())
                                        + " slot(s) match by name, after="
                                        + (byAfter ? juce::String (after) : juce::String ("-"))
                                        + ", rack has " + juce::String ((int) rack.slots.size()) + ")").toRawUTF8());
                    }
                }
            }
            // ANY OTHER OP WITHOUT A SLOT IS NOT GUESSED AT. Defaulting to slot 1 would compare the
            // wrong slot's values and report an honest apply as "not applied" - a false negative inside
            // the one check whose whole purpose is not to say things that are not so. No slot, no
            // settings claim either way: the op's own result stands and nothing is added to it.
            if (slot1 >= 1) asks.push_back ({ slot1, ss });
            else
                EchoJay_NSLog (("EJAgent: " + opName + " carried settings but named no slot, so there is "
                                "nothing to read them back FROM - the settings are neither confirmed nor "
                                "denied, and no figure is claimed").toRawUTF8());
        }
    }

    if (asks.empty()) { finish (juce::var (new juce::DynamicObject()), landedSentence); return; }

    SettingsCheck all;
    for (const auto& a : asks)
    {
        const auto c = checkSlotValues (applyTo, a.slot1, a.settings);
        all.askedCount += c.askedCount;
        all.matched.addArray (c.matched);
        all.missed.addArray (c.missed);
        all.unreadable.addArray (c.unreadable);
        all.matchedIds.addArray (c.matchedIds);
        all.missedIds.addArray (c.missedIds);
        all.matchedPlain.addArray (c.matchedPlain);
        all.missedPlain.addArray (c.missedPlain);
        if (all.slotName.isEmpty()) all.slotName = c.slotName;
    }
    if (all.askedCount == 0) { finish (juce::var (new juce::DynamicObject()), landedSentence); return; }

    // ---- GIVE A PENDING DIAL ITS BOUND BEFORE CALLING IT A FAILURE (16 Oct 2026) ----------------
    //
    // The rig caught this on a real Renaissance Axx: "Added it, but the settings did NOT take: the
    // threshold - threshold 0 (asked threshold -30)" - on a plugin whose map had not arrived yet.
    // The structural landing (the slot exists) and the VALUE landing (the map came back and the
    // write went in) are two different moments, and this read them as one. A miss is re-read, up to
    // three times at 500 ms, and only then reported. A clean read still finishes immediately, so
    // nothing that works today gets slower.
    // UNREADABLE COUNTS TOO (16 Oct 2026). A value that cannot be read yet is the same story as one
    // that has not landed yet: the map is still coming, or the device is still being built. Both are
    // re-read; a clean confirmation still finishes at once.
    // ---- THE DIAL ITSELF SAYS WHETHER IT HAS FINISHED (16 Oct 2026, the joint live pass) --------
    //
    // Two of five live attempts at "add Renaissance Axx to the Kick's rack and set its Threshold"
    // came back "Added it, but the settings did NOT take: slot 1 Threshold is 0.00 (asked -18.00)".
    // The write had not failed; it had not happened yet. On a Link the round trip is V2 -> server ->
    // V2 -> Link -> dial -> sidecar, and six looks at 800 ms is not always enough of it.
    //
    // Counting looks was the wrong instrument: it measures how many times this function ran, not
    // whether the thing it waits for is still coming. The dial now says so itself - ChainHost's own
    // DialStatus::pending, carried to a Link's reader in its sidecar - so the wait is on the signal,
    // with a stated ceiling: up to kPendingLooks at 800 ms (about twenty seconds) WHILE the slot
    // reports pending, and the original six when it does not. A clean read still finishes at once.
    bool anyDialPending = false;
    if (src_.slotDialPending != nullptr)
        for (const auto& a : asks)
            if (src_.slotDialPending (applyTo, a.slot1)) { anyDialPending = true; break; }
    static constexpr int kPendingLooks = 25;        // x 800 ms = about twenty seconds
    // Raised ONCE, and `pendingBudget` is how this function knows it already did: the recursion
    // counts down through the default budget on its way to zero, so a test on the count alone would
    // raise the ceiling again every time it passed six and the read-back would never end.
    const bool enterPending = anyDialPending && ! pendingBudget;
    const int  looksLeft    = enterPending ? kPendingLooks : triesLeft;
    const bool inPending    = pendingBudget || enterPending;
    if ((! all.missed.isEmpty() || ! all.unreadable.isEmpty()) && looksLeft > 1 && sinks_.afterMs != nullptr)
    {
        const auto aliveW = this->alive();
        auto callCopy = std::make_shared<ToolCall> (call);
        EchoJay_NSLog (("EJAgent: " + juce::String (all.missed.size() + all.unreadable.size())
                        + " setting(s) have not landed or cannot be read yet - "
                        "re-reading in 800 ms (" + juce::String (looksLeft - 1) + " more look(s), dial "
                        + (anyDialPending ? "STILL PENDING" : "settled")
                        + "), because a map that is still arriving is not a setting that did not take").toRawUTF8());
        // 800 ms x 6 = about four seconds, not one and a half. A third party's map comes from the
        // SERVER: the rig watched the V2 ask for it, finish the request without it, and receive it a
        // moment after the read-back had already given up and said "this build cannot read that
        // plugin's values back". A bound that is shorter than the thing it waits for is not a bound,
        // it is a race - and this one is still bounded, still only runs when something is missing,
        // and still lets a clean read finish at once.
        sinks_.afterMs (800, [this, aliveW, callCopy, applyTo, opName, slotsOnRack, landedSentence, done,
                              looksLeft, inPending]
        {
            if (aliveW.expired())
            { done (ToolOutcome::success (juce::var (new juce::DynamicObject()), {}, landedSentence)); return; }
            confirmSettings (*callCopy, applyTo, opName, slotsOnRack, landedSentence, done,
                             looksLeft - 1, inPending);
        });
        return;
    }

    // ---- A DIAL STILL RUNNING WHEN THE CEILING IS REACHED IS NOT A FAILED WRITE -----------------
    // The absence of a reading is not evidence that the write failed, and "Threshold is 0.00 (asked
    // -18.00)" is a positive claim that it did. If the slot is STILL pending now, every miss becomes
    // an UNREADABLE - which the wording below reports as "sent, not confirmed" instead - and the log
    // says the ceiling was reached rather than that the plugin refused.
    // A dial still running at the ceiling is the REASON nothing could be read, whether the values
    // came back wrong-and-unmatched or could not be read at all.
    const bool stillDialling = anyDialPending;
    // AMBIGUOUS SLOT: the same conversion, for the same reason. A miss read off a slot that might not
    // be the one this add made is not evidence of anything, so it is reported as not confirmed.
    if (slotAmbiguous && ! all.missed.isEmpty())
    {
        EchoJay_NSLog (("EJAgent: the rack holds " + juce::String (sameNameCount) + " slots named \""
                        + ambiguousName + "\", none of them reads back what was asked, and the server "
                        "stated no placement - so this read-back cannot say WHICH of them the add made. "
                        + juce::String (all.missed.size()) + " setting(s) are reported as NOT CONFIRMED "
                        "rather than as not applied; naming a figure from one of the others is the "
                        "false claim this build must never make").toRawUTF8());
        all.unreadable.addArray (all.missedIds);
        all.missed.clear(); all.missedIds.clear(); all.missedPlain.clear();
    }
    if (anyDialPending && ! all.missed.isEmpty())
    {
        EchoJay_NSLog (("EJAgent: the dial is STILL pending after about twenty seconds, so "
                        + juce::String (all.missed.size()) + " setting(s) are reported as NOT CONFIRMED "
                        "rather than as not applied - no reading is not the same as a refused write")
                           .toRawUTF8());
        all.unreadable.addArray (all.missedIds);
        all.missed.clear(); all.missedIds.clear(); all.missedPlain.clear();
    }

    auto* r = new juce::DynamicObject();
    r->setProperty ("applied", true);                 // the STRUCTURE landed; that is what this says
    r->setProperty ("op", opName);
    r->setProperty ("settings_asked",      all.askedCount);
    r->setProperty ("settings_applied",    all.matched.joinIntoString (", "));
    r->setProperty ("settings_unapplied",  all.missed.joinIntoString ("; "));
    r->setProperty ("settings_unreadable", all.unreadable.joinIntoString (", "));
    // ---- SEAN, 15 Oct 2026: THE IDS ARE FOR THE MODEL, THE WORDS ARE FOR THE PERSON ------------
    // "The not-confirmed line shows raw parameter IDs. Show plain names." Both exist from here: the
    // fields above keep the wire's vocabulary, which the model needs to retry or explain, and the
    // *_plain fields - and every landed line below - read as English.
    // THE ID LISTS, not the prose lists: `missed` entries are whole sentences and the renderer takes the
    // last whitespace token out of one, so "slot 1 attack_db is 0.00 (asked 6.00)" came out as "6.00)".
    const auto unreadablePlain = echojay::params::plainParamList (all.unreadable);
    const auto askedPlain      = echojay::params::plainParamList (all.missedIds);
    const auto landedPlain     = echojay::params::plainParamList (all.matchedIds);
    if (unreadablePlain.isNotEmpty()) r->setProperty ("settings_unreadable_plain", unreadablePlain);
    if (askedPlain.isNotEmpty())      r->setProperty ("settings_unapplied_plain", askedPlain);
    if (landedPlain.isNotEmpty())     r->setProperty ("settings_applied_plain", landedPlain);

    if (! all.missed.isEmpty())
    {
        r->setProperty ("settings_state", "not_applied");
        r->setProperty ("settings_confirmed", false);
        finish (juce::var (r),
                // Same rule: the add landed, the SETTINGS did not. Lead with the part that is true.
                "Added it, but the settings did NOT take: " + askedPlain
                + (all.missedPlain.isEmpty() ? juce::String()
                                             : " - " + all.missedPlain.joinIntoString (", "))
                + (all.matchedPlain.isEmpty() ? juce::String()
                                              : ". What did land: " + all.matchedPlain.joinIntoString (", ")),
                // ...and the model's half, which carries the instruction and the ids (Sean's #10: it is
                // NOT shown on the row).
                "Added it, but the settings did NOT take: " + all.missed.joinIntoString ("; ")
                + (all.matched.isEmpty() ? juce::String()
                                         : ". What did land: " + all.matched.joinIntoString (", "))
                + ". Report the plugin as added with its settings NOT applied, and do not name any value "
                  "you were not given here.");
        return;
    }
    if (all.matched.isEmpty())
    {
        // NOTHING READABLE. This is MTransient's case exactly: a third party with no parameter map, so
        // the values were never written and cannot be read either. "added, settings not applied" is
        // Sean's own wording, and it is the only honest thing to say.
        r->setProperty ("settings_state", "not_confirmed");
        r->setProperty ("settings_confirmed", false);
        // ---- WHY IT CANNOT BE CONFIRMED, AND THE TWO REASONS ARE NOT THE SAME (16 Oct 2026) ----
        // "this build cannot read that plugin's values back" is a claim about the BUILD, and it was
        // being made over a dial that was simply still running - the live pass caught it on a Link,
        // where the round trip outlasts the read-back's ceiling. A slot that is still dialling is
        // told apart in words, and neither sentence names a figure.
        const auto why = slotAmbiguous
            ? ("this rack holds " + juce::String (sameNameCount) + " copies of that plugin and I cannot "
               "tell which one I just added, so I will not name a figure from one of the others")
            : stillDialling
                ? juce::String ("the plugin had not reported them yet")
                : juce::String ("this build cannot read that plugin's values back");
        finish (juce::var (r),
                // SEAN'S #5 (16 Oct 2026): "Never 'Not applied: add X' when X was added: say 'Added X.
                // Settings not confirmed.'" The sentence LEADS with what landed, so no renderer and no
                // closing text can turn a successful add into a failure.
                //
                // SEAN'S #10: the instruction that used to finish this sentence - "Report it as ADDED
                // ... do NOT state any figure" - is addressed to the MODEL and he read it in his step
                // details. It now travels on the model's line only.
                "Added it. Settings not confirmed: " + why
                + (unreadablePlain.isEmpty() ? juce::String() : " (" + unreadablePlain + ")")
                + ", so there is no evidence they were applied.",
                "Added it. Settings not confirmed: " + why
                + (unreadablePlain.isEmpty() ? juce::String() : " (" + unreadablePlain + ")")
                + ", so there is no evidence they were applied. Report it as ADDED, with its settings "
                  "not confirmed. Do NOT call the add itself 'not applied', and do NOT state any "
                  "figure for the settings."
                + (stillDialling ? juce::String (" The dial was still running when the read-back "
                                                 "reached its ceiling, so this is NOT a refused write.")
                                 : juce::String()));
        return;
    }
    r->setProperty ("settings_state", all.unreadable.isEmpty() ? "applied" : "partly_confirmed");
    r->setProperty ("settings_confirmed", all.unreadable.isEmpty());
    // ---- SEAN'S #6 (16 Oct 2026): THE ROW READS AS WORDS, THE RESULT KEEPS THE IDS --------------
    // What he saw: "slot 2 threshold_db -14.00, slot 2 ratio 4.00". What he asked for: "Compressor:
    // threshold -14 dB, ratio 4:1". Same figures, same order, one audience each - the model's copy
    // travels on the result object (settings_applied), untouched.
    const auto plainFigures = (all.slotName.isNotEmpty() ? all.slotName + ": " : juce::String())
                            + all.matchedPlain.joinIntoString (", ");
    finish (juce::var (r),
            landedSentence + "; read back " + plainFigures
            + (unreadablePlain.isEmpty() ? juce::String()
                                         : ". Not readable, so not confirmed: " + unreadablePlain),
            // the MODEL's half: the ids and the slot numbers it needs to retry or explain
            "read back " + landedPlain + " - " + all.matched.joinIntoString (", "));
}

// ---- ITEM 1 (14 Oct 2026): ONE CROSS-CHANNEL GATE ----------------------------------------------------
// Three ops needed it (set_rack_out, build, every edit op) and two had their own copy. The copies agreed
// today, which is exactly why a third would have been written from memory rather than from this one.
bool ExecutorDo::resolveTargetWithConfirm (const ToolCall& call, Done& done, juce::String& applyTo)
{
    const auto asked = resolveChannelArg (call);
    applyTo = this->target();
    if (asked.isEmpty() || asked == applyTo) return true;

    juce::String askedName;
    const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();
    for (const auto& t : tracks) if (t.uid == asked) askedName = t.name;
    if (askedName.isEmpty())
    {
        done (ToolOutcome::failureFor ("unknown_channel",
                                    "\"" + asked + "\" is not a channel I can reach." + knownChannelsSentence(),
                                    "I could not find a channel called \"" + asked + "\"."));
        return false;
    }
    if (! argFlag (call, "confirm"))
    {
        // ITEM 3: the model is told how to proceed; the user is asked a question. Same gate, two audiences.
        done (ToolOutcome::failureFor ("needs_confirmation",
                                    "This is on " + askedName + "'s channel, not the one you have open. "
                                    "Ask the user: \"This is on " + askedName + "'s channel, go ahead?\" - and if "
                                    "they say yes, repeat this same call with \"confirm\": true. Nothing has been "
                                    "changed yet.",
                                    "This is on " + askedName + "'s channel, not the one you have open."));
        return false;
    }
    applyTo = asked;
    return true;
}

// ---- ITEM 1: THE LANDING IS A READ-BACK, AND IT IS GIVEN TIME -----------------------------------------
//
// Sean approved a build for the Kick, was told it applied, and the Kick's Link had nothing on it; the agent
// then read that empty rack for 18 rounds. The first half of the fix is that a dispatch is not a landing.
// The second half is that a landing is not INSTANT either: a build instantiates plugins, so the first read
// after the ack can honestly show nothing, and failing on that first read would be the same false sentence
// with the sign flipped. So it polls, bounded, and whichever way it ends it says what it SAW.
//
// The clock comes from sinks_.afterMs rather than a juce::Timer inside this object, so a guard drives it and
// the poll is provable. With no clock supplied it reads once - correct for a synchronous fake, and the only
// behaviour an older caller could have expected.
void ExecutorDo::confirmLanded (const juce::String& applyTo, int wantSlots, const juce::String& opName,
                                juce::String sentence, Done done, int waitedMs, const ToolCall* call)
{
    // SEAN'S CHECK 5 (15 Oct 2026): the structural read-back is only half of it. When the op carried
    // SETTINGS, the settings are read back too - separately, and never reported as applied without a
    // read. `call` is null only for a caller that has nothing to check (and the old behaviour then).
    const auto finishOk = [this, opName, done, applyTo, call] (juce::String withSentence, int slotsOnRack)
    {
        if (call != nullptr)
        { confirmSettings (*call, applyTo, opName, slotsOnRack, withSentence, done); return; }
        auto* r = new juce::DynamicObject();
        r->setProperty ("applied", true);
        r->setProperty ("op", opName);
        // ONE UNDO TOKEN PER `do`, because one do is one undo step. The token is the op's own call id, which
        // the server already has, so an agent can undo exactly the thing it asked for and nothing else.
        done (ToolOutcome::success (juce::var (r),
                                    /*undoToken*/ opName + ":" + juce::String (juce::Time::currentTimeMillis()),
                                    withSentence));
    };

    if (wantSlots < 0) { finishOk (sentence, -1); return; }      // nothing structural to count

    const auto back = applyTo.isNotEmpty() ? (src_.linkRack ? src_.linkRack (applyTo) : RackRead())
                                           : (src_.ownRack  ? src_.ownRack()          : RackRead());
    const int have = back.valid ? (int) back.slots.size() : -1;
    if (back.valid && have >= wantSlots)
    {
        finishOk (sentence.isNotEmpty() ? sentence + "; read back " + juce::String (have) + " slot(s) on the rack"
                                        : "read back " + juce::String (have) + " slot(s) on the rack", have);
        return;
    }

    const bool canWait = sinks_.afterMs != nullptr && waitedMs + kLandPollMs <= kLandWaitMaxMs;
    if (canWait)
    {
        const auto aliveW = this->alive();
        // `call` is COPIED into the poll (not the pointer): the poll outlives doOp's frame, and a
        // pointer to a caller's ToolCall is the dangling read fix 1(b) already recorded once.
        auto callCopy = call != nullptr ? std::make_shared<ToolCall> (*call) : std::shared_ptr<ToolCall>();
        sinks_.afterMs (kLandPollMs, [this, aliveW, applyTo, wantSlots, opName, sentence, done, waitedMs, callCopy]
        {
            // The window can close while a build is still landing. Nothing is claimed in that case: the turn
            // is over and there is no rack read to be had from a dead editor.
            if (aliveW.expired()) { done (ToolOutcome::success (juce::var (new juce::DynamicObject()), {}, sentence)); return; }
            confirmLanded (applyTo, wantSlots, opName, sentence, done, waitedMs + kLandPollMs, callCopy.get());
        });
        return;
    }

    // LOUD, AND IT SAYS WHAT IT SAW, including how long it waited - so the next round does not plan against
    // a rack that is not there, and so "it needs longer" can be told apart from "it never arrived".
    done (ToolOutcome::failureFor ("not_applied",
                                "the rack acknowledged the change (" + sentence + ") but a read-back of "
                                + (applyTo.isNotEmpty() ? "that Link" : juce::String ("this rack"))
                                + (waitedMs > 0 ? ", after " + juce::String (waitedMs) + " ms, " : " ")
                                + "shows " + (have < 0 ? juce::String ("a rack that cannot be read")
                                                       : juce::String (have) + " slot(s)")
                                + " where " + juce::String (wantSlots) + " were expected, so the change did NOT "
                                "land. Do not plan against this rack until a look confirms what is on it.",
                                "That change did not land - the rack still shows "
                                + (have < 0 ? juce::String ("nothing I can read")
                                            : juce::String (have) + " slot(s)") + "."));
}

// How many slots the target must hold after this op for it to have landed, or -1 when the op does not change
// the rack's shape and there is nothing to count. Deliberately a FLOOR, not an equality, for every op but
// build: an add puts one more slot on whatever was there, and this executor does not get to assume it knew the
// count beforehand (it may not have looked). build is the exception because the chain block IS the whole rack.
int ExecutorDo::structuralWantSlots (const ToolCall& call, const juce::String& opName) const
{
    if (opName == "build")
    {
        const auto chain = argVar (call, "chain");
        if (auto* arr = chain.getArray()) return juce::jmax (1, arr->size());
        return 1;
    }
    if (opName == "add" || opName == "replace") return 1;   // at least something is on the rack afterwards
    return -1;
}

void ExecutorDo::undoStep (const juce::String& token, Done done)
{
    if (! (sinks_.agentModeOn && sinks_.agentModeOn()))
    { done (ToolOutcome::failure ("not_permitted", "Agent mode is off, so nothing was changed and there is "
                                                   "nothing to undo.")); return; }
    if (! sinks_.undoOne)
    { done (ToolOutcome::failure ("not_in_phase", "this build cannot undo an agent step")); return; }
    sinks_.undoOne (token, [done] (bool ok, juce::String sentence)
    {
        if (! ok) { done (ToolOutcome::failure ("not_applied",
                                                sentence.isNotEmpty() ? sentence
                                                : juce::String ("that step could not be undone"))); return; }
        auto* r = new juce::DynamicObject(); r->setProperty ("undone", true);
        done (ToolOutcome::success (juce::var (r), {}, sentence));
    });
}

// ---- AUTO MODE'S PRE-FLIGHT, AND THE UNDO TOKEN'S RULE, IN ONE PLACE -----------------------------------
//
// Sean's Auto ruling: "every step applies without waiting; the run stops only for a question, or for any
// step that cannot be undone (still ask-first, with the reason on the line)". That needs undoability known
// BEFORE a step runs, where today it is only known after (Step::canUndo() is "it finished and it handed back
// a token"). Two lists would drift, so this is the only one, and agent_round16_guard holds it against the
// tokens the ops really return.
//
// The op names are this executor's own dispatch (translate(), above): set, set_wet, set_io, set_master_wet,
// set_pre_gain, bypass, remove, move, add, replace, build, set_level, open_editor - plus `undo`, which is
// routed before translate.
bool ExecutorDo::looksLikeUndoGoal (const juce::String& goal)
{
    const auto g = " " + goal.trim().toLowerCase() + " ";
    return g.contains (" undo ") || g.contains ("undo that") || g.contains ("put that back")
        || g.contains ("put it back") || g.contains (" revert ") || g.contains ("revert that");
}

Undoability ExecutorDo::undoabilityOf (const ToolCall& call, const juce::String& goal)
{
    Undoability u;
    // A CALL THAT CHANGES NOTHING NEEDS NO UNDO. read / look / check / talk only answer questions, and a
    // question that waited for an approval would be an approval for nothing.
    if (call.name != "do")
    {
        u.undoable = true;
        return u;
    }
    const auto op = argString (call, "op").trim().toLowerCase();

    // Every one of these pushes an entry on the plugin-wide history and returns a token from confirmLanded /
    // confirmValues, so the history can put it back.
    static const char* kUndoable[] = { "set", "set_wet", "set_io", "set_master_wet", "set_pre_gain",
                                       "bypass", "remove", "move", "add", "replace", "build", "set_level" };
    for (auto* o : kUndoable)
        if (op == o) { u.undoable = true; return u; }

    if (op == "undo")
    {
        // THE ONE CARVE-OUT, AND THE WHOLE REASON IT IS SAFE. An undo returns no token (undoStep answers
        // success with {}), because undoing an undo is a redo and there is no redo op before beta. Taken
        // literally, Auto would stop and ask before an undo the user had just asked for out loud - so
        // Sean ruled: "'undo that' typed by the user runs without a second approval". The USER'S WORDS are
        // the approval, which is why this reads the goal and not the op alone; and B refuses every other op
        // on an undo goal, so the carve-out cannot be widened by a model.
        u.undoable = false;
        if (looksLikeUndoGoal (goal))
        {
            u.preApprovedInAuto = true;
            u.whyNot = "you asked for this to be undone, so it runs without a second approval";
        }
        else
            u.whyNot = "an undo cannot itself be undone (there is no redo yet)";
        return u;
    }

    if (op == "open_editor")
    {
        // It opens a window and changes no audio. Nothing to put back, nothing to ask about.
        u.preApprovedInAuto = true;
        u.whyNot = "it only opens a window";
        return u;
    }

    // THE FALLBACK IS THE SAFE ANSWER, NOT THE CONVENIENT ONE - the dialCapable convention, and the reason
    // it exists: an op this executor does not know might be anything, so Auto asks.
    u.whyNot = "I do not have a way to put \"" + (op.isNotEmpty() ? op : juce::String ("that")) + "\" back";
    return u;
}

void ExecutorDo::undoToCheckpoint (const juce::String& token, Done done)
{
    // SAME PATH, DIFFERENT TOKEN. A checkpoint token names a point rather than a step, and the sink is the
    // only thing that knows how to walk back to one - so it is handed through rather than interpreted here.
    undoStep (token, std::move (done));
}

} // namespace echojay::agent
