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
//   G9   check(level) / measure: the window's in/out/delta/peak; nothing heard -> not_playing
//   G10  check(gr, slot): the slot's GR; out of range -> unknown_slot; no reading -> not_playing
//   G11  check(true_peak): the last slot's peak + overs; a named slot
//   G12  check(spectrum): the bands; check(balance): Link target vs this mix bus, else no_mix_reading
//   G13  captureCheckpoint + compare_to_checkpoint: unchanged -> rackChanged:false; a bypass + a level move ->
//        rackChanged:true and deltaDb; an unknown token -> unknown_checkpoint; a Link target compares frames
//   G14  beginPlaybackWindow calls the window ONCE and never the song's reset; readPlayback reads the out tally;
//        a Link target counts heardSeconds beyond the base at window start
//   G15  doOp / undoStep / undoToCheckpoint -> not_in_phase
//   G16  EVERY result in this guard, plus a 16-slot rack with 300-char settings, serialises under 8 KB
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJAgentExecutorRead.h"
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
               && std::abs ((double) s1.getProperty ("grDb", {}) - 2.4) < 0.01,
               "G2c a third-party slot carries its prose settings, its IN trim and its GR, and no guessed role");
        check ((int) s2.getProperty ("wet", {}) == 15 && (int) s0.getProperty ("wet", {}) == 100 && ! s3.hasProperty ("grDb") && ! s2.hasProperty ("grDb"),
               "G2d wet is a percentage; a slot with no GR reading carries no grDb");
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
        check (o.ok && slots != nullptr && slots->size() == 4 && std::abs ((double) (*slots)[1].getProperty ("grDb", {}) - 2.4) < 0.01
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
        check (o.ok && (int) o.result.getProperty ("slot", {}) == 2 && o.result.getProperty ("name", {}).toString() == "API-2500 (s)" && std::abs ((double) o.result.getProperty ("grDb", {}) - 2.4) < 0.01,
               "G10a check(gr, slot 2): the slot's GR");
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
    // ---- G15 the mutating half --------------------------------------------------------------------------------------------------------------
    {
        auto d = run (ex, "do", call ("do", R"({"op":"set","slot":1})"));
        ToolOutcome u1, u2; ex.undoStep ("u1", [&] (ToolOutcome o) { u1 = o; }); ex.undoToCheckpoint ("cp_1", [&] (ToolOutcome o) { u2 = o; });
        check (! d.ok && d.errorCode == "not_in_phase" && d.errorMessage.contains ("set") && ! u1.ok && u1.errorCode == "not_in_phase" && ! u2.ok && u2.errorCode == "not_in_phase",
               "G15 doOp, undoStep and undoToCheckpoint answer not_in_phase (A's half)");
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

    std::printf ("\n==== agent_executor_read_guard: %s (%d ok, %d failed) ====\n", failures == 0 ? "GREEN" : "RED", passes, failures);
    return failures == 0 ? 0 : 1;
}
