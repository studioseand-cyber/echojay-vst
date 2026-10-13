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
        bool p = false; const double pct = argNum (call, "pct", p);
        if (! p) { refusal = "a set_wet needs a pct from 0 to 100"; return {}; }
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
        bool p = false; const bool on = argBool (call, "on", p);
        if (! p) { refusal = "a bypass needs on: true or false"; return {}; }
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
    }
    else if (op == "build")
    {
        // The chain block, as the build path already takes it: one ack, one undo step. Passed through whole
        // rather than re-shaped here, because the block's schema is the server's and this executor is not the
        // place to learn it a second time.
        const auto chain = argVar (call, "chain");
        if (chain.getArray() == nullptr) { refusal = "a build needs a chain array"; return {}; }
        auto* o = addOp ("build");
        o->setProperty ("chain", chain);
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
                  "open_editor.";
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
    if (! sinks_.applyChainOps)
    {
        done (ToolOutcome::failure ("not_in_phase", "this build has no way to send that change to a rack"));
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

        const auto askedOut = resolveChannelArg (call);
        juce::String outTo = this->target();
        if (askedOut.isNotEmpty() && askedOut != outTo)
        {
            juce::String nm;
            const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();
            for (const auto& t : tracks) if (t.uid == askedOut) nm = t.name;
            if (nm.isEmpty())
            { done (ToolOutcome::failureFor ("unknown_channel", "\"" + askedOut + "\" is not a channel I can reach." + knownChannelsSentence(),
                                             "I could not find a channel called \"" + askedOut + "\".")); return; }
            if (! argFlag (call, "confirm"))
            { done (ToolOutcome::failureFor ("needs_confirmation",
                                          "This is on " + nm + "'s channel, not the one you have open. Ask the user: "
                                          "\"This is on " + nm + "'s channel, go ahead?\" - and if they say yes, repeat "
                                          "this same call with \"confirm\": true. Nothing has been changed yet.",
                                          "This is on " + nm + "'s channel, not the one you have open.")); return; }
            outTo = askedOut;
        }
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
    const auto asked   = resolveChannelArg (call);
    const auto current = this->target();
    juce::String applyTo = current;
    if (asked.isNotEmpty() && asked != current)
    {
        // Resolution already tried uid, exact name and a unique containment match. Still not a known channel
        // means the model named something that does not exist - say so WITH the list, so the loop stops.
        juce::String askedName;
        const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();
        for (const auto& t : tracks) if (t.uid == asked) askedName = t.name;
        if (askedName.isEmpty())
        {
            done (ToolOutcome::failureFor ("unknown_channel",
                                        "\"" + asked + "\" is not a channel I can reach." + knownChannelsSentence(),
                                        "I could not find a channel called \"" + asked + "\"."));
            return;
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
            return;
        }
        applyTo = asked;
    }

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
    sinks_.applyChainOps (applyTo, edits, [this, aliveW, done, opName, applyTo, wantSlots] (bool ok, juce::String sentence)
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
        if (wantSlots >= 0)
        {
            const auto back = applyTo.isNotEmpty() ? (src_.linkRack ? src_.linkRack (applyTo) : RackRead())
                                                   : (src_.ownRack  ? src_.ownRack()          : RackRead());
            const int have = back.valid ? (int) back.slots.size() : -1;
            if (! back.valid || have < wantSlots)
            {
                // LOUD, AND IT SAYS WHAT IT SAW. The point is that the next round must not plan against a rack
                // that is not there, so the sentence carries the figure the readback actually returned.
                done (ToolOutcome::failureFor ("not_applied",
                                            "the rack acknowledged the change (" + sentence + ") but a read-back of "
                                            + (applyTo.isNotEmpty() ? "that Link" : juce::String ("this rack"))
                                            + " shows " + (have < 0 ? juce::String ("a rack that cannot be read")
                                                                    : juce::String (have) + " slot(s)")
                                            + " where " + juce::String (wantSlots) + " were expected, so the change did "
                                            "NOT land. Do not plan against this rack until a look confirms what is on it.",
                                            "That change did not land - the rack still shows "
                                            + (have < 0 ? juce::String ("nothing I can read")
                                                        : juce::String (have) + " slot(s)") + "."));
                return;
            }
            sentence = sentence.isNotEmpty() ? sentence + "; read back " + juce::String (have) + " slot(s) on the rack"
                                             : "read back " + juce::String (have) + " slot(s) on the rack";
        }
        auto* r = new juce::DynamicObject();
        r->setProperty ("applied", true);
        r->setProperty ("op", opName);
        // ONE UNDO TOKEN PER `do`, because one do is one undo step. The token is the op's own call id, which
        // the server already has, so an agent can undo exactly the thing it asked for and nothing else.
        done (ToolOutcome::success (juce::var (r), /*undoToken*/ opName + ":" + juce::String (juce::Time::currentTimeMillis()),
                                    sentence));
    });
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

void ExecutorDo::undoToCheckpoint (const juce::String& token, Done done)
{
    // SAME PATH, DIFFERENT TOKEN. A checkpoint token names a point rather than a step, and the sink is the
    // only thing that knows how to walk back to one - so it is handed through rather than interpreted here.
    undoStep (token, std::move (done));
}

} // namespace echojay::agent
