#include "EJAgentExecutorDo.h"

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
        sinks_.buildChain (buildTo, chain, [this, aliveB, done, buildTo, wantSlots] (bool ok, juce::String sentence)
        {
            if (! ok)
            {
                done (ToolOutcome::failure ("not_applied", sentence.isNotEmpty() ? sentence
                                                        : juce::String ("the build was not sent to the rack")));
                return;
            }
            if (aliveB.expired())
            { done (ToolOutcome::success (juce::var (new juce::DynamicObject()), {}, sentence)); return; }
            confirmLanded (buildTo, wantSlots, "build", sentence, done, 0);
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
    sinks_.applyChainOps (applyTo, edits, [this, aliveW, done, opName, applyTo, wantSlots, call] (bool ok, juce::String sentence)
    {
        if (! ok)
        {
            done (ToolOutcome::failure ("not_applied",
                                        sentence.isNotEmpty() ? sentence
                                                              : juce::String ("that change was not applied")));
            return;
        }
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
        confirmLanded (applyTo, wantSlots, opName, sentence, done, 0);
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
        if (auto* p = root->getProperty ("params").getDynamicObject()) asked = p;
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

    juce::StringArray matched, missed, unreadable;
    for (auto& kv : asked->getProperties())
    {
        const auto id = kv.name.toString();
        const auto want = kv.value;
        if (! (want.isDouble() || want.isInt() || want.isInt64())) continue;   // a mode name, not a number
        const double wanted = (double) want;
        double got = 0.0;
        if (! src_.slotParam (applyTo, slot1, id, got)) { unreadable.add (id); continue; }
        const double tol = juce::jmax (0.05, std::abs (wanted) * 0.02);
        if (std::abs (got - wanted) <= tol) matched.add (id + " " + juce::String (got, 2));
        else missed.add (id + " is " + juce::String (got, 2) + " (asked " + juce::String (wanted, 2) + ")");
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
        auto o = ToolOutcome::failure ("not_applied", why);
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
    done (ToolOutcome::success (juce::var (r), "set:" + juce::String (juce::Time::currentTimeMillis()),
                                "read back: " + matched.joinIntoString (", ")));
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
                                juce::String sentence, Done done, int waitedMs)
{
    const auto finishOk = [this, opName, done] (juce::String withSentence)
    {
        auto* r = new juce::DynamicObject();
        r->setProperty ("applied", true);
        r->setProperty ("op", opName);
        // ONE UNDO TOKEN PER `do`, because one do is one undo step. The token is the op's own call id, which
        // the server already has, so an agent can undo exactly the thing it asked for and nothing else.
        done (ToolOutcome::success (juce::var (r),
                                    /*undoToken*/ opName + ":" + juce::String (juce::Time::currentTimeMillis()),
                                    withSentence));
    };

    if (wantSlots < 0) { finishOk (sentence); return; }      // nothing structural to count

    const auto back = applyTo.isNotEmpty() ? (src_.linkRack ? src_.linkRack (applyTo) : RackRead())
                                           : (src_.ownRack  ? src_.ownRack()          : RackRead());
    const int have = back.valid ? (int) back.slots.size() : -1;
    if (back.valid && have >= wantSlots)
    {
        finishOk (sentence.isNotEmpty() ? sentence + "; read back " + juce::String (have) + " slot(s) on the rack"
                                        : "read back " + juce::String (have) + " slot(s) on the rack");
        return;
    }

    const bool canWait = sinks_.afterMs != nullptr && waitedMs + kLandPollMs <= kLandWaitMaxMs;
    if (canWait)
    {
        const auto aliveW = this->alive();
        sinks_.afterMs (kLandPollMs, [this, aliveW, applyTo, wantSlots, opName, sentence, done, waitedMs]
        {
            // The window can close while a build is still landing. Nothing is claimed in that case: the turn
            // is over and there is no rack read to be had from a dead editor.
            if (aliveW.expired()) { done (ToolOutcome::success (juce::var (new juce::DynamicObject()), {}, sentence)); return; }
            confirmLanded (applyTo, wantSlots, opName, sentence, done, waitedMs + kLandPollMs);
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
