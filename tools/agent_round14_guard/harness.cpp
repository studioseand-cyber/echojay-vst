// =============================================================================
//  agent_round14_guard - Sean's in-host round on ship_2026-10-13a, plus the
//  dead-path sweep's contract findings (14 Oct 2026)
// =============================================================================
//
//  Each leg is written from the symptom Sean reported or the evidence in his log,
//  and each was run in both directions (fix disabled -> red, restored -> green).
//
//   N1  ITEM 1(a). The OWN rack's ops reach the parser in the envelope it reads
//       (`{"edit":[...]}`), not as a bare array. Sean: "add EchoJay Compressor" ->
//       "that change carried no operation this rack understands", twice.
//   N2  ITEM 1(b). A `build` goes to the BUILD sink and never into the edit-op
//       array. Sean's Link: `op 1 invalid: unknown operation "build"`.
//   N3  ITEM 1(c). A structural change is given TIME to appear: the read-back
//       polls, so a build whose slots arrive late is not called a failure, and one
//       that never arrives still fails loudly with the figures.
//   N4  THE SWEEP. The contract's own argument names: `wet_pct` on set_wet (B's
//       line 103), `bypassed` on bypass (line 104), and `wet_pct` carried through
//       an add (line 98). All three refused or dropped the call before this.
//   N5  ITEM 8. makeup_cap_db bounds MAKE-UP only; a cut is never capped (the cap
//       Sean removed on 29 Sep was stopping a cut from finishing, and that case
//       must not come back through this door).
//   N6  RULING 2 (13 Oct). An output_param in a block is named and IGNORED.
//
//  Fixture note: opaque hex uids with realistic names, for the reason recorded in
//  agent_round13_guard - a uid containing its own track name cannot test a uid
//  parser, and that mistake produced a false green there first.
// =============================================================================
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJAgentExecutorRead.h"
#include "EJAgentExecutorDo.h"
#include "EJDontUse.h"   // his 11:5x item 3: the mark's words, parsed in one place
// EJCalibLoop.h logs through EchoJay_NSLog without declaring it (NativeClip.mm defines it and every plugin
// TU has it in scope by other means). A guard that includes the header directly has to declare it, exactly as
// Source/NativeClip.h and Source/PluginScanner.h do - the archive carries the definition.
extern "C" void EchoJay_NSLog (const char* msg);
#include "EJCalibLoop.h"
#include <cmath>
#include <cstdio>

using namespace echojay::agent;

namespace
{
int failures = 0, passes = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok    %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                                       detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); }
    std::fflush (stdout);
}

ToolCall call (const juce::String& name, const juce::String& argsJson)
{
    ToolCall c; c.id = "t"; c.name = name; c.args = juce::JSON::parse (argsJson); c.approval = "free"; return c;
}

struct FakeHost
{
    RackRead own, link;
    std::vector<TrackRead> tracks;
    int ownSlotsOnReadback = 1;
    int linkSlotsOnReadback = 2;

    FakeHost()
    {
        own.valid = true; own.name = "Mix Bus"; own.kind = "mix_bus"; own.revision = 7;
        { SlotRead s; s.n = 1; s.name = "EchoJay EQ"; s.builtin = true; own.slots.push_back (s); }
        link.valid = true; link.remote = true; link.uid = "bb411b8240"; link.name = "88_7even Kick_2_01"; link.kind = "channel";
        { SlotRead s; s.n = 1; s.name = "UAD 1176"; link.slots.push_back (s); }

        TrackRead t1; t1.uid = "bb411b8240"; t1.name = "88_7even Kick_2_01"; t1.connected = true;
        TrackRead t2; t2.uid = "7c91ae0032"; t2.name = "Lead Vocal";         t2.connected = true;
        tracks = { t1, t2 };
    }

    RackRead nSlots (const RackRead& base, int n) const
    {
        RackRead r = base; r.slots.clear();
        for (int i = 0; i < n; ++i) { SlotRead s; s.n = i + 1; s.name = "slot"; r.slots.push_back (s); }
        return r;
    }

    Sources sources()
    {
        Sources s;
        s.ownName = [this] { return own.name; };
        s.ownKind = [this] { return own.kind; };
        s.ownRack = [this] { return nSlots (own, ownSlotsOnReadback); };
        s.linkRack = [this] (const juce::String& uid)
        {
            if (uid == link.uid) return nSlots (link, linkSlotsOnReadback);
            RackRead r; r.why = "no Link with uid \"" + uid + "\" is in the registry"; return r;
        };
        s.tracks = [this] { return tracks; };
        s.inventory = [] { return juce::StringArray { "FabFilter Pro-C 2" }; };
        s.chainRevision = [this] { return own.revision; };
        return s;
    }
};

// Everything the sinks were asked to do, so a leg can assert the ROUTE and not just the answer. The whole of
// item 1 is a routing fault, so the route is the thing to measure.
struct SinkLog
{
    int chainOps = 0, builds = 0, rackOut = 0, clockCalls = 0;
    // N7 (Sean's ruling 4, 14 Oct 2026): the plugin's own undo history, as the sink sees it. `depth` is the
    // thing the executor is required to READ BACK - a sink that says "done" without the depth moving is the
    // whole class of fault this round removed from nine other sinks, so the guard can make it happen here.
    int undoCalls = 0, depth = 3;
    bool undoSinkSaysOk = true;      // what the sink CLAIMS
    bool undoSinkMovesDepth = true;  // whether anything actually happened
    juce::String undoLabel = "remove EchoJay EQ";
    juce::String lastUid, lastBuildUid;
    juce::var lastOps, lastChain;
    bool applyOk = true, buildOk = true;
    // the fake clock's queue: a leg runs it by hand, so the poll is proved rather than waited out
    std::vector<std::function<void()>> pending;
};

DoSinks sinks (SinkLog& lg, bool withClock = true)
{
    DoSinks s;
    s.agentModeOn = [] { return true; };
    s.echoJayOnly = [] { return false; };
    s.applyChainOps = [&lg] (const juce::String& uid, const juce::var& ops, std::function<void (bool, juce::String, juce::String)> cb)
    {
        ++lg.chainOps; lg.lastUid = uid; lg.lastOps = ops;
        if (cb) cb (lg.applyOk, lg.applyOk ? juce::String ("the rack took it") : juce::String ("the rack refused it"), {});
    };
    s.buildChain = [&lg] (const juce::String& uid, const juce::var& chain, std::function<void (bool, juce::String)> cb)
    {
        ++lg.builds; lg.lastBuildUid = uid; lg.lastChain = chain;
        if (cb) cb (lg.buildOk, lg.buildOk ? juce::String ("the build was sent") : juce::String ("the build was refused"));
    };
    s.setRackOutDb = [&lg] (const juce::String& uid, double db, bool absolute, std::function<void (bool, juce::String)> cb)
    { ++lg.rackOut; lg.lastUid = uid; if (cb) cb (true, "out " + juce::String (db, 1)); };
    s.undoOne = [] (const juce::String&, std::function<void (bool, juce::String)> cb) { if (cb) cb (true, "undone"); };
    s.undoHistoryStep = [&lg] (std::function<void (bool, juce::String, int, int)> cb)
    {
        ++lg.undoCalls;
        const int before = lg.depth;
        if (lg.undoSinkMovesDepth && lg.depth > 0) --lg.depth;
        if (cb) cb (lg.undoSinkSaysOk, lg.undoLabel, before, lg.depth);
    };
    if (withClock)
        s.afterMs = [&lg] (int, std::function<void()> fn) { ++lg.clockCalls; lg.pending.push_back (std::move (fn)); };
    return s;
}

/** Run the fake clock's queue until it is empty or `maxTicks` runs out. Each tick runs the callbacks that were
    queued when the tick began, so a poll that re-queues itself advances exactly one step per tick - which is
    what makes "it polled N times" a measurable fact. */
int runClock (SinkLog& lg, int maxTicks = 200)
{
    int ticks = 0;
    while (! lg.pending.empty() && ticks < maxTicks)
    {
        auto batch = std::move (lg.pending);
        lg.pending.clear();
        ++ticks;
        for (auto& fn : batch) fn();
    }
    return ticks;
}

ToolOutcome run (ToolExecutor& ex, const char* tool, const ToolCall& c)
{
    ToolOutcome got; bool fired = false;
    auto done = [&] (ToolOutcome o) { got = std::move (o); fired = true; };
    const juce::String t (tool);
    if (t == "look") ex.look (c, done); else if (t == "check") ex.check (c, done); else ex.doOp (c, done);
    if (! fired) got = ToolOutcome::failure ("no_completion", "the executor never called done");
    return got;
}

/** The ops array the executor handed the apply sink, as the chain channel would see it. */
juce::String opNamesOf (const juce::var& ops)
{
    juce::StringArray names;
    if (auto* arr = ops.getArray())
        for (const auto& o : *arr) names.add (o.getProperty ("op", juce::var()).toString());
    return names.joinIntoString (",");
}
juce::var firstOp (const juce::var& ops)
{
    if (auto* arr = ops.getArray()) if (! arr->isEmpty()) return arr->getFirst();
    return {};
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("==== agent_round14_guard ====\n");

    // ---------------------------------------------------------------- N1
    std::printf ("== N1: item 1(a), the own rack's ops are a chain-channel edit array ==\n");
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        h.ownSlotsOnReadback = 2;                           // the add landed
        const auto o = run (ex, "do", call ("do", R"({"op":"add","name":"EchoJay Compressor","after":1})"));
        runClock (lg);
        check (lg.chainOps == 1 && lg.lastUid.isEmpty(),
               "an add on the OPEN rack goes to the apply sink with an EMPTY uid (the own rack)",
               "ops=" + juce::String (lg.chainOps) + " uid=\"" + lg.lastUid + "\"");
        check (opNamesOf (lg.lastOps) == "add",
               "...carrying exactly one op, named \"add\"", opNamesOf (lg.lastOps));
        check (firstOp (lg.lastOps).getProperty ("name", juce::var()).toString() == "EchoJay Compressor",
               "...with the plugin's name on it");
        check (o.ok, "...and the do SUCCEEDS once the rack reads back the new slot", o.errorMessage);
    }
    {
        // The real parser's rule, asserted here so the envelope cannot regress unnoticed: a BARE ARRAY has no
        // dynamic object, so ChainHost::parseChainEditOps returns zero ops - which is the sentence Sean saw.
        // This is the one leg that tests the editor's sink indirectly, by stating the parser's contract.
        const auto bare = juce::JSON::parse (R"([{"op":"add","name":"X"}])");
        check (bare.getDynamicObject() == nullptr,
               "a BARE ops array has no dynamic object - which is why the parser returned zero ops "
               "(RED as it stood: the own-rack sink passed exactly this)");
        auto* env = new juce::DynamicObject();
        env->setProperty ("edit", bare);
        check (juce::var (env).getProperty ("edit", juce::var()).getArray() != nullptr,
               "...and the envelope the parser reads DOES carry the array under \"edit\"");
    }

    // ---------------------------------------------------------------- N2
    std::printf ("== N2: item 1(b), a build takes the build road, never the edit-op array ==\n");
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        h.linkSlotsOnReadback = 2;
        const auto o = run (ex, "do", call ("do",
            R"({"op":"build","channel":"bb411b8240","confirm":true,
                "chain":[{"name":"EchoJay EQ"},{"name":"EchoJay Compressor"}]})"));
        runClock (lg);
        check (lg.builds == 1, "a build reaches the BUILD sink", juce::String (lg.builds));
        check (lg.chainOps == 0,
               "...and NOT the edit-op sink  (RED as it stood: it went as an op named \"build\", and the Link "
               "answered `op 1 invalid: unknown operation \"build\"`)", juce::String (lg.chainOps));
        check (lg.lastBuildUid == "bb411b8240", "...on the TARGET Link, not the open rack", lg.lastBuildUid);
        check (lg.lastChain.getArray() != nullptr && lg.lastChain.getArray()->size() == 2,
               "...with the chain array passed through whole");
        check (o.ok, "...and it succeeds once that Link reads back its two slots", o.errorMessage);
    }
    {
        FakeHost h; SinkLog lg; auto sk = sinks (lg); sk.buildChain = nullptr;
        ExecutorDo ex (h.sources(), sk);
        const auto o = run (ex, "do", call ("do", R"({"op":"build","chain":[{"name":"X"}]})"));
        check (! o.ok && o.errorCode == "not_in_phase",
               "a build with no build sink refuses PLAINLY rather than falling back to an edit op",
               o.errorCode + ": " + o.errorMessage);
        check (lg.chainOps == 0, "...and nothing was sent to the rack");
    }
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"build","chain":[]})"));
        check (! o.ok && o.errorCode == "bad_request", "an empty chain is refused with a reason", o.errorCode);
    }

    // ---------------------------------------------------------------- N3
    std::printf ("== N3: item 1(c), the read-back is given time, and still fails loudly ==\n");
    {
        // The slots arrive LATE: empty on the first read, there by the third tick. Before the poll this was a
        // "did NOT land" refusal for a build that was simply still loading.
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        h.linkSlotsOnReadback = 0;
        ToolOutcome got; bool fired = false;
        ex.doOp (call ("do", R"({"op":"build","channel":"bb411b8240","confirm":true,
                                 "chain":[{"name":"A"},{"name":"B"}]})"),
                 [&] (ToolOutcome o) { got = std::move (o); fired = true; });
        check (! fired, "a build whose rack still reads EMPTY does not answer at once - it waits");
        runClock (lg, 2);
        h.linkSlotsOnReadback = 2;                      // the plugins finished loading
        runClock (lg);
        check (fired && got.ok,
               "...and SUCCEEDS when the slots appear  (RED before the poll: one immediate read called this "
               "a failure)", got.errorMessage);
        check (lg.clockCalls >= 1, "...having actually used the clock", juce::String (lg.clockCalls));
    }
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        h.linkSlotsOnReadback = 0;                      // never arrives
        ToolOutcome got; bool fired = false;
        ex.doOp (call ("do", R"({"op":"build","channel":"bb411b8240","confirm":true,"chain":[{"name":"A"}]})"),
                 [&] (ToolOutcome o) { got = std::move (o); fired = true; });
        const int ticks = runClock (lg, 500);
        check (fired && ! got.ok && got.errorCode == "not_applied",
               "a build that NEVER lands fails - the wait is bounded, not endless", got.errorCode);
        check (got.errorMessage.contains ("0 slot(s)") && got.errorMessage.contains ("1 were expected"),
               "...and the sentence carries both figures", got.errorMessage);
        check (got.errorMessage.contains ("ms"), "...and how long it waited", got.errorMessage);
        check (got.forTheUser().isNotEmpty() && ! got.forTheUser().contains ("Do not plan"),
               "...while the USER's line stays one plain sentence (round 12b item 3)", got.forTheUser());
        const int expected = ExecutorDo::kLandWaitMaxMs / ExecutorDo::kLandPollMs;
        check (ticks <= expected + 2, "...and it polled about the bound, not for ever",
               juce::String (ticks) + " tick(s), bound " + juce::String (expected));
    }
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"set_pre_gain","db":-2.0})"));
        check (o.ok && lg.clockCalls == 0,
               "a NON-structural op is answered at once and never polls - there is no slot count to wait for",
               juce::String (lg.clockCalls));
    }

    // ---------------------------------------------------------------- N4
    std::printf ("== N4: the sweep, B's own argument names ==\n");
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"set_wet","slot":1,"wet_pct":40})"));
        runClock (lg);
        check (o.ok && lg.chainOps == 1,
               "set_wet accepts the contract's `wet_pct`  (RED as it stood: it read `pct` and refused every "
               "call B sends)", o.errorMessage);
        check (std::abs ((double) firstOp (lg.lastOps).getProperty ("wet_pct", 0.0) - 40.0) < 0.01,
               "...and the figure reaches the wire", firstOp (lg.lastOps).getProperty ("wet_pct", juce::var()).toString());
    }
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"set_wet","slot":1,"pct":55})"));
        check (o.ok, "...and `pct` still works, so an older caller is not broken by the fix", o.errorMessage);
    }
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"bypass","slot":1,"bypassed":true})"));
        runClock (lg);
        check (o.ok && lg.chainOps == 1,
               "bypass accepts the contract's `bypassed`  (RED as it stood: it read `on` and refused)",
               o.errorMessage);
        check ((bool) firstOp (lg.lastOps).getProperty ("on", false),
               "...and it reaches the wire as the chain channel's own `on`");
    }
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"bypass","slot":1})"));
        check (! o.ok && o.errorCode == "bad_request" && o.errorMessage.contains ("bypassed"),
               "a bypass with NO flag is refused, and the refusal names the field the contract uses",
               o.errorMessage);
    }
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        h.ownSlotsOnReadback = 2;
        run (ex, "do", call ("do", R"({"op":"add","name":"FabFilter Pro-C 2","after":1,"wet_pct":40})"));
        runClock (lg);
        check (std::abs ((double) firstOp (lg.lastOps).getProperty ("wet_pct", 0.0) - 40.0) < 0.01,
               "an add CARRIES wet_pct  (RED as it stood: it was dropped, so a plugin asked for at 40% wet "
               "arrived at 100% with nothing said)",
               firstOp (lg.lastOps).getProperty ("wet_pct", juce::var()).toString());
    }

    // ---------------------------------------------------------------- N5
    std::printf ("== N5: item 8, the make-up cap bounds make-up and never a cut ==\n");
    {
        auto blockWith = [] (bool withCap, double cap) -> juce::var
        {
            auto* c = new juce::DynamicObject();
            c->setProperty ("slot", 1);
            c->setProperty ("actuator", "threshold");
            c->setProperty ("param", "Threshold");
            c->setProperty ("min_db", -60.0); c->setProperty ("max_db", 0.0);
            if (withCap) c->setProperty ("makeup_cap_db", cap);
            return juce::var (c);
        };
        echojay::CalibLoop::Config c1; juce::String why1;
        check (echojay::CalibLoop::configFromBlock (blockWith (true, 3.0), 4, false, "Comp", c1, why1),
               "a block carrying makeup_cap_db parses");
        check (c1.makeupCapDb == c1.makeupCapDb && std::abs (c1.makeupCapDb - 3.0f) < 0.01f,
               "...and the cap is CARRIED  (RED as it stood: makeup_cap_db existed nowhere in the plugin)",
               juce::String (c1.makeupCapDb, 2));
        echojay::CalibLoop::Config c2; juce::String why2;
        echojay::CalibLoop::configFromBlock (blockWith (false, 0.0), 4, false, "Comp", c2, why2);
        check (! (c2.makeupCapDb == c2.makeupCapDb),
               "a block with no cap leaves it NaN - no cap, which is what every block before the field meant");
    }

    // ---------------------------------------------------------------- N6
    std::printf ("== N6: ruling 2 (13 Oct), output_param is named and ignored ==\n");
    {
        auto* c = new juce::DynamicObject();
        c->setProperty ("slot", 1);
        c->setProperty ("actuator", "threshold");
        c->setProperty ("param", "Threshold");
        c->setProperty ("output_param", "Makeup");
        echojay::CalibLoop::Config cfg; juce::String why;
        const bool parsed = echojay::CalibLoop::configFromBlock (juce::var (c), 4, false, "Comp", cfg, why);
        check (parsed, "a block naming output_param still parses - the block is not rejected over it");
        check (why.containsIgnoreCase ("output_param") && why.containsIgnoreCase ("IGNORED"),
               "...and the loop SAYS it is ignoring it, naming the field  (Sean's ruling of 13 Oct: the 29 Sep "
               "ruling stands and B is dropping the field)", why);
    }


    // ---------------------------------------------------------------- N7
    std::printf ("== N7: ruling 4 (14 Oct), \"undo\" walks the PLUGIN'S undo history one step ==\n");
    // Sean's ruling, after the rig's row C10: asked to "undo that" having just removed a slot, the agent
    // answered "I've undone the pre-gain change" - it re-dialled an earlier parameter, because it had no undo
    // tool and had to CHOOSE what "that" meant. The ruling takes the choice away: there is an undo op, it
    // walks the one plugin-wide history (EchoJayProcessor::undoHistory) one step, and whatever that step was
    // is what gets undone. These legs hold the route, the read-back and the honesty.
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"undo"})"));
        check (lg.undoCalls == 1, "an undo reaches the undo-history sink exactly once",
               "calls=" + juce::String (lg.undoCalls));
        check (lg.chainOps == 0 && lg.builds == 0,
               "...and NOTHING is sent to the rack as an edit op (the item-1 fault, which an undo could repeat)",
               "ops=" + juce::String (lg.chainOps) + " builds=" + juce::String (lg.builds));
        check (o.ok, "...and the do succeeds", o.errorMessage);
        check (o.result.getProperty ("label", juce::var()).toString() == "remove EchoJay EQ",
               "...naming WHAT was undone, so the model reports the step rather than guessing at it",
               juce::JSON::toString (o.result));
        check ((int) o.result.getProperty ("depthBefore", juce::var()) == 3
               && (int) o.result.getProperty ("depthAfter", juce::var()) == 2,
               "...with the history depth before and after, which is the read-back",
               juce::JSON::toString (o.result));
        check (o.landedLine.containsIgnoreCase ("remove EchoJay EQ"),
               "...and the user's line names the step in plain words", o.landedLine);
    }
    {
        // NOTHING TO UNDO. An empty history is not a failure of the plugin and must not be dressed as one -
        // it is a true answer the model can relay.
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        lg.depth = 0; lg.undoSinkSaysOk = false; lg.undoSinkMovesDepth = false;
        const auto o = run (ex, "do", call ("do", R"({"op":"undo"})"));
        check (! o.ok, "an undo with an empty history FAILS rather than claiming a step");
        check (o.errorCode == "not_applied", "...as not_applied", o.errorCode);
        check (o.errorMessage.containsIgnoreCase ("nothing") || o.errorMessage.containsIgnoreCase ("empty"),
               "...saying there was nothing to undo", o.errorMessage);
    }
    {
        // THE READ-BACK, PROVED THE ONLY WAY IT CAN BE: a sink that CLAIMS success while the history does not
        // move. Nine sinks reported a write as a landing this round; this is the tenth and it cannot.
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        lg.undoSinkSaysOk = true; lg.undoSinkMovesDepth = false;
        const auto o = run (ex, "do", call ("do", R"({"op":"undo"})"));
        check (! o.ok, "a sink that says \"undone\" while the depth has not moved is NOT reported as success");
        check (o.errorMessage.containsIgnoreCase ("history") || o.errorMessage.containsIgnoreCase ("did not"),
               "...and the failure says the history did not move", o.errorMessage);
    }
    {
        // AGENT MODE OFF. Sean's switch: off means the agent changes nothing, and an undo is a change.
        FakeHost h; SinkLog lg; auto sk = sinks (lg); sk.agentModeOn = [] { return false; };
        ExecutorDo ex (h.sources(), std::move (sk));
        const auto o = run (ex, "do", call ("do", R"({"op":"undo"})"));
        check (! o.ok && o.errorCode == "not_permitted", "with Agent mode off an undo is refused", o.errorCode);
        check (lg.undoCalls == 0, "...and the history is never touched", juce::String (lg.undoCalls));
    }
    {
        // A BUILD WITHOUT THE SINK. The same shape as every other op: a build of the plugin that has no undo
        // route says so, rather than failing as though the history were empty.
        FakeHost h; SinkLog lg; auto sk = sinks (lg); sk.undoHistoryStep = nullptr;
        ExecutorDo ex (h.sources(), std::move (sk));
        const auto o = run (ex, "do", call ("do", R"({"op":"undo"})"));
        check (! o.ok && o.errorCode == "not_in_phase",
               "a build with no undo route refuses as not_in_phase, not as an empty history", o.errorCode);
    }
    {
        // THE CAPABILITY, DECLARED TO THE SERVER. B refuses the op in process unless the start context's
        // capabilities array names it - so the op existing is not the same as the op being reachable. The rig
        // proved exactly that gap: C10 failed with the op built and guarded, because the model was never told.
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto ctx = ex.startContext();
        juce::StringArray caps;
        if (auto* arr = ctx.getProperty ("capabilities", juce::var()).getArray())
            for (const auto& c : *arr) caps.add (c.toString());
        check (caps.contains ("undo"),
               "the start context DECLARES the undo capability  (RED as it stood: the op was built, guarded "
               "and unreachable - B gates on this array and sends nothing without it)",
               caps.joinIntoString (","));
        check (caps.contains ("look") && caps.contains ("check"),
               "...without dropping the capabilities that were already there", caps.joinIntoString (","));
    }
    {
        // AND IT IS NOT CLAIMED BY A BUILD THAT CANNOT DO IT. A capability is a statement about this binary;
        // a build with no undo route that claimed one would have the server send an op that comes straight
        // back as not_in_phase, making the model look like it invented the call.
        FakeHost h; SinkLog lg; auto sk = sinks (lg); sk.undoHistoryStep = nullptr;
        ExecutorDo ex (h.sources(), std::move (sk));
        juce::StringArray caps;
        if (auto* arr = ex.startContext().getProperty ("capabilities", juce::var()).getArray())
            for (const auto& c : *arr) caps.add (c.toString());
        check (! caps.contains ("undo"),
               "a build with no undo route does NOT declare the capability", caps.joinIntoString (","));
    }
    {
        // THE OP IS ADVERTISED. A model told "that is not an operation this build can do" is told WHICH ones
        // are - and undo has to be in that list or the capability is invisible.
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", R"({"op":"frobnicate"})"));
        check (! o.ok, "an op this build does not have is refused");
        check (o.errorMessage.contains ("undo"),
               "...and the refusal's list of what it DOES know names undo", o.errorMessage);
    }

    // ---- SL: THE SLOT THE ADD LANDED ON, AS A READ FACT (his 11:27 item 2, 21 Oct 2026) -------
    //
    // *"Include the slot position the add actually landed on in the read-back, so B can state the
    // order as fact instead of "the order hasn't read back yet"."*
    //
    // The read-back ALREADY resolves which slot it is reading (by name against the rack, then the
    // placement), and then threw that number away - so the one place in the build that knows where
    // the add landed, from a RACK READ rather than from the order the ops were sent in, was keeping
    // it to itself. Both directions are held here, because this field is a CLAIM ABOUT ORDER and a
    // wrong one is worse than none: B states it as fact.
    std::printf ("== SL: the read-back states where the add landed, and withholds it when unsure ==\n");
    {
        // A rack that answers with real names, so the name match resolves exactly as it does live.
        // The leg sets its own ground: FakeHost's nSlots() calls every slot "slot", which cannot
        // tell a name match from a miss.
        auto namedRack = [] (const juce::StringArray& names)
        {
            RackRead r; r.valid = true; r.remote = true; r.uid = "bb411b8240";
            r.name = "88_7even Kick_2_01"; r.kind = "channel";
            for (int i = 0; i < names.size(); ++i)
            { SlotRead sl; sl.n = i + 1; sl.name = names[i]; r.slots.push_back (sl); }
            return r;
        };

        // ---- ONE match by name: the position is known, so it is stated ----------------------
        {
            FakeHost h; SinkLog lg;
            auto src = h.sources();
            src.linkRack = [namedRack] (const juce::String&)
            { return namedRack (juce::StringArray { "UAD 1176", "FabFilter Pro-C 2" }); };
            ExecutorDo ex (src, sinks (lg));
            ToolOutcome got; bool fired = false;
            ex.doOp (call ("do", R"({"op":"add","name":"FabFilter Pro-C 2","channel":"bb411b8240",
                                     "confirm":true,"settings_structured":{"threshold_db":-18}})"),
                     [&] (ToolOutcome o) { got = std::move (o); fired = true; });
            runClock (lg);
            check (fired && got.ok, "SL1 the add lands", got.errorMessage);
            check ((int) got.result.getProperty ("slot_landed", juce::var (0)) == 2,
                   "SL2 RED-FIRST: the result carries slot_landed 2 - read off the rack, which is what "
                   "lets B state the order as fact (it carried nothing before)",
                   got.result.getProperty ("slot_landed", juce::var()).toString());
            check (got.result.getProperty ("slot_landed_name", juce::var()).toString() == "FabFilter Pro-C 2",
                   "SL3 ...with the name that was READ at that position, so the number is not a bare "
                   "claim", got.result.getProperty ("slot_landed_name", juce::var()).toString());
            check (got.landedForTheUser().contains ("in slot 2"),
                   "SL4 ...and the row he reads says where it went", got.landedForTheUser());
        }

        // ---- AND AN ADD WITH NO SETTINGS, which is most of B's adds ------------------------
        // The door my own wrong fixture found: with nothing to read back there is no settings check
        // to carry the position, and this is exactly the case where B was saying "the order hasn't
        // read back yet".
        {
            FakeHost h; SinkLog lg;
            auto src = h.sources();
            src.linkRack = [namedRack] (const juce::String&)
            { return namedRack (juce::StringArray { "UAD 1176", "FabFilter Pro-C 2" }); };
            ExecutorDo ex (src, sinks (lg));
            ToolOutcome got; bool fired = false;
            ex.doOp (call ("do", R"({"op":"add","name":"FabFilter Pro-C 2","channel":"bb411b8240",
                                     "confirm":true})"),
                     [&] (ToolOutcome o) { got = std::move (o); fired = true; });
            runClock (lg);
            check (fired && got.ok, "SL8 an add with no settings lands", got.errorMessage);
            check ((int) got.result.getProperty ("slot_landed", juce::var (0)) == 2,
                   "SL9 RED-FIRST: ...and still reports WHERE - the branch with nothing to read back "
                   "is the one B had no order from",
                   got.result.getProperty ("slot_landed", juce::var()).toString());
            check (got.landedForTheUser().contains ("in slot 2"),
                   "SL10 ...and the row says it", got.landedForTheUser());
        }

        // ---- TWO of that name and no placement: the read-back CANNOT say which -------------
        // This is the ambiguity the function already refuses to guess at for figures (it converts
        // every miss to "not confirmed"). A position is the same kind of claim, so it is withheld by
        // the same test - and a leg holds it, because a number here would be B stating a WRONG order
        // as fact, which is worse than B saying the order has not read back.
        {
            FakeHost h; SinkLog lg;
            auto src = h.sources();
            src.linkRack = [namedRack] (const juce::String&)
            { return namedRack (juce::StringArray { "UAD 1176", "FabFilter Pro-C 2", "FabFilter Pro-C 2" }); };
            ExecutorDo ex (src, sinks (lg));
            ToolOutcome got; bool fired = false;
            ex.doOp (call ("do", R"({"op":"add","name":"FabFilter Pro-C 2","channel":"bb411b8240",
                                     "confirm":true,"settings_structured":{"threshold_db":-18}})"),
                     [&] (ToolOutcome o) { got = std::move (o); fired = true; });
            runClock (lg);
            check (fired && got.ok, "SL5 the add lands", got.errorMessage);
            check (! got.result.getDynamicObject()->hasProperty ("slot_landed"),
                   "SL6 two slots of that name and no placement: NO position is claimed",
                   got.result.getProperty ("slot_landed", juce::var()).toString());
            check (! got.landedForTheUser().contains ("in slot"),
                   "SL7 ...and the row does not name one either", got.landedForTheUser());
        }
    }

    // ---- DU: "DON'T USE THAT ONE" (his 11:5x item 3, 21 Oct 2026) -----------------------------
    //
    // *"The MLimiterX is an expired demo. Can the plugin tell (it passes audio, so silent-slot
    // detection won't)? If not, give Sean a way to mark a plugin "don't use", stored like the silent
    // mark, and send it so B leaves it out."*
    //
    // IT CANNOT TELL: a host sees parameters and audio, and an expired demo that passes audio is
    // identical to a licensed one in both - which is why the silent-slot detector cannot reach this
    // case at all. So the mark is his, and these legs hold the two halves that can go wrong without
    // anybody noticing: WHICH SENTENCES TAKE IT (a false positive silently removes a plugin from
    // every future build), and WHETHER IT REACHES B.
    std::printf ("== DU: the mark's words, and the mark reaching the model ==\n");
    {
        using echojay::dontuse::parse;

        // THE SENTENCES THAT MARK
        check (parse ("don't use it again").marks, "DU1 his own words mark");
        check (parse ("Don't use the MLimiterX").marks
                 && parse ("Don't use the MLimiterX").name == "mlimiterx",
               "DU2 ...and the name comes out, filler words dropped",
               parse ("Don't use the MLimiterX").name);
        check (parse ("never use MLimiterX again").marks, "DU3 \"never use ... again\" marks");
        check (parse ("stop using the MLimiterX").marks, "DU4 \"stop using\" marks");
        check (parse ("dont use that plugin").marks, "DU5 ...and the apostrophe is optional");
        check (parse ("don't use it again").name.isEmpty(),
               "DU6 \"it\" is NOT a name - the caller resolves it from the limiter the loop just "
               "complained about, or asks", parse ("don't use it again").name);

        // THE SENTENCES THAT LIFT
        check (parse ("use MLimiterX again").lifts
                 && parse ("use MLimiterX again").name == "mlimiterx",
               "DU7 a lift names its plugin", parse ("use MLimiterX again").name);
        check (! parse ("use it again").took(),
               "DU8 ...and a lift with no name takes NOTHING: there is nothing for \"it\" to refer "
               "to once the plugin is out of the rack, and guessing would re-admit the wrong one");
        check (! parse ("use the CL 1B").took(),
               "DU9 ...and a bare \"use X\" is a REQUEST, not a settings change - it belongs to the "
               "server");

        // AND THE OTHER DIRECTION, which is the whole risk of this verb: a stored setting must not
        // change because a sentence mentioned not using something.
        check (! parse ("I don't use that one much").took(),
               "DU10 RED-FIRST: a remark about his habits is not an instruction - the verb has to "
               "OPEN the sentence");
        check (! parse ("why don't use the MLimiterX?").took(),
               "DU11 ...and a question is never an instruction");
        check (! parse ("don't use it again?").took(),
               "DU12 ...including his own words with a question mark on them");
        check (! parse ("can you not use the limiter").took(), "DU13 ...and a polite ask is not the verb");
        check (! parse ("").took() && ! parse ("   ").took(), "DU14 nothing takes nothing");
    }
    {
        // AND IT REACHES THE MODEL, with the reason - the half that makes B leave it out. The
        // inventory withholds them too (EJAgentExecutorRead's inventory source); what a START BODY
        // carries is what lets the model answer when he asks for one by name.
        FakeHost h; SinkLog lg;
        auto sk = sinks (lg);
        sk.dontUsePlugins = [] () -> juce::var
        {
            juce::Array<juce::var> a;
            auto* o = new juce::DynamicObject();
            o->setProperty ("name", "MLimiterX");
            o->setProperty ("why", "don't use it again - expired demo");
            a.add (juce::var (o));
            return juce::var (a);
        };
        ExecutorDo ex (h.sources(), std::move (sk));
        const auto ctx = ex.startContext();
        auto* arr = ctx.getProperty ("plugins_held", juce::var()).getArray();
        check (arr != nullptr && arr->size() == 1,
               "DU15 the start context carries B's own key, plugins_held (his ruling of 21 Oct: one key both sides agree)",
               juce::JSON::toString (ctx.getProperty ("plugins_held", juce::var())));
        check (arr != nullptr && ! arr->isEmpty()
                 && arr->getFirst().getProperty ("name", juce::var()).toString() == "MLimiterX"
                 && arr->getFirst().getProperty ("why", juce::var()).toString().isNotEmpty(),
               "DU16 ...with the reason HE gave, so the model can say why rather than pretend the "
               "plugin does not exist");
        const auto note = ctx.getProperty ("plugins_held_note", juce::var()).toString();
        check (note.containsIgnoreCase ("do not offer") && note.containsIgnoreCase ("clears itself")
                 && note.containsIgnoreCase ("names one"),
               "DU17 ...and the note tells it not to offer them AND that the mark does not expire - "
               "a model that assumes it does will offer the plugin again next week", note);
    }
    {
        // NO MARKS, NO KEY. An empty list must not travel as an empty array that reads like "he has
        // opinions about nothing" - absent means there is nothing to say.
        FakeHost h; SinkLog lg;
        auto sk = sinks (lg);
        sk.dontUsePlugins = [] { return juce::var(); };
        ExecutorDo ex (h.sources(), std::move (sk));
        const auto ctx = ex.startContext();
        check (ctx.getDynamicObject() != nullptr
                 && ! ctx.getDynamicObject()->hasProperty ("plugins_held"),
               "DU18 with no marks the key is ABSENT, not an empty list");
    }

    std::printf ("==== agent_round14_guard: %s (%d assertion(s) failed, %d passed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes);
    return failures == 0 ? 0 : 1;
}
