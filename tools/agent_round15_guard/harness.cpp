// =============================================================================
//  agent_round15_guard - SHIP 1: Sean's seven fixes from his Logic check of
//  ship_2026-10-14a (14 Oct 2026). The parts that are pure logic are held here;
//  the parts that need a real plugin, real audio or a real window are held by
//  the host rig (rows S1-S3, G1, K1, C11, L1).
//
//  EVERY LEG NAMES WHAT IT WAS RED FOR. A leg whose red direction is not stated
//  is a leg nobody can check, and this round is entirely about claims that could
//  not fail: a `set` confirmed by a slot count, a picture with no writer, a
//  finalise hook that only ever heard about success.
// =============================================================================
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJAgentExecutorDo.h"
#include "EchoJayUndoHistory.h"
#include "EJAgentClient.h"
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
ToolCall call (const juce::String& argsJson)
{
    ToolCall c; c.id = "t"; c.name = "do"; c.args = juce::JSON::parse (argsJson); c.approval = "free"; return c;
}

// ---- the fixture: a rack that answers reads, and a slotParam we can make lie --------------------------
struct Fixture
{
    RackRead own;
    // what the slot "holds" after an apply, by parameter name. A set's read-back is compared against this.
    std::map<juce::String, double> held;
    juce::StringArray unreadable;      // names the device refuses to report at all
    int applyCalls = 0;
    bool applyOk = true;

    Fixture()
    {
        own.valid = true; own.name = "Mix Bus"; own.kind = "mix_bus"; own.revision = 3;
        SlotRead s; s.n = 1; s.name = "EchoJay Compressor"; s.builtin = true; own.slots.push_back (s);
    }
    Sources sources()
    {
        Sources s;
        s.ownName = [this] { return own.name; };
        s.ownKind = [this] { return own.kind; };
        s.ownRack = [this] { return own; };
        s.tracks  = [] { return std::vector<TrackRead>(); };
        s.chainRevision = [this] { return own.revision; };
        s.transportKnown   = [this] { return transportKnown; };
        s.transportPlaying = [this] { return transportPlaying; };
        s.slotParam = [this] (const juce::String&, int, const juce::String& id, double& out) -> bool
        {
            if (unreadable.contains (id)) return false;
            const auto it = held.find (id);
            if (it == held.end()) return false;
            out = it->second; return true;
        };
        return s;
    }
    DoSinks sinks()
    {
        DoSinks k;
        k.agentModeOn = [] { return true; };
        k.echoJayOnly = [] { return false; };
        k.applyChainOps = [this] (const juce::String&, const juce::var&, std::function<void (bool, juce::String)> cb)
        { ++applyCalls; if (cb) cb (applyOk, applyOk ? juce::String ("the rack took it") : juce::String ("refused")); };
        k.afterMs = [] (int, std::function<void()> fn) { if (fn) fn(); };
        return k;
    }
    bool transportKnown = true, transportPlaying = true;
};

ToolOutcome runDo (ToolExecutor& ex, const ToolCall& c)
{
    ToolOutcome got; bool fired = false;
    ex.doOp (c, [&] (ToolOutcome o) { got = std::move (o); fired = true; });
    if (! fired) got = ToolOutcome::failure ("no_completion", "the executor never called done");
    return got;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("==== agent_round15_guard (ship 1: Sean's seven fixes) ====\n");

    // ---------------------------------------------------------------- FIX 1
    std::printf ("== FIX 1: a `set` is confirmed by its VALUES, not by a slot count ==\n");
    {
        // The values landed: applied, and the sentence names what was read back.
        Fixture f; f.held = { {"attack_ms", 18.0}, {"release_ms", 85.0} };
        ExecutorDo ex (f.sources(), f.sinks());
        const auto o = runDo (ex, call (R"({"op":"set","slot":1,"settings_structured":{"params":{"attack_ms":18,"release_ms":85}}})"));
        check (o.ok, "a set whose values read back correctly is applied", o.errorMessage);
        check (o.landedLine.contains ("18") && o.landedLine.contains ("85"),
               "...and the user's line states the values that were READ, not the ones asked for", o.landedLine);
        check ((bool) o.result.getProperty ("confirmed", false), "...and the result says it was confirmed");
    }
    {
        // SEAN'S CASE. The apply "succeeds" and the device still holds the old values.
        // RED AS IT STOOD: the success line was "the rack took it and now holds 2 slot(s)" - a SLOT COUNT,
        // which a set never changes, so this could not fail and the closing text claimed 18 ms and 85 ms
        // while the compressor sat at 10 and 120.
        Fixture f; f.held = { {"attack_ms", 10.0}, {"release_ms", 120.0}, {"makeup_db", 0.0} };
        ExecutorDo ex (f.sources(), f.sinks());
        const auto o = runDo (ex, call (R"({"op":"set","slot":1,"settings_structured":{"params":{"attack_ms":18,"release_ms":85,"makeup_db":1.5}}})"));
        check (! o.ok, "a set that changed NOTHING is reported not applied  (RED as it stood: it reported "
                       "success, because the read-back counted slots)");
        check (o.errorCode == "not_applied", "...as not_applied", o.errorCode);
        check (o.errorMessage.contains ("attack_ms") && o.errorMessage.contains ("10")
               && o.errorMessage.contains ("18"),
               "...naming each parameter with what it IS and what was asked", o.errorMessage);
        check (o.result.getProperty ("unapplied_params", juce::var()).toString().contains ("release_ms"),
               "...and the result carries them for the server, so the closing text can only state what landed",
               juce::JSON::toString (o.result));
    }
    {
        // PARTLY. One took, one did not: that is not a success.
        Fixture f; f.held = { {"attack_ms", 18.0}, {"release_ms", 120.0} };
        ExecutorDo ex (f.sources(), f.sinks());
        const auto o = runDo (ex, call (R"({"op":"set","slot":1,"settings_structured":{"params":{"attack_ms":18,"release_ms":85}}})"));
        check (! o.ok, "a set where ONE of two values took is not a success");
        check (o.errorMessage.contains ("release_ms") && ! o.errorMessage.contains ("attack_ms is"),
               "...and it names the one that did not, not the one that did", o.errorMessage);
        check (o.errorMessage.contains ("What did land") && o.errorMessage.contains ("attack_ms"),
               "...while still saying what DID land, so the server can report that much honestly", o.errorMessage);
    }
    {
        // UNREADABLE. A third-party plugin whose values cannot be read: applied, and SAID to be unconfirmed.
        // Never a bare success, and never a failure either - nothing is known to be wrong.
        Fixture f; f.unreadable.add ("threshold_db");
        ExecutorDo ex (f.sources(), f.sinks());
        const auto o = runDo (ex, call (R"({"op":"set","slot":1,"settings_structured":{"params":{"threshold_db":-18}}})"));
        check (o.ok, "a set whose values cannot be read back is not a failure", o.errorMessage);
        check (! (bool) o.result.getProperty ("confirmed", true),
               "...but it is marked UNCONFIRMED rather than reported as done");
        check (o.landedLine.containsIgnoreCase ("could not read"),
               "...and the user is told so in plain words", o.landedLine);
    }
    {
        // THE FLAT FORM, at the executor's door: the names at the top level with no "params" wrapper, which
        // is what B actually sent. The executor must read them as the asked values.
        Fixture f; f.held = { {"attack_ms", 18.0} };
        ExecutorDo ex (f.sources(), f.sinks());
        const auto o = runDo (ex, call (R"({"op":"set","slot":1,"settings_structured":{"attack_ms":18}})"));
        check (o.ok, "a FLAT settings_structured is read for the read-back too  (fix 1(a) accepts the shape; "
                     "this proves the confirmation follows it)", o.errorMessage);
    }

    // ---------------------------------------------------------------- FIX 3
    std::printf ("== FIX 3: nothing playing is an ASK, not a failure ==\n");
    {
        Fixture f; f.transportKnown = true; f.transportPlaying = false;
        ExecutorRead rd (f.sources());
        ToolOutcome got; bool fired = false;
        rd.check (call (R"({"what":"gr","slot":1})"), [&] (ToolOutcome o) { got = std::move (o); fired = true; });
        check (fired && got.errorCode == "needs_playback",
               "with the transport STOPPED, check(gr) answers needs_playback  (RED as it stood: not_playing, "
               "which the model read as a dead end - Sean's nine rounds)", got.errorCode);
        check (got.errorMessage.containsIgnoreCase ("ask the user") && got.errorMessage.containsIgnoreCase ("press play"),
               "...and says to ask the user to press play", got.errorMessage);
        check (got.forTheUser().containsIgnoreCase ("press play"),
               "...with a plain line for the user", got.forTheUser());
        check ((bool) got.result.getProperty ("retryable", false),
               "...and it is retryable, because pressing play changes the answer");
    }
    {
        Fixture f; f.transportKnown = true; f.transportPlaying = true;
        ExecutorRead rd (f.sources());
        ToolOutcome got; bool fired = false;
        rd.check (call (R"({"what":"gr","slot":1})"), [&] (ToolOutcome o) { got = std::move (o); fired = true; });
        check (fired && got.errorCode == "not_playing",
               "with the transport RUNNING and no reading, it is still not_playing - the old answer, now true",
               got.errorCode);
        check (got.errorMessage.containsIgnoreCase ("silent or muted"),
               "...and it says why that is different from a stopped transport", got.errorMessage);
    }

    // ---------------------------------------------------------------- FIX 4
    std::printf ("== FIX 4: undo walks past dead entries, and the pre-gain is one of them ==\n");
    {
        echojay::UndoHistory h;
        juce::StringArray said;
        juce::StringArray applied;
        h.status = [&said] (const juce::String& s) { said.add (s); };
        // two entries whose target has gone, then one that applies
        h.apply = [&applied] (echojay::UndoEntry& e, bool) -> bool
        { if (e.kind == "dead") return false; applied.add (e.label); return true; };
        echojay::UndoEntry live;  live.kind = "preGain"; live.label = "pre-gain 0.0 dB";
        echojay::UndoEntry dead1; dead1.kind = "dead";   dead1.label = "remove NLS Buss (s)";
        echojay::UndoEntry dead2; dead2.kind = "dead";   dead2.label = "remove UAD UA 1176LN Rev E";
        h.push (live); h.push (dead1); h.push (dead2);
        const bool stepped = h.undo();
        check (stepped, "undo returns true");
        check (applied.size() == 1 && applied[0] == "pre-gain 0.0 dB",
               "ONE press walks past both dead entries and undoes the first real one  (RED as it stood: it "
               "consumed one dead entry per press - Sean pressed twice and burned both)",
               applied.joinIntoString (","));
        check (said.size() == 1 && said[0].contains ("skipped 2"),
               "...and says how many it stepped over, in one line", said.joinIntoString (" | "));
        check (h.undoDepth() == 0, "...leaving the stack empty", juce::String (h.undoDepth()));
    }
    {
        // AND IT NEVER UNDOES TWO REAL STEPS FOR ONE PRESS - the opposite fault, and a worse one.
        echojay::UndoHistory h;
        juce::StringArray applied;
        h.apply = [&applied] (echojay::UndoEntry& e, bool) -> bool { applied.add (e.label); return true; };
        echojay::UndoEntry a; a.kind = "x"; a.label = "first";
        echojay::UndoEntry b; b.kind = "x"; b.label = "second";
        h.push (a); h.push (b);
        h.undo();
        check (applied.size() == 1 && applied[0] == "second",
               "one press undoes exactly one applicable step", applied.joinIntoString (","));
    }
    {
        // A STACK OF NOTHING BUT DEAD ENTRIES says so, rather than reporting the last one as the answer.
        echojay::UndoHistory h;
        juce::StringArray said;
        h.status = [&said] (const juce::String& s) { said.add (s); };
        h.apply = [] (echojay::UndoEntry&, bool) -> bool { return false; };
        echojay::UndoEntry d1; d1.kind = "dead"; d1.label = "gone one";
        echojay::UndoEntry d2; d2.kind = "dead"; d2.label = "gone two";
        h.push (d1); h.push (d2);
        h.undo();
        check (said.size() == 1 && said[0].containsIgnoreCase ("Nothing can be undone right now")
                                && said[0].containsIgnoreCase ("no longer present"),
               "a history of only dead entries says so in one line, rather than reporting the last dead "
               "entry as the answer",
               said.joinIntoString (" | "));
        check (said[0].contains ("gone one") && said[0].contains ("gone two"),
               "...naming both", said.joinIntoString (" | "));
        // AND IT SPENDS NOTHING (15 Oct, after remote_ops_guard's STAGE 5 argued the other half of this).
        // Walking past dead entries fixes Sean's item 4 - a press used to spend one dead entry and report
        // success - but a press that can apply NOTHING must not empty the history either: every one of
        // those steps becomes undoable again the moment its Link is back. So the stack is restored exactly,
        // and nothing moves to redo. Asserted here as well as there, because this is the leg that would
        // notice if the restore were dropped.
        check (h.undoDepth() == 2 && h.redoDepth() == 0,
               "...and a press that could apply nothing LOSES NOTHING: both steps are still undoable",
               "undo " + juce::String (h.undoDepth()) + ", redo " + juce::String (h.redoDepth()));
    }
    {
        // ONE DRAG, ONE ENTRY: the knob emits a value per mouse-move (Sean's drag logged twelve in a second).
        echojay::UndoHistory h;
        h.apply = [] (echojay::UndoEntry&, bool) -> bool { return true; };
        auto pack = [] (double db) { auto* o = new juce::DynamicObject(); o->setProperty ("db", db);
                                     o->setProperty ("userSet", true); return juce::var (o); };
        for (int i = 0; i < 12; ++i)
        {
            echojay::UndoEntry e; e.kind = "preGain"; e.coalesceKey = "preGain";
            e.label = "pre-gain"; e.before = pack (0.0); e.after = pack (0.5 * (i + 1));
            h.push (e);
        }
        check (h.undoDepth() == 1, "twelve mouse-moves coalesce into ONE undo entry",
               juce::String (h.undoDepth()));
    }

    // ---------------------------------------------------------------- FIX 2
    std::printf ("== FIX 2: the client's bound is longer than the server's ==\n");
    {
        // The constant itself, because the behaviour it drives cannot be unit-tested without a 5-minute
        // round: a client that gives up before the server does turns a slow answer into a lost one, and
        // Sean's was cut at exactly 60.000 s with "check your internet connection".
        check (EJAgentClient::defaultConnectTimeoutMs() >= 300000,
               "the agent client waits at least as long as the server's 300 s  (RED as it stood: 60'000)",
               juce::String (EJAgentClient::defaultConnectTimeoutMs()));
    }

    // ---------------------------------------------------------------- the refusal text
    std::printf ("== the unknown-track refusal asks with NAMES, never 'the exact name' ==\n");
    {
        Fixture f;
        auto s = f.sources();
        s.tracks = [] { std::vector<TrackRead> v; TrackRead a; a.uid = "aaa111"; a.name = "Untitled";
                        TrackRead b; b.uid = "bbb222"; b.name = "Untitled 2"; v = { a, b }; return v; };
        ExecutorRead rd (s);
        ToolOutcome got; bool fired = false;
        rd.look (call (R"({"what":"rack","channel":"no-such-uid"})"), [&] (ToolOutcome o) { got = std::move (o); fired = true; });
        check (fired && ! got.ok, "a channel that does not exist is refused");
        check (got.errorMessage.containsIgnoreCase ("tap choices"),
               "...telling the model to ask with the NAMES as tap choices  (RED as it stood: \"by uid or by "
               "its exact name\", which is how Sean was asked for an exact name on 13 Oct)", got.errorMessage);
        check (! got.errorMessage.containsIgnoreCase ("exact name"),
               "...and never 'exact name'", got.errorMessage);
        check (got.errorMessage.contains ("Untitled") && got.errorMessage.contains ("Untitled 2"),
               "...with the real names in it", got.errorMessage);
    }

    std::printf ("==== agent_round15_guard: %s (%d assertion(s) failed, %d passed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes);
    return failures == 0 ? 0 : 1;
}
