// agent_round13_guard - SEAN'S 13 OCT IN-HOST ROUND, AGAINST A FAKE HOST.
//
// Items 1, 2, 3, 8, 9 and 10 of the 13 Oct round, each as a leg that fails on the code as it stood before the
// fix and passes after it. The fake-source shape is agent_executor_read_guard's, deliberately: these are the
// same sources, and a second fixture would be a second set of assumptions to keep true.
//
//   L1  ITEM 1, RESOLUTION. "uid bb411b8240" resolves to bb411b8240. Was the whole round's root cause: the agent
//       sent the label with the uid, having read it that way out of the start context, and nothing stripped
//       it. Also "uid:", "track Kick", the bare uid, the exact name, the wrong case - and an unresolvable
//       argument comes back AS SENT so the failure quotes the model its own string.
//   L2  ITEM 1, NOT OVER-STRIPPING. A track whose name begins with a label word keeps its name.
//   L3  ITEM 1, THE READBACK. A build that is acked but reads back an empty rack FAILS, loudly, and the
//       sentence carries the figure. RED DIRECTION: the identical build on a rack that reads back its slots
//       succeeds - so the leg proves the refusal is about the readback and not about builds.
//   L4  ITEM 2. describeOtherChannel names another channel and says nothing about the open one.
//   L5  ITEM 3. The cross-channel refusal: the MODEL's text keeps the instruction and the channel list, and
//       the USER's line has neither. Both halves asserted, because dropping the model's half to tidy the
//       user's would break item 6(d)'s loop-stopping.
//   L6  ITEM 8. A measurement aimed at a Link answers "not from a Link yet" and NEVER "slot not on rack" -
//       the sentence Sean's agent retried eighteen times.
//   L7  ITEM 9. The inventory pages and filters: total is the match count before trimming, offset/limit walk
//       it, hasMore is true until the end, and a filter that matches nothing is a RESULT.
//   L8  ITEM 10. set_rack_out reaches the rack-out sink (delta by default, absolute when asked) and never the
//       chain-op path; an unknown op's refusal NAMES the ops this build has.
//   L9  ITEM 3b. The markdown renderer: **bold** becomes a bold run with no asterisks left in the text, and
//       an unmatched marker stays a literal character.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJAgentExecutorRead.h"
#include "EJAgentExecutorDo.h"
#include "EJChatMarkdown.h"
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
}

ToolCall call (const juce::String& name, const juce::String& argsJson)
{
    ToolCall c; c.id = "t"; c.name = name; c.args = juce::JSON::parse (argsJson); c.approval = "free"; return c;
}

struct FakeHost
{
    RackRead own, link;
    std::vector<TrackRead> tracks;
    juce::StringArray inventory;
    // what a read-back of the Link reports: the readback leg drives THIS, not the apply
    int linkSlotsOnReadback = 2;

    FakeHost()
    {
        own.valid = true; own.name = "Mix Bus"; own.kind = "mix_bus"; own.revision = 7;
        { SlotRead s; s.n = 1; s.name = "EchoJay EQ"; s.builtin = true; own.slots.push_back (s); }

        link.valid = true; link.remote = true; link.uid = "bb411b8240"; link.name = "88_7even Kick_2_01"; link.kind = "channel"; link.revision = 3;
        { SlotRead s; s.n = 1; s.name = "UAD 1176"; link.slots.push_back (s); }
        { SlotRead s; s.n = 2; s.name = "EchoJay EQ"; s.builtin = true; link.slots.push_back (s); }

        // OPAQUE uids, as the registry really holds them (Sean's was bb411b8240). My FIRST fixture used the
        // uid "lnk_kick" for a track called "Kick", and the unique-containment fallback then resolved
        // "uid lnk_kick" by finding "kick" inside it - so every L1 leg passed GREEN with the label-stripping
        // deleted, which I only noticed by running the red direction. A fixture whose uid contains its own
        // track name cannot test a uid parser.
        TrackRead t1; t1.uid = "bb411b8240"; t1.name = "88_7even Kick_2_01"; t1.connected = true;
        TrackRead t2; t2.uid = "7c91ae0032"; t2.name = "Lead Vocal";         t2.connected = true;
        TrackRead t3; t3.uid = "0af3b71d55"; t3.name = "linkwood texture";   t3.connected = true;   // L2's trap
        tracks = { t1, t2, t3 };

        const char* real[] = { "Abbey Road Chambers", "Acme Opticom XLA-3", "FabFilter Pro-C 2",
                               "FabFilter Pro-Q 3", "Kramer PIE Compressor", "L3 UltraMaximizer",
                               "Tube-Tech CL 1B Compressor", "SSL G-Master Buss Compressor" };
        for (auto* n : real) inventory.add (n);
        for (int i = 0; i < 400; ++i) inventory.add ("Filler Device " + juce::String (i));
    }

    RackRead linkBack() const
    {
        RackRead r = link;
        r.slots.clear();
        for (int i = 0; i < linkSlotsOnReadback; ++i) { SlotRead s; s.n = i + 1; s.name = "slot"; r.slots.push_back (s); }
        return r;
    }

    Sources sources()
    {
        Sources s;
        s.ownName = [this] { return own.name; };
        s.ownKind = [this] { return own.kind; };
        s.ownRack = [this] { return own; };
        s.linkRack = [this] (const juce::String& uid)
        {
            if (uid == link.uid) return linkBack();
            RackRead r; r.why = "no Link with uid \"" + uid + "\" is in the registry"; return r;
        };
        s.tracks = [this] { return tracks; };
        s.inventory = [this] { return inventory; };
        s.chainRevision = [this] { return own.revision; };
        return s;
    }
};

// What the sinks were asked to do, so a leg can assert the ROUTE and not only the answer.
struct SinkLog
{
    int chainOps = 0, rackOut = 0;
    juce::String lastUid;
    double lastDb = 0.0;
    bool lastAbsolute = false;
    bool applyOk = true;
};

DoSinks sinks (SinkLog& lg)
{
    DoSinks s;
    s.agentModeOn = [] { return true; };
    s.echoJayOnly = [] { return false; };
    s.applyChainOps = [&lg] (const juce::String& uid, const juce::var&, std::function<void (bool, juce::String, juce::String)> cb)
    {
        ++lg.chainOps; lg.lastUid = uid;
        if (cb) cb (lg.applyOk, lg.applyOk ? juce::String ("the rack took it") : juce::String ("the rack refused it"), {});
    };
    s.setRackOutDb = [&lg] (const juce::String& uid, double db, bool absolute,
                            std::function<void (bool, juce::String)> cb)
    {
        ++lg.rackOut; lg.lastUid = uid; lg.lastDb = db; lg.lastAbsolute = absolute;
        if (cb) cb (true, "the rack's OUT is now " + juce::String (db, 1) + " dB");
    };
    s.undoOne = [] (const juce::String&, std::function<void (bool, juce::String)> cb) { if (cb) cb (true, "undone"); };
    // ---- 14 OCT 2026: THIS FIXTURE GAINED A BUILD SINK, BECAUSE THE PRODUCT DID ----------------------
    //
    // L3 below drives a `build` to prove the read-back refusal. When these legs were written a build went
    // out as an EDIT OP, so the apply sink was the only sink it needed. Sean's 13 Oct round showed why that
    // was wrong (`op 1 invalid: unknown operation "build"`), and a build now takes buildChain - so a fixture
    // without one gets the new, correct `not_in_phase` refusal and five legs go red for a reason that has
    // nothing to do with what they test.
    //
    // The fixture follows the product. The SLOT-COUNT rule L3 exists for is unchanged and still proved here;
    // the poll that was added with the new route has its own legs in agent_round14_guard (N3).
    s.buildChain = [&lg] (const juce::String& uid, const juce::var&, std::function<void (bool, juce::String)> cb)
    {
        ++lg.chainOps; lg.lastUid = uid;
        if (cb) cb (lg.applyOk, lg.applyOk ? juce::String ("the build was sent to the rack")
                                           : juce::String ("the rack refused it"));
    };
    // ...and an immediate clock, so a leg that expects an answer gets one in the same call rather than
    // waiting on a timer this harness does not run.
    s.afterMs = [] (int, std::function<void()> fn) { if (fn) fn(); };
    return s;
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

// The uid a call on `channel` would ACT on, read off the sink the apply actually reached.
//
// My first version of this probe asked look(rack) and read the uid out of the result, which measured two
// things at once and got both wrong: a resolved track whose rack the fixture does not serve looked
// "unresolved", and the rack shape does not carry a uid for every case anyway. The sink is the right
// instrument - resolution exists to decide what gets written, so what got written is the answer.
juce::String resolvedUid (const Sources& src, const juce::String& channel)
{
    SinkLog lg; ExecutorDo ex (src, sinks (lg));
    const auto o = run (ex, "do", call ("do", "{\"op\":\"set_pre_gain\",\"db\":-1.0,\"confirm\":true,"
                                              "\"channel\":\"" + channel + "\"}"));
    if (! o.ok) return "<refused: " + o.errorMessage.upToFirstOccurrenceOf (".", false, false) + ">";
    return lg.chainOps == 1 ? lg.lastUid : juce::String ("<nothing was applied>");
}
} // namespace

// EchoJay_NSLog is NOT defined here: this guard links the shipped SharedCode archive, which carries
// NativeClip's own definition. agent_executor_read_guard does the same (it defines nothing), and defining a
// second one is a duplicate symbol at link time rather than a quiet override.

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("==== agent_round13_guard ====\n");

    // ---------------------------------------------------------------- L1
    std::printf ("== L1: item 1, the label is not part of the uid ==\n");
    {
        FakeHost h; const auto src = h.sources();
        struct Case { const char* arg; const char* want; const char* why; };
        const Case cases[] = {
            { "bb411b8240",       "bb411b8240", "a bare uid" },
            { "uid bb411b8240",   "bb411b8240", "\"uid <x>\" - SEAN'S CASE, red before the fix" },
            { "uid: bb411b8240",  "bb411b8240", "\"uid: <x>\"" },
            { "track 88_7even Kick_2_01", "bb411b8240", "\"track <name>\"" },
            { "88_7even Kick_2_01", "bb411b8240", "the exact track name" },
            { "88_7EVEN KICK_2_01", "bb411b8240", "the exact name in the wrong case" },
        };
        for (const auto& c : cases)
        {
            const auto got = resolvedUid (src, c.arg);
            check (got == c.want, juce::String ("\"") + c.arg + "\" -> " + c.want + "   (" + c.why + ")", got);
        }
        // Unresolvable: the message must quote what the MODEL sent, not a normalised form of it.
        SinkLog lgU; ExecutorDo ex (h.sources(), sinks (lgU));
        const auto o = run (ex, "do", call ("do", "{\"op\":\"set_pre_gain\",\"db\":-1.0,\"confirm\":true,"
                                                  "\"channel\":\"uid nope_1234\"}"));
        check (! o.ok, "an unresolvable channel fails rather than guessing");
        check (o.errorMessage.contains ("uid nope_1234"),
               "...and the failure quotes the argument AS SENT, label and all", o.errorMessage.substring (0, 90));
        check (o.errorMessage.contains ("88_7even Kick_2_01") && o.errorMessage.contains ("Lead Vocal"),
               "...and still lists the channels that exist (item 6(d) survives)");
    }

    // ---------------------------------------------------------------- L2
    std::printf ("== L2: item 1, a name that begins with a label word is not stripped ==\n");
    {
        FakeHost h; const auto src = h.sources();
        check (resolvedUid (src, "linkwood texture") == "0af3b71d55",
               "\"linkwood texture\" resolves to its OWN track - the label rule needs a separator after it, so "
               "\"link\" is not stripped out of \"linkwood\"", resolvedUid (src, "linkwood texture"));
        check (resolvedUid (src, "0af3b71d55") == "0af3b71d55", "...and so does its uid", resolvedUid (src, "0af3b71d55"));
    }

    // ---------------------------------------------------------------- L3
    std::printf ("== L3: item 1, a build is not applied until a read-back says so ==\n");
    {
        const juce::String buildCall =
            "{\"op\":\"build\",\"channel\":\"uid bb411b8240\",\"confirm\":true,"
            "\"chain\":[{\"name\":\"UAD 1176\"},{\"name\":\"EchoJay EQ\"},{\"name\":\"L3 UltraMaximizer\"}]}";
        {   // the rack comes back EMPTY: Sean's case, and it must refuse
            FakeHost h; h.linkSlotsOnReadback = 0;
            SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", buildCall));
            check (lg.chainOps == 1, "the build WAS sent (the apply is not what this leg changes)");
            check (! o.ok, "an acked build whose rack reads back EMPTY is reported as NOT applied  "
                           "(RED as it stood: success came from the file write)", o.errorMessage.substring (0, 70));
            check (o.errorMessage.contains ("0 slot(s)") && o.errorMessage.contains ("3 were expected"),
                   "...and the sentence carries both figures", o.errorMessage.substring (0, 120));
            check (o.errorMessage.containsIgnoreCase ("do not plan against this rack"),
                   "...and tells the next round to stop planning against it");
            check (o.forTheUser().length() < 120 && ! o.forTheUser().containsIgnoreCase ("do not plan"),
                   "...while the USER's line is one plain sentence (item 3)", o.forTheUser());
        }
        {   // THE RED DIRECTION: the same build, a rack that reads back its slots
            FakeHost h; h.linkSlotsOnReadback = 3;
            SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", buildCall));
            check (o.ok, "the IDENTICAL build succeeds when the read-back shows the slots - so the refusal is "
                         "about the read-back, not about builds", o.errorMessage);
            check (o.landedLine.contains ("read back 3 slot(s)"), "...and says what it read back", o.landedLine);
        }
        {   // a value op changes no shape, so it must not be gated on a slot count
            FakeHost h; h.linkSlotsOnReadback = 0;
            SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", "{\"op\":\"set_pre_gain\",\"db\":-2.0}"));
            check (o.ok, "a non-structural op is NOT gated on a slot count", o.errorMessage);
        }
    }

    // ---------------------------------------------------------------- L4
    std::printf ("== L4: item 2, the plan line can name the track ==\n");
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        check (ex.describeOtherChannel (call ("do", "{\"op\":\"set\",\"channel\":\"uid bb411b8240\",\"slot\":1}")) == "88_7even Kick_2_01",
               "a call on another channel describes it by NAME, from a labelled uid");
        check (ex.describeOtherChannel (call ("do", "{\"op\":\"set\",\"slot\":1}")).isEmpty(),
               "a call on the open channel describes nothing");
        ex.setTarget ("bb411b8240");
        check (ex.describeOtherChannel (call ("do", "{\"op\":\"set\",\"channel\":\"bb411b8240\",\"slot\":1}")).isEmpty(),
               "...and neither does a call on the channel already addressed");
    }

    // ---------------------------------------------------------------- L5
    std::printf ("== L5: item 3, one gate, two audiences ==\n");
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        const auto o = run (ex, "do", call ("do", "{\"op\":\"set\",\"channel\":\"uid bb411b8240\",\"slot\":1,"
                                                  "\"settings_structured\":{\"params\":{\"ratio\":4}}}"));
        check (! o.ok && o.errorCode == "needs_confirmation", "a cross-channel edit without confirm is refused", o.errorCode);
        check (lg.chainOps == 0, "...and NOTHING was applied");
        check (o.errorMessage.contains ("confirm") && o.errorMessage.contains ("88_7even Kick_2_01"),
               "the MODEL is told how to proceed, by name");
        const auto u = o.forTheUser();
        check (! u.containsIgnoreCase ("confirm") && ! u.contains ("uid ") && ! u.contains ("repeat this same call"),
               "the USER's line carries no instruction and no uid  (RED as it stood: it was the model's text)", u);
        check (u.contains ("88_7even Kick_2_01"), "...but does say whose channel it is", u);
        // and WITH confirm it goes through, to that Link
        SinkLog lg2; ExecutorDo ex2 (h.sources(), sinks (lg2));
        const auto o2 = run (ex2, "do", call ("do", "{\"op\":\"set\",\"channel\":\"uid bb411b8240\",\"slot\":1,\"confirm\":true,"
                                                    "\"settings_structured\":{\"params\":{\"ratio\":4}}}"));
        check (o2.ok && lg2.chainOps == 1 && lg2.lastUid == "bb411b8240",
               "with confirm:true it applies TO THAT LINK", lg2.lastUid);
    }

    // ---------------------------------------------------------------- L6
    std::printf ("== L6: item 8, a measurement on a Link says so, once ==\n");
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        // ---- gr LEFT THIS RULE ON 18 OCT 2026 (item 13) ----------------------------------------
        // A Link publishes its slots' measurements now, so "gain reduction cannot be read from a
        // Link yet" stopped being true, and on 19 Oct check(gr) began reading the channel the call
        // NAMES rather than whatever the window has open - so this call reaches the Link's own
        // road and answers (here: nothing has been heard on that slot yet, which is a fact about
        // the Link). The item-8 protection this leg exists for is kept below, where it still
        // bites: the refusal must never be about the open rack.
        {
            const auto g = run (ex, "check", call ("check", "{\"what\":\"gr\",\"slot\":2,\"channel\":\"uid bb411b8240\"}"));
            check (g.errorCode != "not_available_on_link",
                   "check(gr) on a Link is NOT refused with \"cannot be read from a Link\" any more "
                   "(item 13, 18 Oct)", g.errorCode + " / " + g.errorMessage.substring (0, 60));
            check (! g.errorMessage.containsIgnoreCase ("is not on the rack"),
                   "...and still never answers about the open rack", g.errorMessage.substring (0, 80));
        }
        // ---- AND THESE TWO LEFT THE RULE ON 19 OCT 2026 ---------------------------------------
        // *"check spectrum failed twice with 'The analyser's spectrum cannot be read from a Link
        // yet'. The full analysis must be reachable for every Link, just like it is when you
        // capture... never 'cannot be read'."*  The gates were stale in the same way gr's was: the
        // Link's frame carries the six bands and its true peak, and its rack read carries each
        // slot's outTpDb, so both branches were refusing data that had already crossed.
        //
        // item 8's RULE still stands and still has a leg - a measurement that genuinely cannot be
        // taken says so once, in words, and says a retry will not help. What changed is WHICH
        // measurements those are: as of today, none of the three. The loop is therefore empty, and
        // kept rather than deleted, so the next reader sees the rule and its history in one place.
        for (const char* what : std::initializer_list<const char*> { })
        {
            const auto o = run (ex, "check", call ("check", juce::String ("{\"what\":\"") + what
                                                            + "\",\"slot\":2,\"channel\":\"uid bb411b8240\"}"));
            const juce::String w (what);
            check (! o.ok && o.errorCode == "not_available_on_link",
                   "check(" + w + ") on a Link -> not_available_on_link", o.errorCode + " / " + o.errorMessage.substring (0, 60));
            check (! o.errorMessage.containsIgnoreCase ("is not on the rack"),
                   "...and NEVER \"is not on the rack\"  (RED as it stood: it answered about the open rack)",
                   o.errorMessage.substring (0, 80));
            check (o.errorMessage.containsIgnoreCase ("will not work on a retry"),
                   "...and says a retry will not help, so the loop stops");
        }
        // the own rack still measures: the refusal must be about Links, not about measuring
        const auto own = run (ex, "check", call ("check", "{\"what\":\"gr\",\"slot\":1}"));
        check (own.errorCode != "not_available_on_link",
               "the OWN rack is not refused by this rule", own.errorCode);
        // ---- AND NEITHER IS A LINK, FOR ANY OF THE THREE (19 Oct 2026) ----------------------
        for (const char* what : { "gr", "true_peak", "spectrum" })
        {
            const auto o = run (ex, "check", call ("check", juce::String ("{\"what\":\"") + what
                                                            + "\",\"slot\":2,\"channel\":\"uid bb411b8240\"}"));
            check (o.errorCode != "not_available_on_link",
                   juce::String ("check(") + what + ") on a Link is NOT refused with \"cannot be read "
                   "from a Link\" any more - his 19 Oct ruling, and the data was already crossing",
                   o.errorCode.isEmpty() ? juce::String ("answered") : o.errorCode);
            check (! o.errorMessage.containsIgnoreCase ("cannot be read from a Link"),
                   juce::String ("...and the sentence is gone from check(") + what + ") too",
                   o.errorMessage.substring (0, 70));
        }
    }

    // ---------------------------------------------------------------- L7
    std::printf ("== L7: item 9, the inventory pages and filters ==\n");
    {
        FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
        auto page = [&] (const juce::String& args)
        {
            const auto o = run (ex, "look", call ("look", "{\"what\":\"inventory\"" + args + "}"));
            return o.result;
        };
        const auto all = page ("");
        check ((int) all.getProperty ("total", 0) == 408, "total is the WHOLE list, not the page",
               all.getProperty ("total", 0).toString());
        check ((int) all.getProperty ("returned", 0) == 60 && (bool) all.getProperty ("hasMore", false),
               "the first page is bounded and says there is more",
               all.getProperty ("returned", 0).toString());

        const auto comp = page (",\"match\":\"compressor\"");
        check ((int) comp.getProperty ("total", 0) == 3,
               "match \"compressor\" finds the three compressors and not 408 plugins  "
               "(RED as it stood: there was no filter at all)", comp.getProperty ("total", 0).toString());
        check (! (bool) comp.getProperty ("hasMore", true), "...and a short result says there is no more");
        if (auto* arr = comp.getProperty ("names", juce::var()).getArray())
        {
            juce::StringArray got; for (const auto& v : *arr) got.add (v.toString());
            check (got.contains ("Kramer PIE Compressor") && got.contains ("Tube-Tech CL 1B Compressor")
                   && got.contains ("SSL G-Master Buss Compressor"), "...and they are the right three", got.joinIntoString (", "));
        }
        else check (false, "match \"compressor\" returned a names array");

        const auto two = page (",\"match\":\"fabfilter pro\"");
        check ((int) two.getProperty ("total", 0) == 2, "every token must appear: \"fabfilter pro\" -> 2",
               two.getProperty ("total", 0).toString());

        const auto p2 = page (",\"offset\":400,\"limit\":100");
        check ((int) p2.getProperty ("offset", -1) == 400 && (int) p2.getProperty ("returned", 0) == 8
               && ! (bool) p2.getProperty ("hasMore", true),
               "offset + limit walk to the end of the list",
               p2.getProperty ("returned", 0).toString());

        // The compatibility fact the OLD agent_executor_read_guard holds, asserted here too so the two guards
        // cannot drift apart: before paging, `count` was set before fitUnder8K halved `names`, so `count` has
        // always meant how many are INSTALLED and `truncated` meant "these names are only a slice". A page must
        // not quietly redefine either. (RED as it stood: count was the page length and truncated was gone.)
        check ((int) all.getProperty ("count", -1) == 408,
               "`count` keeps its old meaning - the whole list, not this page",
               all.getProperty ("count", -1).toString());
        check ((bool) all.getProperty ("truncated", false),
               "...and `truncated` keeps its old meaning - you are not seeing all of it");
        check ((int) comp.getProperty ("count", -1) == 3 && ! (bool) comp.getProperty ("truncated", false),
               "...while a page that IS the whole match is not marked truncated",
               comp.getProperty ("count", -1).toString());
        check ((int) all.getProperty ("returned", -1) == 60 && (int) all.getProperty ("total", -1) == 408,
               "...so `returned` is the only field that means the page length");

        const auto none = page (",\"match\":\"nosuchthing\"");
        const auto o = run (ex, "look", call ("look", "{\"what\":\"inventory\",\"match\":\"nosuchthing\"}"));
        check (o.ok, "a filter that matches nothing is a RESULT, not a failure to retry");
        check ((int) none.getProperty ("total", -1) == 0, "...with total 0", none.getProperty ("total", -1).toString());
    }

    // ---------------------------------------------------------------- L8
    std::printf ("== L8: item 10, the rack OUT has an op of its own ==\n");
    {
        {   // a delta, which is what "turn it down 2 dB" means
            FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", "{\"op\":\"set_rack_out\",\"delta\":-2.0}"));
            check (o.ok, "set_rack_out applies", o.errorMessage);
            check (lg.rackOut == 1 && lg.chainOps == 0,
                   "...through the RACK-OUT sink and never as a chain op  (RED as it stood: there was no such "
                   "op, so the server dialled the EQ's output_db)");
            check (std::abs (lg.lastDb + 2.0) < 1e-9 && ! lg.lastAbsolute, "...as a DELTA of -2.0 dB",
                   juce::String (lg.lastDb, 2) + (lg.lastAbsolute ? " absolute" : " delta"));
        }
        {   // an absolute set
            FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", "{\"op\":\"set_rack_out\",\"db\":-4.5}"));
            check (o.ok && lg.rackOut == 1 && lg.lastAbsolute && std::abs (lg.lastDb + 4.5) < 1e-9,
                   "db alone is an ABSOLUTE gain", juce::String (lg.lastDb, 2));
        }
        {   // it obeys the cross-channel gate like every other mutating op
            FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", "{\"op\":\"set_rack_out\",\"delta\":-2.0,\"channel\":\"uid bb411b8240\"}"));
            check (! o.ok && o.errorCode == "needs_confirmation" && lg.rackOut == 0,
                   "a rack OUT on another channel is ask-first too", o.errorCode);
            const auto o2 = run (ex, "do", call ("do", "{\"op\":\"set_rack_out\",\"delta\":-2.0,"
                                                       "\"channel\":\"uid bb411b8240\",\"confirm\":true}"));
            check (o2.ok && lg.rackOut == 1 && lg.lastUid == "bb411b8240", "...and reaches that Link once confirmed", lg.lastUid);
        }
        {   // neither figure
            FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", "{\"op\":\"set_rack_out\"}"));
            check (! o.ok && o.errorMessage.contains ("db") && o.errorMessage.contains ("delta"),
                   "set_rack_out with no figure names both arguments", o.errorMessage);
        }
        {   // the refusal has to teach, or the next guess is output_db again
            FakeHost h; SinkLog lg; ExecutorDo ex (h.sources(), sinks (lg));
            const auto o = run (ex, "do", call ("do", "{\"op\":\"set_output_level\",\"db\":-2.0}"));
            check (! o.ok, "an op this build does not have is refused");
            check (o.errorMessage.contains ("set_rack_out"),
                   "...and the refusal NAMES the ops it does have, set_rack_out among them",
                   o.errorMessage.substring (0, 120));
        }
    }

    // ---------------------------------------------------------------- L9
    std::printf ("== L9: item 3b, markdown in a bubble ==\n");
    {
        auto plain = [] (const juce::String& md)
        {
            juce::AttributedString as;
            echojay::appendChatMarkdown (as, md, 12.0f, juce::Colours::white);
            return as.getText();
        };
        auto boldRuns = [] (const juce::String& md)
        {
            juce::AttributedString as;
            echojay::appendChatMarkdown (as, md, 12.0f, juce::Colours::white);
            int n = 0;
            for (int i = 0; i < as.getNumAttributes(); ++i) if (as.getAttribute (i).font.isBold()) ++n;
            return n;
        };
        check (plain ("a **bold** word") == "a bold word",
               "**bold** leaves no asterisks in the text  (RED as it stood: no bubble rendered markdown)",
               plain ("a **bold** word"));
        check (boldRuns ("a **bold** word") == 1, "...and exactly one run is bold",
               juce::String (boldRuns ("a **bold** word")));
        check (plain ("an *italic* word") == "an italic word", "*italic* too", plain ("an *italic* word"));
        check (plain ("an _italic_ word") == "an italic word", "_italic_ too", plain ("an _italic_ word"));
        check (plain ("a `code` word") == "a code word", "`code` too", plain ("a `code` word"));
        // THE PART THAT MATTERS MORE THAN THE EMPHASIS: nothing a model writes may eat a reply.
        check (plain ("2 * 3 = 6") == "2 * 3 = 6", "a lone asterisk stays a literal asterisk", plain ("2 * 3 = 6"));
        check (plain ("gain **down") == "gain **down", "an unclosed ** stays literal", plain ("gain **down"));
        // ---- SEAN'S RULING, 15 OCT 2026: THIS LEG HAD THE BUG WRITTEN DOWN AS CORRECT -------------
        // It asserted "a_b_c" -> "abc", i.e. that an underscore INSIDE A WORD is emphasis. He hit the
        // consequence in Logic: "88_Omnisphere_01" rendered as italic "88Omnisphere01", and the chat
        // has been misspelling the names of the tracks it is talking about ever since 3b landed. The
        // rule is CommonMark's own - `_` only marks at a word boundary, `*` may mark inside a word -
        // so the contract changed and this leg changed with it.
        check (plain ("a_b_c and x_y") == "a_b_c and x_y",
               "an underscore INSIDE a word is a literal character, not emphasis  (RED before this "
               "ruling: it rendered as \"abc and x_y\")", plain ("a_b_c and x_y"));
        check (plain ("88_Omnisphere_01 is loud") == "88_Omnisphere_01 is loud",
               "...so Sean's own track name survives the renderer, which is the case that found it",
               plain ("88_Omnisphere_01 is loud"));
        check (plain ("88_7even Kick_2_01 and Aitch_4_01") == "88_7even Kick_2_01 and Aitch_4_01",
               "...and so do the rest of them, in one string, with several underscores apiece",
               plain ("88_7even Kick_2_01 and Aitch_4_01"));
        check (plain ("an _italic_ word") == "an italic word",
               "...while a REAL _italic_ at a word boundary still renders - proven in both directions, "
               "so the rule is neither unfalsifiable nor unpassable", plain ("an _italic_ word"));
        check (plain ("snake_case_name") == "snake_case_name",
               "...and a three-part snake_case name keeps all of its underscores",
               plain ("snake_case_name"));
        check (plain ("") == "", "an empty reply renders nothing");
        check (boldRuns ("**a** and **b**") == 2, "two bold runs", juce::String (boldRuns ("**a** and **b**")));
    }

    std::printf ("==== agent_round13_guard: %s (%d assertion(s) failed, %d passed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes);
    return failures == 0 ? 0 : 1;
}
