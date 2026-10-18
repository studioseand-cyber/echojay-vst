// agent_executor_read_guard - THE READ-ONLY EXECUTOR AGAINST A FAKE HOST (Session A2, 9 Oct 2026).
//
// EJAgentExecutorRead answers look / check / captureCheckpoint / the playback window through `Sources`, a set of
// functions. Here every source is a fake the guard owns (a rack, a registry, two analyses, two tallies, a window
// recorder), so each leg asserts the SHAPE and the COMPACTNESS the contract names, not a measurement:
//   G1   startContext: channel {uid:"self", name, kind, links[]}, capabilities, agentMode; a Link target names the Link
//   G2   look(rack) / get_rack: 1-based n, name/bypassed/wet/keepLevel/builtin/settings/inDb/outDb/grDb; settings capped;
//        the LEVELLING RECORD rides at rack level {option, targetLufs, drives, landedGainDb, in/out, converged} and
//        NO slot is an EchoJay Level (Sean, 10 Oct: the slot is dropped)
//   G3   look(rack, channel:<uid>) reads the Link's sidecar; an unknown uid -> unknown_channel
//   G4   look(channel) / list_tracks: the registry rows with connected / audio / gone / placement
//   G5   look(analysis) / analyse: the numbers + at most six bands; no audio -> not_playing with the hint
//   G6   look(levels): per slot in/out/GR or "none", and the chain in/out/delta
//   G7   look(inventory): 400 names -> under 8 KB, truncated:true, count 400; unbound -> inventory_unavailable
//   G8   look(maps) -> server_tool; an unknown what -> unknown_what (both look and check)
//   C    look(inventory) FOR THE SERVER'S CENSUS (18 Oct 2026, B's FOR_A 76): a `tc_census_<n>` call gets the whole
//        list in one page and the whole cannot-run list by name, reason and declared type; every other call gets
//        exactly the 200 names and 8 KB it got yesterday; and an oversized census page loses names off the END so
//        offset/hasMore still describe what was sent
//   G9   check(level) / measure: the window's in/out/delta/peak; nothing heard -> not_playing
//   G10  check(gr, slot): the slot's GR; out of range -> unknown_slot; no reading -> not_playing
//   G11  check(true_peak): the last slot's peak + overs; a named slot
//   G12  check(spectrum): the bands; check(balance): Link target vs this mix bus, else no_mix_reading
//   G13  captureCheckpoint + compare_to_checkpoint: unchanged -> rackChanged:false; a bypass + a level move ->
//        rackChanged:true and deltaDb; an unknown token -> unknown_checkpoint; a Link target compares frames
//   G14  beginPlaybackWindow calls the window ONCE and never the song's reset; readPlayback reads the out tally;
//        a Link target counts heardSeconds beyond the base at window start
//   G15  doOp / undoStep / undoToCheckpoint -> not_in_phase ON THE READ-ONLY HALF (it must stay that way)
//   G17  A's MUTATING half: the switch governs do AND undo; one do = one command = one undo step; set_io may
//        carry both; set_level is the RACK RECORD; a refusal names the field or the op (10 Oct 2026)
//   G16  EVERY result in this guard, plus a 16-slot rack with 300-char settings, serialises under 8 KB
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJAgentExecutorRead.h"
#include "EJAgentExecutorDo.h"
#include <cmath>
#include <cstdio>

using namespace echojay::agent;

namespace
{
int failures = 0, passes = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok  %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("FAIL  %s%s\n", what.toRawUTF8(), detail.isNotEmpty() ? (" -- " + detail).toRawUTF8() : ""); }
}

ToolCall call (const juce::String& name, const juce::String& argsJson)
{
    ToolCall c; c.id = "t"; c.name = name; c.args = juce::JSON::parse (argsJson); c.approval = "free"; return c;
}

struct FakeHost
{
    RackRead own, link;
    std::vector<TrackRead> tracks;
    AnalysisRead ownA, linkA;
    LevellingRead ownL, linkL;
    echojay::LevelTally::Snapshot inLoop, out;
    int windows = 0, songResets = 0;
    bool transportKnown = true, playing = true;
    juce::StringArray inventory;
    bool bindInventory = true;
    juce::var notLoadable;              // [{name, loadable:false, why, type?}], void = not bound

    FakeHost()
    {
        own.valid = true; own.name = "Mix Bus"; own.kind = "mix_bus"; own.revision = 7; own.masterWet = 1.0f; own.preGainDb = -3.0f;
        // no EchoJay Level slot anywhere: levelling is a rack-level record (10 Oct 2026)
        const char* names[] = { "EchoJay EQ", "API-2500 (s)", "bx_saturator V2", "EchoJay Limiter" };
        for (int i = 0; i < 4; ++i)
        {
            SlotRead s; s.n = i + 1; s.name = names[i]; s.builtin = juce::String (names[i]).startsWith ("EchoJay");
            s.wet = i == 2 ? 0.15f : 1.0f; s.settings = i == 1 ? "ratio 4:1, attack 10 ms, release 300 ms" : "";
            s.dialSummary = i == 0 ? "bell 300 Hz -2.0 dB Q 1.4" : ""; s.preTrimDb = i == 1 ? -2.0f : 0.0f;
            s.pictureValid = i != 2; s.grKnown = i == 1; s.grDb = 2.4f; s.inLufs = -16.0f - i; s.outLufs = -16.5f - i; s.inTpDb = -4.0f; s.outTpDb = -4.5f;
            // SEAN'S CHECKS 2 AND 3 (15 Oct 2026): the slot that reports GR also has a LISTEN WINDOW,
            // so the peak and the typical are proven present and named. Put here rather than on a new
            // fifth slot deliberately: a fifth slot moves "the last slot", the rack's size and the
            // "1..4" range that four other legs assert, and proves nothing extra.
            // ---- SEAN'S RULING, 15 Oct 2026: NO ESTIMATES, EVER, AND THREE FIGURES NOT TWO -------
            // A figure is given only for an EchoJay device or a plugin CERTIFIED in its map (it passed
            // the EJ Maps compressor sweep). This slot models a third party that HAS passed it, which is
            // the only shape in which a third party may report one at all; the UNCERTIFIED case gets its
            // own leg (G10a3), because a new refusal with no test is how the -200 sentinel came to be
            // printed as a measurement.
            //
            // And the window is three figures: the LOUDEST HITS (95th percentile, the reported figure),
            // the TYPICAL (median), and the SPIKE max (one transient instant, kept for warnings and
            // never quoted) - "the absolute-max peak (8 dB) is a single transient instant nobody can see
            // on the meter; I see 2-4, hitting 5." The read must preserve spike >= loudest >= typical.
            if (i == 1)
            {
                s.grCertified   = true;     // see the note above: a swept third party
                s.grPeakDb      = 5.1f;     // the SPIKE max: one transient instant, never quoted
                s.grLoudestDb   = 4.2f;     // the 95th percentile - THE figure to report
                s.grTypicalDb   = 2.8f;     // the median - where the needle sits
                s.grHeldDb      = 3.9f;     // the 2 s hold the tile and the device meter both draw
                s.grWindowFrames = 412800.0;
            }
            own.slots.push_back (s);
        }
        ownL.present = true; ownL.option = "dynamic"; ownL.targetLufs = -12.0f; ownL.drives = "limiter_in"; ownL.landedGainDb = 3.3f;
        ownL.inLufs = -15.3f; ownL.outLufs = -12.1f; ownL.converged = true; ownL.state = "landed";
        link.valid = true; link.remote = true; link.uid = "lnk_kick"; link.name = "Kick"; link.kind = "channel"; link.revision = 3;
        { SlotRead s; s.n = 1; s.name = "UAD 1176"; s.settings = "fast attack"; link.slots.push_back (s); }
        { SlotRead s; s.n = 2; s.name = "EchoJay EQ"; s.builtin = true; link.slots.push_back (s); }
        linkL.present = true; linkL.option = "match"; linkL.drives = "rack_out"; linkL.landedGainDb = -1.2f; linkL.inLufs = -20.0f; linkL.outLufs = -20.3f; linkL.converged = true;
        TrackRead t1; t1.uid = "lnk_kick"; t1.name = "Kick"; t1.connected = true; t1.audioFlowing = true; t1.channels = 2; t1.placement = 2;
        TrackRead t2; t2.uid = "lnk_vox"; t2.name = "Lead Vocal"; t2.connected = true; t2.audioFlowing = false; t2.fresh = false; t2.gainDb = -1.5f;
        tracks = { t1, t2 };
        ownA.valid = true; ownA.integratedLufs = -14.7f; ownA.shortTermMaxLufs = -11.9f; ownA.truePeakMaxDb = -0.8f; ownA.psrDb = 8.2f; ownA.oversCount = 3;
        ownA.haveBands = true; ownA.bandRelDb = { 2.1f, 1.0f, -0.5f, -1.2f, -0.9f, -0.5f }; ownA.heardSeconds = 42.0f; ownA.playing = true; ownA.transportKnown = true;
        linkA.valid = true; linkA.integratedLufs = -20.3f; linkA.shortTermMaxLufs = -17.0f; linkA.truePeakMaxDb = -3.1f; linkA.haveBands = true;
        linkA.bandRelDb = { 6.0f, 2.0f, -1.0f, -3.0f, -2.0f, -2.0f }; linkA.heardSeconds = 30.0f; linkA.playing = true;
        inLoop.known = true; inLoop.kWeighted = true; inLoop.levelDb = -15.3f; inLoop.heardSeconds = 12.0f;
        out.known = true; out.kWeighted = true; out.levelDb = -12.1f; out.heardSeconds = 12.0f; out.heardAboveSeconds = 11.5f; out.truePeakDb = -0.2f; out.maxShortTermDb = -9.8f;
        for (int i = 0; i < 400; ++i) inventory.add ("Plugin Number " + juce::String (i) + " (s)");
    }

    Sources sources()
    {
        Sources s;
        s.ownName = [this] { return own.name; };
        s.ownKind = [this] { return own.kind; };
        s.projectName = [] { return juce::String ("Guard Song"); };
        s.ownRack = [this] { return own; };
        s.linkRack = [this] (const juce::String& uid) { if (uid == link.uid) return link; RackRead r; r.why = "no Link with uid \"" + uid + "\" is in the registry"; return r; };
        s.tracks = [this] { return tracks; };
        s.ownAnalysis = [this] { return ownA; };
        s.linkAnalysis = [this] (const juce::String& uid) { if (uid == link.uid) return linkA; AnalysisRead a; a.why = "no Link with uid \"" + uid + "\""; return a; };
        s.ownLevelling = [this] { return ownL; };
        s.linkLevelling = [this] (const juce::String& uid) { return uid == link.uid ? linkL : LevellingRead(); };
        s.chainInLoop = [this] { return inLoop; };
        s.chainOut = [this] { return out; };
        s.beginWindow = [this] { ++windows; };
        s.transportKnown = [this] { return transportKnown; };
        s.transportPlaying = [this] { return playing; };
        s.chainRevision = [this] { return own.revision; };
        if (bindInventory) s.inventory = [this] { return inventory; };
        // 18 Oct 2026 (FOR_A 76): bound only when a leg sets it, so every leg written before the
        // cannot-run list existed asks exactly the question it asked then.
        if (! notLoadable.isVoid()) s.notLoadable = [this] { return notLoadable; };
        return s;
    }
};

ToolOutcome run (ToolExecutor& ex, const char* tool, const ToolCall& c)
{
    ToolOutcome got; bool fired = false;
    auto done = [&] (ToolOutcome o) { got = std::move (o); fired = true; };
    if (juce::String (tool) == "look") ex.look (c, done); else if (juce::String (tool) == "check") ex.check (c, done); else ex.doOp (c, done);
    if (! fired) { got = ToolOutcome::failure ("no_completion", "the executor never called done"); }
    return got;
}
int bytesOf (const juce::var& v) { return juce::JSON::toString (v, true).getNumBytesAsUTF8(); }
int maxBytesSeen = 0;
ToolOutcome runCounted (ToolExecutor& ex, const char* tool, const ToolCall& c)
{
    auto o = run (ex, tool, c);
    if (o.ok) maxBytesSeen = juce::jmax (maxBytesSeen, bytesOf (o.result));
    return o;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_agent_executor_read_guard");
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("agent_executor_read_guard - the read-only executor against a fake host\n\n");

    FakeHost fh;
    ExecutorRead ex (fh.sources());

    // ---- G1 startContext ------------------------------------------------------------------------------------------
    {
        auto ctx = ex.startContext();
        maxBytesSeen = juce::jmax (maxBytesSeen, bytesOf (ctx));
        auto ch = ctx.getProperty ("channel", {});
        check (ch.getProperty ("uid", {}).toString() == "self" && ch.getProperty ("name", {}).toString() == "Mix Bus" && ch.getProperty ("kind", {}).toString() == "mix_bus"
               && ch.getProperty ("links", {}).getArray() != nullptr && ch.getProperty ("links", {}).getArray()->size() == 2
               && (bool) ctx.getProperty ("agentMode", {}) && ctx.getProperty ("capabilities", {}).getArray() != nullptr && ctx.getProperty ("project", {}).toString() == "Guard Song",
               "G1 startContext: channel {uid:self, name, kind, links[2]}, capabilities, agentMode:true, project");
        check (ctx.getProperty ("levelling", {}).getProperty ("option", {}).toString() == "dynamic" && std::abs ((double) ctx.getProperty ("levelling", {}).getProperty ("targetLufs", {}) + 12.0) < 0.01,
               "G1c ...and the rack-level levelling record {option, targetLufs}");
        ex.setTarget ("lnk_kick");
        auto ctx2 = ex.startContext();
        check (ctx2.getProperty ("channel", {}).getProperty ("uid", {}).toString() == "lnk_kick" && ctx2.getProperty ("channel", {}).getProperty ("name", {}).toString() == "Kick",
               "G1b with a Link target the channel is the Link");
        ex.setTarget ({});
    }
    // ---- G2 look(rack) --------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "look", call ("look", R"({"what":"rack","channel":"self"})"));
        auto* slots = o.result.getProperty ("slots", {}).getArray();
        check (o.ok && slots != nullptr && slots->size() == 4 && (int) (*slots)[0].getProperty ("n", {}) == 1 && (int) (*slots)[3].getProperty ("n", {}) == 4,
               "G2a look(rack): one line per slot, n is 1-based");
        {
            bool anyLevelSlot = false;
            for (auto& sv : *slots) if (sv.getProperty ("name", {}).toString() == "EchoJay Level" || sv.getProperty ("role", {}).toString() == "level") anyLevelSlot = true;
            const auto lv = o.result.getProperty ("levelling", {});
            check (! anyLevelSlot && lv.getProperty ("option", {}).toString() == "dynamic" && std::abs ((double) lv.getProperty ("targetLufs", {}) + 12.0) < 0.01
                   && lv.getProperty ("drives", {}).toString() == "limiter_in" && std::abs ((double) lv.getProperty ("landedGainDb", {}) - 3.3) < 0.01
                   && std::abs ((double) lv.getProperty ("inLufs", {}) + 15.3) < 0.01 && std::abs ((double) lv.getProperty ("outLufs", {}) + 12.1) < 0.01
                   && std::abs ((double) lv.getProperty ("deltaDb", {}) - 3.2) < 0.01 && (bool) lv.getProperty ("converged", {}) && lv.getProperty ("state", {}).toString() == "landed",
                   "G2h the LEVELLING RECORD rides at rack level {option, targetLufs, drives, landedGainDb, in/out, delta, converged, state}; no slot is a Level");
        }
        const auto s0 = (*slots)[0], s1 = (*slots)[1], s2 = (*slots)[2], s3 = (*slots)[3];
        check (s0.getProperty ("name", {}).toString() == "EchoJay EQ" && (bool) s0.getProperty ("builtin", {}) && s0.getProperty ("role", {}).toString() == "eq"
               && s0.getProperty ("settings", {}).toString() == "bell 300 Hz -2.0 dB Q 1.4",
               "G2b a built-in carries builtin:true, a coarse role, and the dial summary as its settings");
        check (s1.getProperty ("settings", {}).toString() == "ratio 4:1, attack 10 ms, release 300 ms" && ! s1.hasProperty ("role") && std::abs ((double) s1.getProperty ("inDb", {}) + 2.0) < 0.01
               && std::abs ((double) s1.getProperty ("grNowDb", {}) - 2.4) < 0.01,
               "G2c a third-party slot carries its prose settings, its IN trim and its GR, and no guessed role");
        // SEAN'S CHECKS 2 AND 3 (15 Oct 2026): the momentary figure is grNowDb now, in BOTH reads. It
        // was `grDb`, and `grDb` is the name that kept being quoted as what a compressor is doing -
        // one instant read in the release tail. Retiring the name from look(rack) as well as from
        // check(gr) is deliberate: one quantity with two names is how that mistake survives a fix.
        check ((int) s2.getProperty ("wet", {}) == 15 && (int) s0.getProperty ("wet", {}) == 100
               && ! s3.hasProperty ("grNowDb") && ! s2.hasProperty ("grNowDb")
               && ! s1.hasProperty ("grDb"),
               "G2d wet is a percentage; a slot with no GR reading carries no grNowDb - and the old "
               "`grDb` name is gone from every slot, so nothing can quote it by habit");
        check (o.result.getProperty ("channel", {}).getProperty ("kind", {}).toString() == "mix_bus" && (int) o.result.getProperty ("revision", {}) == 7
               && (int) o.result.getProperty ("masterWet", {}) == 100 && std::abs ((double) o.result.getProperty ("preGainDb", {}) + 3.0) < 0.01 && (bool) o.result.getProperty ("remote", {}) == false,
               "G2e channel kind, revision, masterWet, preGainDb, remote:false");
        auto alias = runCounted (ex, "look", call ("look", R"({"what":"get_rack"})"));
        check (alias.ok && juce::JSON::toString (alias.result, true) == juce::JSON::toString (o.result, true), "G2f get_rack is the same answer as rack");
        fh.own.slots[1].settings = juce::String::repeatedString ("a very long settings line ", 20);
        auto capped = runCounted (ex, "look", call ("look", R"({"what":"rack"})"));
        check (capped.ok && (*capped.result.getProperty ("slots", {}).getArray())[1].getProperty ("settings", {}).toString().length() <= ExecutorRead::kMaxSettingsChars,
               "G2g a long settings string is capped");
        fh.own.slots[1].settings = "ratio 4:1, attack 10 ms, release 300 ms";
    }
    // ---- G3 a Link's rack ----------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "look", call ("look", R"({"what":"rack","channel":"lnk_kick"})"));
        check (o.ok && (bool) o.result.getProperty ("remote", {}) && o.result.getProperty ("channel", {}).getProperty ("uid", {}).toString() == "lnk_kick"
               && o.result.getProperty ("slots", {}).getArray()->size() == 2 && (*o.result.getProperty ("slots", {}).getArray())[0].getProperty ("name", {}).toString() == "UAD 1176"
               && o.result.getProperty ("levelling", {}).getProperty ("option", {}).toString() == "match" && o.result.getProperty ("levelling", {}).getProperty ("drives", {}).toString() == "rack_out"
               && std::abs ((double) o.result.getProperty ("levelling", {}).getProperty ("landedGainDb", {}) + 1.2) < 0.01,
               "G3a look(rack, channel:<uid>) reads the Link's sidecar: remote:true, its slots, its rack-level levelling (match drives the rack OUT)");
        auto bad = run (ex, "look", call ("look", R"({"what":"rack","channel":"lnk_nope"})"));
        check (! bad.ok && bad.errorCode == "unknown_channel" && bad.errorMessage.contains ("lnk_nope"), "G3b an unknown uid -> unknown_channel naming it");
        ex.setTarget ("lnk_kick");
        auto viaTarget = runCounted (ex, "look", call ("look", R"({"what":"rack"})"));
        check (viaTarget.ok && (bool) viaTarget.result.getProperty ("remote", {}), "G3c with a Link target, channel:self means the Link");
        ex.setTarget ({});
    }
    // ---- G4 channel / list_tracks ------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "look", call ("look", R"({"what":"channel"})"));
        auto* links = o.result.getProperty ("links", {}).getArray();
        check (o.ok && o.result.getProperty ("uid", {}).toString() == "self" && links != nullptr && links->size() == 2
               && (*links)[0].getProperty ("uid", {}).toString() == "lnk_kick" && (bool) (*links)[0].getProperty ("audio", {}) && (*links)[0].getProperty ("placement", {}).toString() == "insert"
               && (bool) (*links)[1].getProperty ("gone", {}) && std::abs ((double) (*links)[1].getProperty ("gainDb", {}) + 1.5) < 0.01,
               "G4a look(channel): uid, name, kind, the registry rows with audio / placement / gone / gainDb");
        auto t = runCounted (ex, "look", call ("look", R"({"what":"list_tracks"})"));
        check (t.ok && (int) t.result.getProperty ("count", {}) == 2 && t.result.getProperty ("tracks", {}).getArray()->size() == 2, "G4b list_tracks: {count, tracks[]}");
        auto lk = runCounted (ex, "look", call ("look", R"({"what":"channel","channel":"lnk_kick"})"));
        check (lk.ok && lk.result.getProperty ("name", {}).toString() == "Kick" && lk.result.getProperty ("kind", {}).toString() == "channel", "G4c look(channel, <uid>) names the Link");
        // ---- G4d/G4e BUSY IS NOT GONE (18 Oct 2026, queue item 13) --------------------------------------
        // A Link rebuilding its rack answers nothing for a few seconds. Reported as `gone`, that became the
        // agent telling a user "88_7even Kick_2_01 isn't reachable from this session right now (it shows as
        // gone)" about a track whose chain it had just built. The two flags are mutually exclusive by
        // construction, so no reader has to decide which to believe.
        {
            TrackRead b; b.uid = "lnk_bz"; b.name = "Kick"; b.connected = true; b.busy = true; b.fresh = false;
            TrackRead g; g.uid = "lnk_gg"; g.name = "Old"; g.connected = true; g.fresh = false;
            TrackRead h; h.uid = "lnk_hh"; h.name = "Here"; h.connected = true;
            // THE TEMPORARY-VAR POINTER RULE (18 Sep 2026, a Pro Tools crash): getArray() on a var that
            // dies at the end of the expression hands back a pointer into the dead object. The first cut of
            // this leg did exactly that and read rubbish. The var is held.
            const auto tv = ExecutorRead::tracksVar ({ b, g, h });
            auto* arr = tv.getArray();
            const bool shapes = arr != nullptr && arr->size() == 3
                && (bool) (*arr)[0].getProperty ("busy", {}) && ! (*arr)[0].hasProperty ("gone")
                && (bool) (*arr)[1].getProperty ("gone", {}) && ! (*arr)[1].hasProperty ("busy")
                && ! (*arr)[2].hasProperty ("busy") && ! (*arr)[2].hasProperty ("gone");
            check (shapes, "G4d a rebuilding Link reads busy and NOT gone; a silent one still reads gone; a "
                           "live one carries neither", juce::JSON::toString (tv, true));
        }
    }
    // ---- G5 analysis ----------------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "look", call ("look", R"({"what":"analysis"})"));
        auto* bands = o.result.getProperty ("spectrum", {}).getArray();
        check (o.ok && std::abs ((double) o.result.getProperty ("integratedLufs", {}) + 14.7) < 0.01 && std::abs ((double) o.result.getProperty ("loudest3sLufs", {}) + 11.9) < 0.01
               && std::abs ((double) o.result.getProperty ("peakDbtp", {}) + 0.8) < 0.01 && std::abs ((double) o.result.getProperty ("psrDb", {}) - 8.2) < 0.01
               && (int) o.result.getProperty ("oversCount", {}) == 3 && (bool) o.result.getProperty ("playing", {}) && std::abs ((double) o.result.getProperty ("heardSeconds", {}) - 42.0) < 0.01,
               "G5a look(analysis): integrated, loudest 3 s, peak, PSR, overs, heard, playing");
        check (bands != nullptr && bands->size() == 6 && (*bands)[0].getProperty ("band", {}).toString() == "sub" && std::abs ((double) (*bands)[0].getProperty ("vsAverageDb", {}) - 2.1) < 0.01
               && (*bands)[5].getProperty ("band", {}).toString() == "air",
               "G5b ...and at most six bands as {band, vsAverageDb}, never an array of frames");
        auto alias = runCounted (ex, "look", call ("look", R"({"what":"analyse"})"));
        check (alias.ok && juce::JSON::toString (alias.result, true) == juce::JSON::toString (o.result, true), "G5c analyse is the same answer as analysis");
        fh.ownA.valid = false; fh.ownA.why = "no audio has been heard yet";
        auto np = run (ex, "look", call ("look", R"({"what":"analysis"})"));
        check (! np.ok && np.errorCode == "not_playing" && np.result.getProperty ("hint", {}).toString().contains ("wait_for_playback"), "G5d no audio -> not_playing with the wait_for_playback hint");
        fh.ownA.valid = true;
        auto la = runCounted (ex, "look", call ("look", R"({"what":"analysis","channel":"lnk_kick"})"));
        check (la.ok && std::abs ((double) la.result.getProperty ("integratedLufs", {}) + 20.3) < 0.01, "G5e a Link's analysis comes from its frame");
    }
    // ---- G6 levels ------------------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "look", call ("look", R"({"what":"levels"})"));
        auto* slots = o.result.getProperty ("slots", {}).getArray();
        auto chain = o.result.getProperty ("chain", {});
        check (o.ok && slots != nullptr && slots->size() == 4 && std::abs ((double) (*slots)[1].getProperty ("grNowDb", {}) - 2.4) < 0.01
               && (*slots)[2].getProperty ("reading", {}).toString() == "none"
               && std::abs ((double) chain.getProperty ("inLufs", {}) + 15.3) < 0.01 && std::abs ((double) chain.getProperty ("outLufs", {}) + 12.1) < 0.01
               && std::abs ((double) chain.getProperty ("deltaDb", {}) - 3.2) < 0.01,
               "G6 look(levels): per-slot readings or \"none\", and the chain in/out/delta from the loop's tallies");
    }
    // ---- G7 inventory --------------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "look", call ("look", R"({"what":"inventory"})"));
        check (o.ok && (int) o.result.getProperty ("count", {}) == 400 && bytesOf (o.result) <= ExecutorRead::kMaxResultBytes && (bool) o.result.getProperty ("truncated", {})
               && o.result.getProperty ("names", {}).getArray()->size() < 400 && o.result.getProperty ("names", {}).getArray()->size() > 50,
               "G7a 400 installed names: under 8 KB, truncated:true, count still 400, a useful number kept", juce::String (bytesOf (o.result)));
        FakeHost noInv; noInv.bindInventory = false;
        ExecutorRead ex2 (noInv.sources());
        auto u = run (ex2, "look", call ("look", R"({"what":"inventory"})"));
        check (! u.ok && u.errorCode == "inventory_unavailable", "G7b an unbound inventory says so rather than answering empty");
    }
    // ---- C: THE SERVER'S INVENTORY CENSUS (18 Oct 2026, B's FOR_A 76) --------------------------------------------------
    //
    // B: *"'what compressors do I have?' - the answer was the 60-name page's count ('48 of them... and 38 more').
    // With your page cap of 200 it takes eight pages on Sean's 1,577-name Mac (nine plugin steps). Honour `limit` up
    // to 2000 on look(inventory) when the call id starts with tc_census_. And the cannot-run list by name, whole:
    // the pages name at most twelve and declare notLoadableTotal; on Sean's run they named none (total 180)."*
    //
    // The fixture is that machine: 1,577 installed names, and 180 that cannot run NONE of which are in the
    // inventory - which is the real shape, because `names` is what can be offered and a plugin that cannot run is
    // withheld from it. That is why the page-scoped field named none of 180 rather than twelve of them.
    {
        auto callAs = [] (const juce::String& id, const juce::String& argsJson)
        { ToolCall c; c.id = id; c.name = "look"; c.args = juce::JSON::parse (argsJson); c.approval = "free"; return c; };

        FakeHost big;
        // CLEARED FIRST: the fixture ships 400 names for G7, and adding to them made this leg's own
        // figures 1,977 - a number the leg then asserted against 1,577 and called the product wrong.
        big.inventory.clear();
        for (int i = 1; i <= 1577; ++i)
            big.inventory.add ("Maker" + juce::String (i % 40) + " Thing " + juce::String (i));
        check (big.inventory.size() == 1577,
               "C0 PRECONDITION: the fixture is his machine's size, 1,577 installed names",
               juce::String (big.inventory.size()));
        {
            juce::Array<juce::var> bad;
            for (int i = 1; i <= 180; ++i)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("name", "Cannot Run " + juce::String (i));     // deliberately NOT in `inventory`
                o->setProperty ("loadable", false);
                o->setProperty ("why", "its VST3 has no arm64 build");
                o->setProperty ("type", "Dynamics (VST3)");
                bad.add (juce::var (o));
            }
            big.notLoadable = juce::var (bad);
        }
        ExecutorRead exb (big.sources());

        // ---- C1/C4: AN ORDINARY CALL IS UNTOUCHED ------------------------------------------------------------
        auto plain = run (exb, "look", callAs ("tc_7", R"({"what":"inventory","limit":2000})"));
        check (plain.ok && (int) plain.result.getProperty ("limit", {}) == ExecutorRead::kInventoryPageMax
               && bytesOf (plain.result) <= ExecutorRead::kMaxResultBytes
               && (bool) plain.result.getProperty ("hasMore", {}),
               "C1 an ORDINARY call asking for 2000 still gets 200 and still fits 8 KB - the census is the only "
               "thing that changed, because an ordinary caller is still behind the server's own trim",
               "limit=" + plain.result.getProperty ("limit", {}).toString() + " bytes=" + juce::String (bytesOf (plain.result)));
        check (plain.ok && (int) plain.result.getProperty ("notLoadableTotal", {}) == 180
               && plain.result.getProperty ("notLoadable", {}).getArray() == nullptr,
               "C4 ...and its cannot-run field is page-scoped as before: the SCALE as a number, and no names, "
               "because a plugin that cannot run is not in the page (this is what named none of 180)",
               plain.result.getProperty ("notLoadable", {}).getArray() == nullptr
                 ? juce::String ("absent, total=") + plain.result.getProperty ("notLoadableTotal", {}).toString()
                 : juce::String ("present"));

        // ---- C2: ONE PAGE, ONE STEP ---------------------------------------------------------------------------
        auto cen = run (exb, "look", callAs ("tc_census_1", R"({"what":"inventory","limit":2000,"offset":0})"));
        const auto* cn = cen.result.getProperty ("names", {}).getArray();
        check (cen.ok && (int) cen.result.getProperty ("limit", {}) == 2000
               && cn != nullptr && cn->size() == 1577
               && (int) cen.result.getProperty ("returned", {}) == 1577
               && (int) cen.result.getProperty ("total", {}) == 1577
               && ! (bool) cen.result.getProperty ("hasMore", {}),
               "C2 a tc_census_ call gets all 1,577 names in ONE page - eight pages and nine plugin steps become "
               "one (RED as it stood: the limit was capped at 200 and the 8 KB gate halved even that)",
               "returned=" + cen.result.getProperty ("returned", {}).toString()
                 + " hasMore=" + cen.result.getProperty ("hasMore", {}).toString());
        check (cen.ok && bytesOf (cen.result) > ExecutorRead::kMaxResultBytes
               && bytesOf (cen.result) <= ExecutorRead::kCensusMaxBytes,
               "C2b ...so the page is deliberately over the 8 KB contract cap, and still inside the census "
               "ceiling the server allows - both halves asserted, because either alone is half a rule",
               juce::String (bytesOf (cen.result)) + " bytes");

        // ---- C3: THE CANNOT-RUN LIST, BY NAME AND TYPE --------------------------------------------------------
        const auto* bad = cen.result.getProperty ("notLoadable", {}).getArray();
        bool allNamed = bad != nullptr && bad->size() == 180, haveType = false, leaked = false;
        if (bad != nullptr)
            for (const auto& e : *bad)
            {
                if (e.getProperty ("name", {}).toString().isEmpty()
                    || e.getProperty ("why", {}).toString().isEmpty()) allNamed = false;
                if (e.getProperty ("type", {}).toString() == "Dynamics (VST3)") haveType = true;
                if (cn != nullptr && cn->contains (e.getProperty ("name", {}))) leaked = true;
            }
        check (allNamed && (int) cen.result.getProperty ("notLoadableTotal", {}) == 180,
               "C3 ...and all 180 that cannot run are NAMED on it, with the reason - not a count (RED as it "
               "stood: twelve at most, page-scoped, which on a real machine was none)",
               bad == nullptr ? juce::String ("absent") : juce::String (bad->size()) + " named");
        check (haveType,
               "C3b ...each with the type the plugin DECLARES (category and format), which is what the census "
               "needs to count them per type - the role stays the server's to decide from its maps");
        check (! leaked && cen.result.getProperty ("notLoadableNote", {}).toString()
                              .containsIgnoreCase ("not in `names`"),
               "C3c ...and the result says they are NOT in `names`, which is true here and is why they have to "
               "be counted separately rather than filtered out of the list");

        // ---- C5: AN OVERSIZED CENSUS LOSES NAMES OFF THE END, NOT HALF OF ITSELF -----------------------------
        // 2000 names of 400 characters is 800 KB, well past the ceiling. fitUnder8K would HALVE the array, after
        // which offset + returned stops describing the slice the server asked for and the next offset skips
        // names nobody ever saw.
        {
            FakeHost huge;
            huge.inventory.clear();                       // the same 400-name fixture, same reason
            for (int i = 0; i < 2000; ++i)
                huge.inventory.add (juce::String::repeatedString ("N", 390) + juce::String (i + 1000));
            ExecutorRead exh (huge.sources());
            auto cut = run (exh, "look", callAs ("tc_census_2", R"({"what":"inventory","limit":2000,"offset":0})"));
            const auto* kn = cut.result.getProperty ("names", {}).getArray();
            const int ret = (int) cut.result.getProperty ("returned", {});
            check (cut.ok && bytesOf (cut.result) <= ExecutorRead::kCensusMaxBytes
                   && ret > 0 && ret < 2000 && (bool) cut.result.getProperty ("hasMore", {})
                   && (int) cut.result.getProperty ("total", {}) == 2000,
                   "C5 a census page too big even for the ceiling is BOUNDED and says so - returned < total with "
                   "hasMore, so the server asks for the next offset exactly as it does today",
                   "returned=" + juce::String (ret) + " bytes=" + juce::String (bytesOf (cut.result)));
            check (kn != nullptr && kn->size() == ret
                   && (*kn)[0].toString() == huge.inventory[0]
                   && (*kn)[ret - 1].toString() == huge.inventory[ret - 1],
                   "C5b ...and what it kept is the FRONT of the slice, name for name - the trim comes off the "
                   "end, so offset + returned still names the boundary the next page starts at");
        }

        // ---- C6: THE CAP THE CLIENT ENFORCES COMES FROM THE SAME ONE FACT ------------------------------------
        check (ExecutorRead::isCensusCallId ("tc_census_1") && ExecutorRead::isCensusCallId ("TC_CENSUS_12")
               && ! ExecutorRead::isCensusCallId ("tc_7") && ! ExecutorRead::isCensusCallId ("census_1"),
               "C6 the census marker is the call id B stamps, read one way");
        // ASKED THROUGH ToolExecutor&, which is how the client holds it - the client knows nothing about
        // ExecutorRead, and a leg that reached past the base reference would be testing a road nobody takes.
        ToolExecutor& asClientSeesIt = exb;
        check (asClientSeesIt.maxResultBytes ("tc_census_1") == ExecutorRead::kCensusMaxBytes
               && asClientSeesIt.maxResultBytes ("tc_7") == ExecutorRead::kMaxResultBytes,
               "C6b ...and the executor tells the CLIENT the same ceiling it built the page under, through the "
               "base reference the client actually holds, so the last gate before the wire cannot halve a census "
               "answer back again (that gate is where this would otherwise have died)",
               juce::String (asClientSeesIt.maxResultBytes ("tc_census_1")) + " / "
                 + juce::String (asClientSeesIt.maxResultBytes ("tc_7")));
    }

    // ---- G8 server tools / unknown ---------------------------------------------------------------------------------------
    {
        auto m = run (ex, "look", call ("look", R"({"what":"maps","plugins":["x"]})"));
        auto u = run (ex, "look", call ("look", R"({"what":"weather"})"));
        auto c = run (ex, "check", call ("check", R"({"what":"vibes"})"));
        check (! m.ok && m.errorCode == "server_tool" && ! u.ok && u.errorCode == "unknown_what" && ! c.ok && c.errorCode == "unknown_what",
               "G8 look(maps) -> server_tool; an unknown what -> unknown_what on look and check");
    }
    // ---- G9 check(level) ---------------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "check", call ("check", R"({"what":"level"})"));
        check (o.ok && std::abs ((double) o.result.getProperty ("inLufs", {}) + 15.3) < 0.01 && std::abs ((double) o.result.getProperty ("outLufs", {}) + 12.1) < 0.01
               && std::abs ((double) o.result.getProperty ("deltaDb", {}) - 3.2) < 0.01 && std::abs ((double) o.result.getProperty ("outDbtp", {}) + 0.2) < 0.01
               && std::abs ((double) o.result.getProperty ("loudest3sLufs", {}) + 9.8) < 0.01 && o.result.getProperty ("source", {}).toString() == "chain_tallies",
               "G9a check(level): the window's in/out/delta, loudest 3 s and peak from the loop's tallies");
        check (o.result.getProperty ("option", {}).toString() == "dynamic" && std::abs ((double) o.result.getProperty ("targetLufs", {}) + 12.0) < 0.01
               && o.result.getProperty ("drives", {}).toString() == "limiter_in" && std::abs ((double) o.result.getProperty ("landedGainDb", {}) - 3.3) < 0.01 && (bool) o.result.getProperty ("converged", {}),
               "G9a2 ...plus the contract's option / targetLufs / converged from the RACK-LEVEL record, and what drives the gain and what landed");
        auto alias = runCounted (ex, "check", call ("check", R"({"what":"measure"})"));
        check (alias.ok && juce::JSON::toString (alias.result, true) == juce::JSON::toString (o.result, true), "G9b measure is the same answer as level");
        fh.out.known = false;
        auto np = run (ex, "check", call ("check", R"({"what":"level"})"));
        check (! np.ok && np.errorCode == "not_playing", "G9c nothing heard -> not_playing");
        fh.out.known = true;
        ex.setTarget ("lnk_kick");
        auto lk = runCounted (ex, "check", call ("check", R"({"what":"level"})"));
        check (lk.ok && lk.result.getProperty ("source", {}).toString() == "link_frame" && std::abs ((double) lk.result.getProperty ("outLufs", {}) + 20.3) < 0.01
               && lk.result.getProperty ("option", {}).toString() == "match" && lk.result.getProperty ("drives", {}).toString() == "rack_out",
               "G9d on a Link target the level comes from the Link's frame, and says so; its levelling from the Link's record");
        ex.setTarget ({});
    }
    // ---- G10 check(gr) --------------------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "check", call ("check", R"({"what":"gr","slot":2})"));
        // ---- SEAN'S CHECKS 2 AND 3 (15 Oct 2026): THE WINDOW, NOT THE INSTANT -------------------
        // His meter showed about 5 dB on the loud parts and the agent reported 0.1, then 2.1. The
        // figure was `grDb` - one float, read at whatever moment the tool call ran, which is the
        // release tail just after "Got it, you can stop". It is `grNowDb` now, explicitly labelled as
        // this instant and NOT the figure to quote, beside the window's peak and typical.
        // ---- SEAN'S #17 (16 Oct 2026): THE INSTANT IS NOT IN THE RESULT AT ALL ------------------
        // *"'loudest hits' never arrives ... then quoted 'the instant hitting up to 6 dB', which was
        // meant to stay internal."* A figure that must not be quoted has no business being sent, so
        // grNowDb and grSpikeMaxDb are gone from check(gr) - and this leg, which asserted the
        // momentary figure WAS there, had to move with the ruling. It asserts the opposite now, which
        // is the only form of this check that can fail on a build that leaks one.
        check (o.ok && (int) o.result.getProperty ("slot", {}) == 2
               && o.result.getProperty ("name", {}).toString() == "API-2500 (s)"
               && ! o.result.hasProperty ("grNowDb") && ! o.result.hasProperty ("grSpikeMaxDb"),
               "G10a check(gr, slot 2) names the slot and carries NO instantaneous or single-transient "
               "figure for anything to quote");
        // ...and the WINDOW is what it leads with: the peak and the typical, by name, with the
        // momentary value kept and labelled. This is the row Sean asked for - "return the peak and a
        // typical value, named as such" - and it fails on any build that reports one moment.
        {
            // ---- SEAN'S RULING, 15 Oct 2026: THE LOUDEST HITS, NOT THE ABSOLUTE MAX --------------
            // This leg asserted grPeakDb as the headline figure. His ruling moved it: *"the absolute-max
            // 'peak' (8 dB) is a single transient instant nobody can see on the meter; I see 2-4, hitting
            // 5. Report 'loudest hits' as the 95th percentile of active blocks instead, keep the true max
            // internal for spike warnings only."* So the contract is THREE figures with an ORDER, and the
            // leg had to move with it - the old assertion would have failed on the fix, which is the
            // right way round but only if the leg is corrected rather than the product.
            const double loudest = (double) o.result.getProperty ("grLoudestDb", {});
            const double typ     = (double) o.result.getProperty ("grTypicalDb", {});
            const double held    = (double) o.result.getProperty ("grHeldDb", {});
            // THE OLD NAME NOW MEANS THE RIGHT FIGURE (Sean's #17): a prompt written against the old
            // contract asked for "the peak" and got the single transient, so grPeakDb and peakDb
            // alias the loudest hits. Asserted, because an alias nobody checks is an alias that rots.
            const double aliasPeak = (double) o.result.getProperty ("grPeakDb", {});
            const double aliasPk2  = (double) o.result.getProperty ("peakDb", {});
            const auto sentence  = o.result.getProperty ("sentence", {}).toString();
            const auto note      = o.result.getProperty ("note", {}).toString();
            check (std::abs (loudest - 4.2) < 0.01 && std::abs (typ - 2.8) < 0.01
                   && std::abs (held - 3.9) < 0.01
                   && std::abs (aliasPeak - loudest) < 0.01 && std::abs (aliasPk2 - loudest) < 0.01
                   && loudest >= typ
                   && o.result.getProperty ("measured", {}).toString().containsIgnoreCase ("listen window")
                   && sentence.containsIgnoreCase ("loudest hits")
                   && note.containsIgnoreCase ("loudest hits")
                   && note.containsIgnoreCase ("no instantaneous"),
                   "G10a2 check(gr) gives the LOUDEST HITS (95th percentile), the TYPICAL (median), "
                   "the meter's held figure and a ready sentence - with grPeakDb/peakDb aliasing the "
                   "loudest hits and NO instant anywhere  (RED before: the absolute max was the "
                   "headline, and then the figure was renamed out from under a prompt that asked for "
                   "a peak)",
                   "loudest " + juce::String (loudest, 2) + " typical " + juce::String (typ, 2)
                   + " held " + juce::String (held, 2) + " peak-alias " + juce::String (aliasPeak, 2)
                   + " sentence \"" + sentence + "\"");
        }
        // ---- G10a3: AN UNCERTIFIED SLOT GETS NO FIGURE AT ALL (Sean's ruling, 15 Oct) -------------
        // *"NO estimates, ever... check(gr) returns a figure only for those; for anything else it
        // returns not_certified with no number."* The refusal is new, and a refusal with no test is how
        // the -200 sentinel came to be printed as a measurement. Proven BOTH ways on one slot: certified
        // above gives the figures, uncertified here gives a refusal with no number in it.
        {
            fh.own.slots[1].grCertified = false;
            auto nc = run (ex, "check", call ("check", R"({"what":"gr","slot":2})"));
            // NO FIGURE ANYWHERE - and "no digits" is the WRONG test for that, which I nearly shipped:
            // this slot is called "API-2500 (s)" and naming the plugin is exactly what the refusal must
            // do. What must not appear is a MEASUREMENT, so the test is for the unit.
            const bool noNumber = ! nc.result.hasProperty ("grLoudestDb")
                               && ! nc.result.hasProperty ("grTypicalDb")
                               && ! nc.result.hasProperty ("grNowDb")
                               && ! nc.result.hasProperty ("grHeldDb")
                               && ! nc.errorMessage.contains (" dB")
                               && ! nc.userMessage.contains (" dB");
            check (! nc.ok && nc.errorCode == "not_certified" && noNumber
                   && nc.errorMessage.containsIgnoreCase ("estimate")
                   && nc.userMessage.containsIgnoreCase ("guess"),
                   "G10a3 an UNCERTIFIED slot's gain reduction is refused as not_certified, with no "
                   "figure anywhere in the answer and no estimate offered  (Sean, 15 Oct: \"no "
                   "estimates, ever\" - a plausible figure nobody can check is worse than no figure)",
                   "code=" + nc.errorCode + " msg=\"" + nc.errorMessage.substring (0, 90) + "\"");
            fh.own.slots[1].grCertified = true;      // restored: later legs read this slot
        }
        auto oor = run (ex, "check", call ("check", R"({"what":"gr","slot":9})"));
        check (! oor.ok && oor.errorCode == "unknown_slot" && oor.errorMessage.contains ("1..4"), "G10b slot 9 of 4 -> unknown_slot naming the range");
        auto nr = run (ex, "check", call ("check", R"({"what":"gr","slot":1})"));
        check (! nr.ok && nr.errorCode == "not_playing", "G10c a slot with no GR reading -> not_playing");
    }
    // ---- G11 check(true_peak) --------------------------------------------------------------------------------------------------
    {
        auto o = runCounted (ex, "check", call ("check", R"({"what":"true_peak"})"));
        check (o.ok && (int) o.result.getProperty ("slot", {}) == 4 && o.result.getProperty ("name", {}).toString() == "EchoJay Limiter"
               && std::abs ((double) o.result.getProperty ("peakDbtp", {}) + 4.5) < 0.01 && (int) o.result.getProperty ("oversCount", {}) == 3,
               "G11a check(true_peak): the LAST slot's output peak and the overs");
        auto n2 = runCounted (ex, "check", call ("check", R"({"what":"true_peak","slot":2})"));
        check (n2.ok && (int) n2.result.getProperty ("slot", {}) == 2, "G11b ...or a named slot's");
    }
    // ---- G12 spectrum / balance --------------------------------------------------------------------------------------------------
    {
        auto sp = runCounted (ex, "check", call ("check", R"({"what":"spectrum"})"));
        check (sp.ok && sp.result.getProperty ("spectrum", {}).getArray() != nullptr && sp.result.getProperty ("spectrum", {}).getArray()->size() == 6, "G12a check(spectrum): the six bands");
        auto nb = run (ex, "check", call ("check", R"({"what":"balance"})"));
        check (! nb.ok && nb.errorCode == "no_mix_reading", "G12b balance with no Link target -> no_mix_reading");
        ex.setTarget ("lnk_kick");
        auto b = runCounted (ex, "check", call ("check", R"({"what":"balance"})"));
        check (b.ok && std::abs ((double) b.result.getProperty ("channelLufs", {}) + 20.3) < 0.01 && std::abs ((double) b.result.getProperty ("mixLufs", {}) + 14.7) < 0.01
               && std::abs ((double) b.result.getProperty ("deltaDb", {}) + 5.6) < 0.01,
               "G12c balance: the Link channel against this mix bus, two hosts' readings, deltaDb");
        ex.setTarget ({});
        fh.own.kind = "channel";
        ex.setTarget ("lnk_kick");
        auto nk = run (ex, "check", call ("check", R"({"what":"balance"})"));
        check (! nk.ok && nk.errorCode == "no_mix_reading", "G12d ...and not when this instance is not on the mix bus");
        fh.own.kind = "mix_bus"; ex.setTarget ({});
    }
    // ---- G13 checkpoints ---------------------------------------------------------------------------------------------------------------
    {
        const auto tok = ex.captureCheckpoint ("agent: build the mix bus");
        check (tok == "cp_1" && ex.checkpoint (tok) != nullptr && ex.checkpoint (tok)->rack.slots.size() == 4 && ex.checkpoint (tok)->out.known
               && ex.checkpoint (tok)->levelling.present && std::abs (ex.checkpoint (tok)->levelling.landedGainDb - 3.3f) < 0.01f,
               "G13a captureCheckpoint snapshots the rack, the out tally and the levelling record, keyed by a token");
        auto same = runCounted (ex, "check", call ("check", R"({"what":"compare_to_checkpoint","checkpoint":"cp_1"})"));
        check (same.ok && (bool) same.result.getProperty ("rackChanged", {}) == false && std::abs ((double) same.result.getProperty ("deltaDb", {})) < 0.01
               && same.result.getProperty ("slotsThen", {}).toString() == same.result.getProperty ("slotsNow", {}).toString(),
               "G13b nothing changed -> rackChanged:false, deltaDb 0, the same slot line twice");
        fh.own.slots[2].bypassed = true; fh.out.levelDb = -10.6f; fh.ownL.landedGainDb = 4.8f;
        auto diff = runCounted (ex, "check", call ("check", R"({"what":"compare_to_checkpoint"})"));
        check (diff.ok && (bool) diff.result.getProperty ("rackChanged", {}) && std::abs ((double) diff.result.getProperty ("deltaDb", {}) - 1.5) < 0.01
               && diff.result.getProperty ("slotsNow", {}).toString().contains ("(byp)") && ! diff.result.getProperty ("slotsThen", {}).toString().contains ("(byp)")
               && diff.result.getProperty ("checkpoint", {}).toString() == "cp_1",
               "G13c a bypass and a 1.5 dB level move -> rackChanged:true, deltaDb 1.5; no token means the newest checkpoint");
        check (std::abs ((double) diff.result.getProperty ("landedGainThenDb", {}) - 3.3) < 0.01 && std::abs ((double) diff.result.getProperty ("landedGainNowDb", {}) - 4.8) < 0.01
               && std::abs ((double) diff.result.getProperty ("landedGainDeltaDb", {}) - 1.5) < 0.01 && ! diff.result.hasProperty ("optionChanged"),
               "G13c2 ...and the levelling record's landed gain then / now / delta, from the rack-level record");
        fh.own.slots[2].bypassed = false; fh.out.levelDb = -12.1f; fh.ownL.landedGainDb = 3.3f;
        auto unk = run (ex, "check", call ("check", R"({"what":"compare_to_checkpoint","checkpoint":"cp_99"})"));
        check (! unk.ok && unk.errorCode == "unknown_checkpoint", "G13d an unknown token -> unknown_checkpoint");
        ex.setTarget ("lnk_kick");
        const auto tok2 = ex.captureCheckpoint ("agent: the kick");
        fh.linkA.integratedLufs = -18.3f;
        auto lc = runCounted (ex, "check", call ("check", R"({"what":"compare_to_checkpoint","checkpoint":"cp_2"})"));
        check (tok2 == "cp_2" && lc.ok && std::abs ((double) lc.result.getProperty ("deltaDb", {}) - 2.0) < 0.01 && (bool) lc.result.getProperty ("rackChanged", {}) == false,
               "G13e on a Link target the comparison is frame against frame");
        fh.linkA.integratedLufs = -20.3f; ex.setTarget ({});
    }
    // ---- G14 the playback window --------------------------------------------------------------------------------------------------------
    {
        ex.beginPlaybackWindow();
        check (fh.windows == 1 && fh.songResets == 0, "G14a beginPlaybackWindow opens the loop's window ONCE and never resets the song's integrated reading");
        auto r = ex.readPlayback();
        check (r.transportKnown && r.playing && std::abs (r.heardAboveSeconds - 11.5f) < 0.01f && std::abs (r.integratedLufs + 12.1f) < 0.01f
               && std::abs (r.loudestShortTermLufs + 9.8f) < 0.01f && std::abs (r.truePeakDbtp + 0.2f) < 0.01f,
               "G14b readPlayback reads the out tally: heardAbove, integrated, loudest 3 s, true peak, and the transport");
        ex.setTarget ("lnk_kick");
        ex.beginPlaybackWindow();
        check (fh.windows == 1, "G14c a Link target opens no local window (the Link's tally is its own)");
        auto r0 = ex.readPlayback();
        fh.linkA.heardSeconds = 34.5f;
        auto r1 = ex.readPlayback();
        check (std::abs (r0.heardAboveSeconds) < 0.01f && std::abs (r1.heardAboveSeconds - 4.5f) < 0.01f && std::abs (r1.integratedLufs + 20.3f) < 0.01f,
               "G14d ...and counts the frame's heardSeconds beyond the base at window start");
        fh.linkA.heardSeconds = 30.0f; ex.setTarget ({});
    }
    // ---- G15 the mutating half: STILL not_in_phase on the READ-ONLY executor ----------------------------------------------------------------
    // A2's ExecutorRead has not changed and must not: it is the half that cannot act, and a build where the
    // read-only executor started acting would be the whole safety story gone. A's half is a SUBCLASS and is
    // asserted separately in G17 below.
    {
        auto d = run (ex, "do", call ("do", R"({"op":"set","slot":1})"));
        ToolOutcome u1, u2; ex.undoStep ("u1", [&] (ToolOutcome o) { u1 = o; }); ex.undoToCheckpoint ("cp_1", [&] (ToolOutcome o) { u2 = o; });
        check (! d.ok && d.errorCode == "not_in_phase" && d.errorMessage.contains ("set") && ! u1.ok && u1.errorCode == "not_in_phase" && ! u2.ok && u2.errorCode == "not_in_phase",
               "G15 the READ-ONLY executor still answers not_in_phase for do / undoStep / undoToCheckpoint");
    }
    // ---- G17 A'S MUTATING HALF (10 Oct 2026) -------------------------------------------------------------------------------------------------
    // ExecutorDo subclasses ExecutorRead, so this also asserts the read half is INHERITED and not forked.
    {
        bool modeOn = false, ejOnly = false;
        juce::String sentUid; juce::var sentOps; int sends = 0;
        DoSinks sinks;
        sinks.agentModeOn = [&modeOn] { return modeOn; };
        sinks.echoJayOnly = [&ejOnly] { return ejOnly; };
        sinks.applyChainOps = [&] (const juce::String& uid, const juce::var& ops,
                                   std::function<void (bool, juce::String, juce::String)> cb)
        { sentUid = uid; sentOps = ops; ++sends; if (cb) cb (true, "sent to the rack", {}); };
        sinks.undoOne = [] (const juce::String&, std::function<void (bool, juce::String)> cb)
        { if (cb) cb (true, "undone"); };
        FakeHost dh;
        ExecutorDo xd (dh.sources(), sinks);

        // AGENT MODE OFF IS THE DEFAULT, AND IT REFUSES - with the sentence a user needs, naming the switch.
        // This is Sean's standing rule asserted as behaviour: off = chat and Apply, nothing changed.
        {
            auto d = run (xd, "do", call ("do", R"({"op":"set_pre_gain","db":-3})"));
            check (! d.ok && d.errorCode == "not_permitted" && d.errorMessage.containsIgnoreCase ("Agent mode is off")
                   && d.errorMessage.containsIgnoreCase ("Settings") && sends == 0,
                   "G17 with agent mode OFF a `do` is refused, names the switch, and NOTHING is sent",
                   d.errorCode + ": " + d.errorMessage.substring (0, 70));
        }
        // ...and the start context SAYS SO, so a model does not plan a round it cannot execute.
        {
            auto ctx = xd.startContext();
            auto* o = ctx.getDynamicObject();
            check (o != nullptr && ! (bool) o->getProperty ("can_act"),
                   "G17 the start context reports can_act FALSE while the switch is off");
            check (o != nullptr && o->hasProperty ("channel"),
                   "G17 ...and A2's read-half context is INHERITED, not re-implemented");
        }
        modeOn = true; ejOnly = true;
        {
            auto ctx = xd.startContext();
            auto* o = ctx.getDynamicObject();
            check (o != nullptr && (bool) o->getProperty ("can_act")
                   && (bool) o->getProperty ("echojay_only"),
                   "G17 with the switch ON the context reports can_act and echojay_only - both read LIVE, so a "
                   "user who turns either one off mid-round is obeyed");
        }
        // ONE `do`, ONE COMMAND, in the chain channel's own vocabulary.
        {
            sends = 0;
            auto d = run (xd, "do", call ("do", R"({"op":"set_pre_gain","db":-3})"));
            auto* arr = sentOps.getArray();
            const auto* first = (arr != nullptr && arr->size() == 1) ? (*arr)[0].getDynamicObject() : nullptr;
            check (d.ok && sends == 1 && first != nullptr && first->getProperty ("op").toString() == "pre_gain"
                   && std::abs ((double) first->getProperty ("db") + 3.0) < 0.001,
                   "G17 set_pre_gain becomes ONE pre_gain op on the chain channel", juce::JSON::toString (sentOps, true));
            check (d.undoToken.isNotEmpty(), "G17 ...and a landed `do` carries an undo token, because one do is one undo step");
        }
        // set_io MAY CARRY BOTH, and it is STILL one command and one ack - the user asked for one thing.
        {
            sends = 0;
            auto d = run (xd, "do", call ("do", R"({"op":"set_io","slot":2,"inDb":3,"outDb":-3})"));
            auto* arr = sentOps.getArray();
            check (d.ok && sends == 1 && arr != nullptr && arr->size() == 2,
                   "G17 set_io with both inDb and outDb is TWO ops in ONE command and ONE ack",
                   juce::JSON::toString (sentOps, true));
            bool sawIn = false, sawOut = false, slotKept = false;
            if (arr != nullptr) for (auto& v : *arr) if (auto* o = v.getDynamicObject())
            {
                if (o->getProperty ("op").toString() == "slot_in")  sawIn = true;
                if (o->getProperty ("op").toString() == "slot_out") sawOut = true;
                if ((int) o->getProperty ("slot") == 2) slotKept = true;   // 1-based, passed through unchanged
            }
            check (sawIn && sawOut && slotKept,
                   "G17 ...as slot_in and slot_out, with the 1-based slot passed through unchanged");
        }
        // set_level IS THE RACK RECORD, not a Level slot - the 10 Oct ruling dropped that slot, and an
        // executor still writing it would be addressing a plugin that is not in the rack.
        {
            sends = 0;
            auto d = run (xd, "do", call ("do", R"({"op":"set_level","option":"commercial","target_lufs":-9})"));
            auto* arr = sentOps.getArray();
            const auto* first = (arr != nullptr && arr->size() == 1) ? (*arr)[0].getDynamicObject() : nullptr;
            const auto* lv = first != nullptr ? first->getProperty ("level").getDynamicObject() : nullptr;
            check (d.ok && first != nullptr && first->getProperty ("op").toString() == "set_level"
                   && lv != nullptr && lv->getProperty ("option").toString() == "commercial"
                   && std::abs ((double) lv->getProperty ("target_lufs") + 9.0) < 0.001,
                   "G17 set_level carries {option, target_lufs} for the RACK RECORD, not a Level slot",
                   juce::JSON::toString (sentOps, true));
        }
        // A MISSING VALUE IS REFUSED WITH THE SENTENCE THE MODEL IS SHOWN, unparaphrased (contract 4.3).
        {
            sends = 0;
            auto d = run (xd, "do", call ("do", R"({"op":"set_io","slot":1})"));
            check (! d.ok && d.errorCode == "bad_request" && d.errorMessage.containsIgnoreCase ("inDb")
                   && sends == 0,
                   "G17 a set_io with neither inDb nor outDb is refused, says which fields it wanted, and "
                   "sends nothing", d.errorMessage);
        }
        // AN UNKNOWN OP IS NAMED. A model told "that failed" tries the same op again.
        {
            auto d = run (xd, "do", call ("do", R"({"op":"set_sideways","slot":1})"));
            check (! d.ok && d.errorMessage.contains ("set_sideways"),
                   "G17 an op this build does not have is refused BY NAME", d.errorMessage);
        }
        // AND THE SWITCH GOVERNS UNDO TOO, or an agent could unwind a user's work with the switch off.
        {
            modeOn = false;
            ToolOutcome u; xd.undoStep ("tok", [&u] (ToolOutcome o) { u = o; });
            check (! u.ok && u.errorCode == "not_permitted",
                   "G17 with agent mode OFF an agent undo is refused too", u.errorMessage.substring (0, 60));
        }
    }
    // ---- G16 compactness ------------------------------------------------------------------------------------------------------------------------
    {
        FakeHost big;
        big.own.slots.clear();
        for (int i = 0; i < 16; ++i)
        {
            SlotRead s; s.n = i + 1; s.name = "Some Long Third-Party Plugin Name " + juce::String (i) + " (s)";
            s.settings = juce::String::repeatedString ("threshold -18 dB ratio 4:1 attack 10 ms release 300 ms knee soft ", 5);
            s.pictureValid = true; s.grKnown = true; s.grDb = 1.5f; s.inLufs = -16; s.outLufs = -17; s.inTpDb = -4; s.outTpDb = -5;
            big.own.slots.push_back (s);
        }
        ExecutorRead exb (big.sources());
        auto rack = runCounted (exb, "look", call ("look", R"({"what":"rack"})"));
        auto lv = runCounted (exb, "look", call ("look", R"({"what":"levels"})"));
        check (rack.ok && bytesOf (rack.result) <= ExecutorRead::kMaxResultBytes && rack.result.getProperty ("slots", {}).getArray()->size() == 16,
               "G16a a 16-slot rack with 300-char settings fits under 8 KB with every slot kept", juce::String (bytesOf (rack.result)));
        check (lv.ok && bytesOf (lv.result) <= ExecutorRead::kMaxResultBytes, "G16b ...and so do its levels", juce::String (bytesOf (lv.result)));
        check (maxBytesSeen <= ExecutorRead::kMaxResultBytes, "G16c every successful result in this guard was under 8 KB (the largest: " + juce::String (maxBytesSeen) + " bytes)");
    }

    // ---- LM (18 Oct 2026, item 13): A LINK'S GAIN REDUCTION CAN BE MEASURED --------------------
    //
    // *"On the Kick's Link, check gr fails with 'Gain reduction cannot be read from a Link yet', so
    // the certified-compressor measurement can't run on any Link. This was the original item 13."*
    //
    // The Link publishes its slots' own measurements now. What is still refusable is narrower, and
    // each refusal has to be TRUE of what it refuses: an older Link that publishes nothing, and a
    // reading too old to quote. Both directions here, because a measurement that cannot fail is the
    // thing his 15 Oct ruling is really about.
    std::printf ("\n== LM: item 13, a Link's certified compressor measures from its own sidecar ==\n");
    {
        FakeHost h;
        // the Kick's Link, publishing a certified compressor's window right now
        h.link.valid = true; h.link.meterCapable = true;
        h.link.measuredAtMs = juce::Time::currentTimeMillis();
        h.link.slots.clear();
        SlotRead cs;
        cs.n = 1; cs.name = "Tube-Tech CL 1B"; cs.builtin = false;
        cs.pictureValid = true; cs.grKnown = true; cs.grCertified = true;
        cs.grDb = 2.1f; cs.grPeakDb = 7.4f; cs.grTypicalDb = 3.2f; cs.grLoudestDb = 5.6f; cs.grHeldDb = 5.1f;
        cs.grWindowFrames = 480.0;
        cs.inLufs = -14.2f; cs.outLufs = -16.8f; cs.inTpDb = -3.1f; cs.outTpDb = -5.4f;
        h.link.slots.push_back (cs);
        h.playing = true; h.transportKnown = true;

        ExecutorRead ex (h.sources()); ex.setTarget (h.link.uid);     // the TARGET is the Link
        const auto o = run (ex, "check", call ("check", R"({"what":"gr","slot":1,"channel":"lnk_1"})"));
        check (o.ok,
               "LM1 check(gr) on a Link ANSWERS (RED as it stood: \"Gain reduction cannot be read from "
               "a Link yet - this will not work on a retry\", which closed the certified-compressor "
               "measurement on every Link)",
               o.ok ? juce::String ("answered") : (o.errorCode + ": " + o.errorMessage.substring (0, 80)));
        // THE FIELD NAMES THE OWN-RACK BRANCH ALREADY USES - read off the product rather than guessed,
        // which is what my first cut of these two legs did (they failed on my spelling, not the fix).
        const auto loudest = (double) o.result.getProperty ("grLoudestDb", juce::var (-1.0));
        check (std::abs (loudest - 5.6) < 0.05,
               "LM1b ...with the LOUDEST figure (the 95th percentile), which is the one his 15 Oct "
               "ruling says to report", juce::String (loudest, 2));
        check (o.result.getProperty ("measured", juce::var()).toString().containsIgnoreCase ("listen window")
               && std::abs ((double) o.result.getProperty ("grHeldDb", juce::var (-1.0)) - 5.1) < 0.05,
               "LM1c ...and it says the figure came from the whole listen window, with the held figure "
               "the meter shows beside it - so nobody can quote an instant as the measurement",
               o.result.getProperty ("measured", juce::var()).toString());
    }
    {
        // AN OLDER LINK publishes nothing, and that is its own sentence - not "a Link cannot".
        FakeHost h;
        h.link.valid = true; h.link.meterCapable = false; h.link.measuredAtMs = 0;
        h.link.slots.clear();
        SlotRead cs; cs.n = 1; cs.name = "Tube-Tech CL 1B"; cs.grCertified = true;
        h.link.slots.push_back (cs);
        ExecutorRead ex (h.sources()); ex.setTarget (h.link.uid);
        const auto o = run (ex, "check", call ("check", R"({"what":"gr","slot":1,"channel":"lnk_1"})"));
        check (! o.ok && o.errorCode == "link_has_no_meters",
               "LM2 a Link whose build does not publish meters says THAT, and names the fix",
               o.errorCode + ": " + o.userMessage.substring (0, 70));
        check (o.userMessage.containsIgnoreCase ("older build")
               && ! o.userMessage.containsIgnoreCase ("cannot be read from a Link"),
               "LM2b ...in a sentence that is true of this Link rather than of Links", o.userMessage);
    }
    {
        // A STALE READING is refused with no figure in it, and IS worth retrying after playback.
        FakeHost h;
        h.link.valid = true; h.link.meterCapable = true;
        h.link.measuredAtMs = juce::Time::currentTimeMillis() - 60000;   // a minute old
        h.link.slots.clear();
        SlotRead cs;
        cs.n = 1; cs.name = "Tube-Tech CL 1B"; cs.pictureValid = true; cs.grKnown = true;
        cs.grCertified = true; cs.grLoudestDb = 5.6f; cs.grWindowFrames = 480.0;
        h.link.slots.push_back (cs);
        ExecutorRead ex (h.sources()); ex.setTarget (h.link.uid);
        const auto o = run (ex, "check", call ("check", R"({"what":"gr","slot":1,"channel":"lnk_1"})"));
        check (! o.ok && o.errorCode == "link_meters_stale",
               "LM3 a reading too old to be a measurement of what is playing is REFUSED (his ruling: a "
               "figure nobody can check is worse than no figure)", o.errorCode);
        check (! o.errorMessage.contains ("5.6") && ! o.userMessage.contains ("5.6"),
               "LM3b ...with NO figure anywhere in the refusal - a stale number quoted once is a number "
               "that gets repeated", o.userMessage);
        check ((bool) o.result.getProperty ("retryable", juce::var (false)),
               "LM3c ...and marked retryable, because playing the section is exactly what fixes it");
    }
    {
        // THE CAPABILITY B IS WAITING FOR, and the `cannot` entry no longer lying about Links.
        FakeHost h;
        h.link.valid = true; h.link.meterCapable = true;
        h.link.measuredAtMs = juce::Time::currentTimeMillis();
        ExecutorRead ex (h.sources()); ex.setTarget (h.link.uid);
        const auto ctx = ex.startContext();
        juce::StringArray caps;
        if (auto* a = ctx.getProperty ("capabilities", juce::var()).getArray())
            for (const auto& c : *a) caps.add (c.toString());
        check (caps.contains ("link_meters"),
               "LM4 the start context announces link_meters, so B can PLAN a measurement on a Link",
               caps.joinIntoString (","));
        check ((bool) ctx.getProperty ("grReadableOnLink", juce::var (false)),
               "LM4b ...and the flat flag agrees with it");
        const auto* cannot = ctx.getProperty ("cannot", juce::var()).getArray();
        check (cannot == nullptr || cannot->isEmpty(),
               "LM4c ...and nothing is listed as impossible that is now possible (RED as it stood: the "
               "gr-on-a-Link entry was unconditional, which is how the agent learned a limit that has gone)",
               juce::String (cannot == nullptr ? 0 : cannot->size()) + " entry/entries");
    }
    {
        // ...and an older Link still gets the warning BEFORE the call, which is what item 8 is for.
        FakeHost h;
        h.link.valid = true; h.link.meterCapable = false;
        ExecutorRead ex (h.sources()); ex.setTarget (h.link.uid);
        const auto ctx = ex.startContext();
        const auto* cannot = ctx.getProperty ("cannot", juce::var()).getArray();
        check (cannot != nullptr && cannot->size() == 1
               && (*cannot)[0].getProperty ("what", juce::var()).toString() == "gr",
               "LM5 an older Link is still declared BEFORE the agent plans a measurement on it");
        check (! (bool) ctx.getProperty ("grReadableOnLink", juce::var (true)),
               "LM5b ...and the flat flag says so too");
    }

    std::printf ("\n==== agent_executor_read_guard: %s (%d ok, %d failed) ====\n", failures == 0 ? "GREEN" : "RED", passes, failures);
    return failures == 0 ? 0 : 1;
}
