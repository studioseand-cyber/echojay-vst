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
#include "ChainHost.h"   // check 5 part 2: the load-failure record the inventory reads
#include "EchoJayUndoHistory.h"
#include "EJAgentClient.h"
#include "EJParamPlainNames.h"   // 18 Oct: the row's sentence is built there
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
        k.applyChainOps = [this] (const juce::String&, const juce::var&, std::function<void (bool, juce::String, juce::String)> cb)
        { ++applyCalls; if (cb) cb (applyOk, applyOk ? juce::String ("the rack took it") : juce::String ("refused"), {}); };
        k.buildChain = [this] (const juce::String&, const juce::var&, std::function<void (bool, juce::String)> cb)
        { ++buildCalls; if (cb) cb (buildOk, buildOk ? juce::String ("the build was sent")
                                                     : juce::String ("the build was refused")); };
        k.afterMs = [] (int, std::function<void()> fn) { if (fn) fn(); };
        return k;
    }
    bool transportKnown = true, transportPlaying = true;
    bool buildOk = true;
    int  buildCalls = 0;
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

    // ---------------------------------------------------------------- AUTO MODE (ship 2)
    //
    // ONE DECIDER, HELD AGAINST THE TOKENS. Auto asks ExecutorDo::undoabilityOf before a step runs; the
    // outcome carries an undo token after it ran. The design's rule is that these are the same function's
    // two readings, and the thing that would rot is the agreement between them - so it is asserted op by
    // op, and an op that gains a token without gaining its entry here fails this leg rather than quietly
    // becoming something Auto applies without asking.
    std::printf ("== AUTO MODE: undoabilityOf is the one decider, and it agrees with the tokens ==\n");
    {
        Fixture f; ExecutorDo ex (f.sources(), f.sinks());
        const char* undoable[] = { "set", "set_wet", "set_io", "set_master_wet", "set_pre_gain",
                                   "bypass", "remove", "move", "add", "replace", "build", "set_level" };
        bool allUndoable = true; juce::String bad;
        for (auto* op : undoable)
        {
            const auto u = ex.undoabilityOf (call (juce::String ("{\"op\":\"") + op + "\"}"), "make it punchier");
            if (! u.undoable) { allUndoable = false; bad << op << " "; }
        }
        check (allUndoable, "every op that pushes an undo entry is reported undoable, so Auto may run it",
               bad.isEmpty() ? juce::String ("all 12") : "NOT undoable: " + bad);

        const auto unknown = ex.undoabilityOf (call (R"({"op":"teleport_the_rack"})"), "do something odd");
        check (! unknown.mayRunInAuto() && unknown.whyNot.isNotEmpty(),
               "an op this build does not know is NOT run unasked in Auto, and says why  (the dialCapable "
               "rule: the fallback is the safe answer, not the convenient one)", unknown.whyNot);

        const auto undoPlain = ex.undoabilityOf (call (R"({"op":"undo"})"), "make the vocal brighter");
        check (! undoPlain.mayRunInAuto(),
               "an undo on a goal that is NOT an undo request still asks in Auto", undoPlain.whyNot);

        const auto undoAsked = ex.undoabilityOf (call (R"({"op":"undo"})"), "undo that");
        check (undoAsked.preApprovedInAuto && ! undoAsked.undoable,
               "...but \"undo that\" is pre-approved in Auto - the user's own words are the approval (Sean's "
               "ruling, 14 Oct) - and it is still NOT called undoable, because it hands back no token",
               undoAsked.whyNot);
        check (ExecutorDo::looksLikeUndoGoal ("put that back") && ExecutorDo::looksLikeUndoGoal ("revert that")
               && ! ExecutorDo::looksLikeUndoGoal ("make the vocal brighter"),
               "the undo-goal test is narrow: the words a user types when they mean put it back");

        const auto openEd = ex.undoabilityOf (call (R"({"op":"open_editor","slot":1})"), "show me the compressor");
        check (openEd.mayRunInAuto() && ! openEd.undoable,
               "open_editor runs in Auto because it changes nothing - and is not claimed to be undoable",
               openEd.whyNot);

        // AND THE AGREEMENT ITSELF: an op reported undoable must come back WITH a token. This is the pair
        // the design says must not drift.
        Fixture f2; f2.held = { {"attack_ms", 18.0} };
        ExecutorDo ex2 (f2.sources(), f2.sinks());
        const auto setOut = runDo (ex2, call (R"({"op":"set","slot":1,"settings_structured":{"params":{"attack_ms":18}}})"));
        check (setOut.ok && setOut.undoToken.isNotEmpty(),
               "a set that undoabilityOf calls undoable really does hand back an undo token",
               setOut.undoToken);
    }

    // ---------------------------------------------------------------- SEAN'S CHECK 5 (15 Oct 2026)
    std::printf ("== CHECK 5: settings carried on add / replace / build are READ BACK, never counted ==\n");
    {
        // The complaint: "add MTransient with attack +6 dB" landed the slot and reported success, while
        // the Link's log said NO MAP and action=LEAVE-UNEDITED. The slot is not the settings.
        //
        // (a) NOTHING READABLE - a third party with no map. The op succeeds (the plugin IS on the rack)
        //     and the settings must be reported as NOT CONFIRMED, with no figure quoted.
        {
            Fixture f;
            f.own.slots.push_back ({});                      // one slot on the rack after the add
            f.own.slots[0].n = 1; f.own.slots[0].name = "MTransient";
            f.unreadable = { "attack_db", "sustain_db" };     // a third party: no values readable at all
            ExecutorDo ex (f.sources(), f.sinks());
            const auto out = runDo (ex, call (R"({"op":"add","name":"MTransient","settings_structured":)"
                                              R"({"params":{"attack_db":6,"sustain_db":-2}}})"));
            const auto state = out.result.getProperty ("settings_state", juce::var()).toString();
            const auto confirmed = out.result.getProperty ("settings_confirmed", juce::var());
            check (out.ok, "the ADD itself still succeeds - the plugin is on the rack, and failing the op "
                           "would make the model add it a second time", out.errorMessage);
            check (state == "not_confirmed" && ! (bool) confirmed,
                   "...and its SETTINGS are reported not_confirmed, because nothing could be read back "
                   "(RED before this fix: confirmed by slot count, so it reported plain success)",
                   "settings_state=" + state);
            check (out.landedLine.containsIgnoreCase ("not confirmed")
                   && ! out.landedLine.contains ("6"),
                   "...and the landed line says so WITHOUT naming a figure it never read - which is the "
                   "sentence the closing text is built from",
                   out.landedLine);
        }
        // (b) READ BACK AND WRONG. The strongest case: the values exist and are not what was asked.
        {
            Fixture f;
            f.own.slots.push_back ({}); f.own.slots[0].n = 1; f.own.slots[0].name = "EchoJay Compressor";
            f.held = { { "attack_db", 0.0 }, { "sustain_db", 0.0 } };   // what Sean actually saw: 0.00 dB
            ExecutorDo ex (f.sources(), f.sinks());
            const auto out = runDo (ex, call (R"({"op":"add","name":"EchoJay Compressor","settings_structured":)"
                                              R"({"params":{"attack_db":6,"sustain_db":-2}}})"));
            const auto state = out.result.getProperty ("settings_state", juce::var()).toString();
            check (state == "not_applied",
                   "a value read back that is NOT what was asked reports the settings as not_applied, "
                   "naming each one - Sean's Attack still at 0.00 dB against a closing line that said +6",
                   "settings_state=" + state + " unapplied="
                   + out.result.getProperty ("settings_unapplied", juce::var()).toString());
            check (out.landedLine.containsIgnoreCase ("did NOT take")
                   || out.landedLine.containsIgnoreCase ("not take"),
                   "...and the landed line says the settings did not take", out.landedLine);
        }
        // (c) READ BACK AND RIGHT. The honest apply must still read as applied, or the fix is unusable.
        {
            Fixture f;
            f.own.slots.push_back ({}); f.own.slots[0].n = 1; f.own.slots[0].name = "EchoJay Compressor";
            f.held = { { "attack_db", 6.0 }, { "sustain_db", -2.0 } };
            ExecutorDo ex (f.sources(), f.sinks());
            const auto out = runDo (ex, call (R"({"op":"add","name":"EchoJay Compressor","settings_structured":)"
                                              R"({"params":{"attack_db":6,"sustain_db":-2}}})"));
            check (out.ok && out.result.getProperty ("settings_state", juce::var()).toString() == "applied"
                   && (bool) out.result.getProperty ("settings_confirmed", juce::var()),
                   "...and values that DO match read back as applied - proven in both directions, so the "
                   "check is neither unfalsifiable nor unpassable",
                   out.landedLine);
        }
        // (d) A BUILD, per slot. The chain block IS the rack, so slot i of the block is slot i+1.
        {
            Fixture f;
            f.own.slots.push_back ({}); f.own.slots[0].n = 1; f.own.slots[0].name = "EchoJay Compressor";
            f.unreadable = { "threshold_db" };
            ExecutorDo ex (f.sources(), f.sinks());
            const auto out = runDo (ex, call (R"({"op":"build","chain":[{"name":"EchoJay Compressor",)"
                                              R"("settings_structured":{"params":{"threshold_db":-24}}}]})"));
            check (out.ok && out.result.getProperty ("settings_state", juce::var()).toString() == "not_confirmed",
                   "a BUILD's per-slot settings are read back too - it was the other half of \"confirmed by "
                   "slot count only\", and a build is where most settings arrive",
                   out.landedLine);
        }
        // (f) A SLOT THAT WAS NEVER NAMED IS NOT GUESSED AT.
        //
        //     MY FIRST CUT OF THIS LEG WAS WRONG, and the gate caught it: I asserted that a `replace`
        //     with no slot reaches the settings check and claims nothing. It never gets there - the op
        //     is refused before any settings could be read, which is a BETTER answer than the one I
        //     was testing for. So the leg asserts what actually happens, and the belt inside
        //     confirmSettings (no slot -> no settings claim) stays as the second line of defence for
        //     any future op that does reach it without one.
        {
            Fixture f;
            f.own.slots.push_back ({}); f.own.slots[0].n = 1; f.own.slots[0].name = "EchoJay Compressor";
            f.held = { { "threshold_db", -24.0 } };     // slot 1 happens to hold the asked value
            ExecutorDo ex (f.sources(), f.sinks());
            const auto out = runDo (ex, call (R"({"op":"replace","name":"EchoJay Compressor",)"
                                              R"("settings_structured":{"params":{"threshold_db":-24}}})"));
            // Either road is honest; what must never happen is a claim about settings on a slot nobody
            // named. So: refused outright, OR it ran and claimed nothing.
            const auto state = out.result.getProperty ("settings_state", juce::var()).toString();
            check (! out.ok || state.isEmpty(),
                   "an op that carries settings but names no slot never CLAIMS anything about them - "
                   "it is refused before it gets that far, and if it ever does get there it says "
                   "nothing rather than reading slot 1 and being accidentally right",
                   juce::String (out.ok ? "ran, settings_state=\"" + state + "\""
                                        : "refused: " + out.errorCode + " " + out.errorMessage.substring (0, 70)));
        }
        // (e) NO SETTINGS AT ALL still behaves exactly as before: nothing to confirm, nothing claimed.
        {
            Fixture f;
            f.own.slots.push_back ({}); f.own.slots[0].n = 1;
            ExecutorDo ex (f.sources(), f.sinks());
            const auto out = runDo (ex, call (R"({"op":"add","name":"EchoJay Compressor"})"));
            check (out.ok && out.result.getProperty ("settings_state", juce::var()).toString().isEmpty(),
                   "an add with no settings reports no settings state - the check adds nothing to an op "
                   "that asked for nothing", out.landedLine);
        }
    }

    // ---------------------------------------------------------------- CHECK 5, PART 2
    std::printf ("== CHECK 5 part 2: a plugin that will not load is recorded, and never offered again ==\n");
    {
        // Cross-process and cross-session, because the failure happens in the LINK and the pick is made
        // in the V2. Driven here through the real file, in this guard's isolated state root.
        const juce::String name = "Guard Plugin That Will Not Load", fmt = "AudioUnit";
        ChainHost::clearLoadFailure (name, fmt);
        check (! ChainHost::loadFailedPluginNames().contains (name),
               "it starts unmarked, so the two directions below are not reading a leftover");
        ChainHost::recordLoadFailure (name, fmt, "An OS error occurred during initialisation of the plug-in (-1)");
        check (ChainHost::loadFailedPluginNames().contains (name),
               "a load failure is RECORDED where the inventory can read it (RED before: the only record "
               "was sessionLoadFailed_, in the Link's process, for that session only)");
        check (ChainHost::loadFailureReasonFor (name).contains ("OS error"),
               "...with the host's own reason, so the model can say what happened rather than guess",
               ChainHost::loadFailureReasonFor (name));
        ChainHost::clearLoadFailure (name, fmt);
        check (! ChainHost::loadFailedPluginNames().contains (name),
               "...and the mark CLEARS, so it is a reading of now and never a judgement about ownership "
               "(Sean: \"Clear the mark if the plugin later loads fine\")");
    }

    // ---------------------------------------------------------------- 18 OCT 2026
    // SEAN, IN LOGIC: *"The step detail under an add reads like a developer log: 'the rack took it
    // and now holds 1 slot(s); read back 1 slot(s) on the rack; read back Tube-Tech CL 1B:
    // threshold -12.00 dB, ...'. Make it a plain sentence, e.g. 'Added. Read back: threshold -12,
    // ratio 4:1, attack 10, gain 2. Release couldn't be read.'"*
    std::printf ("== 18 Oct: the row is a sentence, the model keeps the plumbing ==\n");
    {
        Fixture f;
        f.own.slots.push_back ({});                      // the add's slot, as (c) above sets one up
        f.own.slots[0].n = 1; f.own.slots[0].name = "Tube-Tech CL 1B";
        f.held = { { "threshold_db", -12.0 }, { "ratio", 4.0 }, { "attack_ms", 10.0 } };
        f.unreadable.add ("release_ms");          // the one his CL 1B would not report
        ExecutorDo ex (f.sources(), f.sinks());
        const auto out = runDo (ex, call (R"({"op":"add","name":"Tube-Tech CL 1B","settings_structured":)"
                                          R"({"params":{"threshold_db":-12,"ratio":4,"attack_ms":10,"release_ms":100}}})"));
        const auto row = out.landedForTheUser();
        check (out.ok, "the add lands", out.errorMessage);
        check (row.startsWith ("Added"),
               "the row OPENS with what happened, in a word (RED as it stood: it opened with \"the "
               "rack took it and now holds 1 slot(s)\")", row);
        check (! row.contains ("slot(s)") && ! row.contains (";") && ! row.contains ("on the rack"),
               "...and carries no slot counts, no semicolons and no plumbing - the three clauses he "
               "read in front of the one fact he wanted", row);
        check (row.contains ("Read back: ") && row.contains ("threshold -12") && row.contains ("ratio 4"),
               "...and says what came back, in words", row);
        check (row.containsIgnoreCase ("release") && row.containsIgnoreCase ("couldn't be read"),
               "...and names what could NOT be read as its own sentence, which is his example verbatim",
               row);
        // THE MODEL'S HALF IS UNTOUCHED: it still gets the structural evidence and the ids.
        check (out.landedLine.contains ("slot(s)") && out.landedLine.contains ("threshold_db")
               && out.landedLine != row,
               "the MODEL still gets the slot count and the ids - two audiences, one call, and the "
               "evidence stays where it is needed", out.landedLine);
    }
    {
        // An op with NOTHING asked: one word and a full stop on the row, the count on the model's half.
        Fixture f;
        f.own.slots.push_back ({}); f.own.slots[0].n = 1;
        ExecutorDo ex (f.sources(), f.sinks());
        const auto out = runDo (ex, call (R"({"op":"add","name":"EchoJay Compressor"})"));
        const auto row = out.landedForTheUser();
        check (out.ok && row.startsWith ("Added") && ! row.contains ("slot(s)"),
               "an add with no settings reads as a sentence too (RED as it stood: \"the rack took it "
               "and now holds 2 slot(s); read back 2 slot(s) on the rack\")", row);
        check (out.landedLine.contains ("slot(s)"),
               "...while the model keeps the read-back it needs as evidence", out.landedLine);
    }
    {
        // The word comes from a CLOSED list, and anything not on it is "Done" rather than an invented tense.
        check (echojay::params::opDoneWord ("add") == "Added"
               && echojay::params::opDoneWord ("remove") == "Removed"
               && echojay::params::opDoneWord ("set") == "Set"
               && echojay::params::opDoneWord ("set_rack_out") == "Done",
               "the op's word is a closed list - never a tense made up from the op name");
        check (echojay::params::landedRowSentence ("add", "CL 1B", "threshold -12", "release")
                   == "Added CL 1B. Read back: threshold -12. Release couldn't be read.",
               "...and the whole sentence is built in ONE place, so no caller writes its own",
               echojay::params::landedRowSentence ("add", "CL 1B", "threshold -12", "release"));
    }

    std::printf ("==== agent_round15_guard: %s (%d assertion(s) failed, %d passed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes);
    return failures == 0 ? 0 : 1;
}
