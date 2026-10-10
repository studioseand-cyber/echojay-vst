#include "EJAgentExecutorRead.h"
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "LinkShm.h"
#include "MeterEngine.h"
#include "LoudnessLoop.h"   // kCountFloorLufs: the window counts what the loop counts; the loop's option / target
#include "EJLevelRecord.h"  // the stored level record (21t-i), the rack-level levelling record's home
#include <cmath>

namespace echojay::agent
{

namespace
{
    const char* kBandNames[6] = { "sub", "low", "lowMid", "mid", "highMid", "air" };   // EchoJayAPI's mbNames, same order

    double r1 (float v) { return juce::String (v, 1).getDoubleValue(); }
    bool finiteLevel (float v) { return std::isfinite (v) && v > -150.0f; }
    void setLevel (juce::DynamicObject* o, const char* key, float v) { if (finiteLevel (v)) o->setProperty (key, r1 (v)); }

    juce::String capped (const juce::String& s, int maxChars)
    {
        const auto t = s.trim();
        return t.length() <= maxChars ? t : t.substring (0, maxChars - 1) + juce::String::fromUTF8 ("\xe2\x80\xa6");
    }

    // check(level)'s levelling fields (contract 2.3: targetLufs?, option, converged - plus what drives the gain and what landed)
    void addLevelling (juce::DynamicObject* o, const LevellingRead& lv)
    {
        if (! lv.present) return;
        if (lv.option.isNotEmpty()) o->setProperty ("option", lv.option);
        if (std::isfinite (lv.targetLufs)) o->setProperty ("targetLufs", r1 (lv.targetLufs));
        if (lv.drives.isNotEmpty()) o->setProperty ("drives", lv.drives);
        if (std::isfinite (lv.landedGainDb)) o->setProperty ("landedGainDb", r1 (lv.landedGainDb));
        o->setProperty ("converged", lv.converged);
    }

    // A coarse role from a built-in's name only; third-party roles are the server's to know from its maps.
    juce::String roleOfBuiltin (const juce::String& name)
    {
        const auto n = name.toLowerCase();
        if (! n.startsWith ("echojay ")) return {};
        if (n.contains ("limiter"))    return "limiter";
        if (n.contains ("level"))      return "level";
        if (n.contains ("compressor")) return "compressor";
        if (n.contains ("eq"))         return "eq";
        if (n.contains ("gain"))       return "gain";
        if (n.contains ("de-esser") || n.contains ("deesser")) return "de_esser";
        if (n.contains ("gate"))       return "gate";
        if (n.contains ("expander"))   return "expander";
        if (n.contains ("saturat") || n.contains ("exciter")) return "saturation";
        if (n.contains ("delay"))      return "delay";
        if (n.contains ("reverb"))     return "reverb";
        return "device";
    }
}

// =============================================================================
ExecutorRead::ExecutorRead (Sources s) : src_ (std::move (s)) {}

juce::String ExecutorRead::resolveChannelArg (const ToolCall& call) const
{
    const auto ch = argString (call, "channel").trim();
    if (ch.isEmpty() || ch == "self") return target_;
    return ch;
}

RackRead ExecutorRead::targetRack() const
{
    if (target_.isNotEmpty()) return src_.linkRack ? src_.linkRack (target_) : RackRead();
    return src_.ownRack ? src_.ownRack() : RackRead();
}

AnalysisRead ExecutorRead::targetAnalysis() const
{
    if (target_.isNotEmpty()) return src_.linkAnalysis ? src_.linkAnalysis (target_) : AnalysisRead();
    return src_.ownAnalysis ? src_.ownAnalysis() : AnalysisRead();
}

LevellingRead ExecutorRead::levellingFor (const juce::String& uid) const
{
    if (uid.isNotEmpty()) return src_.linkLevelling ? src_.linkLevelling (uid) : LevellingRead();
    return src_.ownLevelling ? src_.ownLevelling() : LevellingRead();
}
LevellingRead ExecutorRead::targetLevelling() const { return levellingFor (target_); }

juce::var ExecutorRead::levellingVar (const LevellingRead& lv)
{
    if (! lv.present) return juce::var();
    auto* o = new juce::DynamicObject();
    if (lv.option.isNotEmpty()) o->setProperty ("option", lv.option);
    if (std::isfinite (lv.targetLufs)) o->setProperty ("targetLufs", r1 (lv.targetLufs));
    if (lv.drives.isNotEmpty()) o->setProperty ("drives", lv.drives);
    if (std::isfinite (lv.landedGainDb)) o->setProperty ("landedGainDb", r1 (lv.landedGainDb));
    if (std::isfinite (lv.inLufs))  o->setProperty ("inLufs", r1 (lv.inLufs));
    if (std::isfinite (lv.outLufs)) o->setProperty ("outLufs", r1 (lv.outLufs));
    if (std::isfinite (lv.inLufs) && std::isfinite (lv.outLufs)) o->setProperty ("deltaDb", r1 (lv.outLufs - lv.inLufs));
    o->setProperty ("converged", lv.converged);
    if (lv.state.isNotEmpty()) o->setProperty ("state", lv.state);
    return juce::var (o);
}

ToolOutcome ExecutorRead::notPlaying (const juce::String& what)
{
    auto o = ToolOutcome::failure ("not_playing", "No audio has been heard for " + what + " yet.");
    auto* e = new juce::DynamicObject();
    e->setProperty ("hint", "ask the user to play the loudest section, then wait_for_playback");
    o.result = juce::var (e);
    return o;
}

// =============================================================================
//  the shapes
// =============================================================================
int ExecutorRead::jsonBytes (const juce::var& v) { return juce::JSON::toString (v, true).getNumBytesAsUTF8(); }

juce::var ExecutorRead::fitUnder8K (juce::var v)
{
    if (jsonBytes (v) <= kMaxResultBytes) return v;
    auto* o = v.getDynamicObject();
    if (o == nullptr) return v;
    // 1. the largest array shrinks first (an inventory, a slot list), halving until it fits
    for (int pass = 0; pass < 12 && jsonBytes (v) > kMaxResultBytes; ++pass)
    {
        juce::Identifier biggest; int biggestN = 0;
        for (const auto& p : o->getProperties())
            if (auto* a = p.value.getArray()) if (a->size() > biggestN) { biggestN = a->size(); biggest = p.name; }
        if (biggestN <= 1) break;
        auto* a = o->getProperty (biggest).getArray();
        a->removeRange (a->size() / 2, a->size() - a->size() / 2);
        o->setProperty ("truncated", true);
    }
    // 2. then every string field is capped
    if (jsonBytes (v) > kMaxResultBytes)
    {
        for (auto& p : o->getProperties())
            if (p.value.isString() && p.value.toString().length() > kMaxSettingsChars)
                o->setProperty (p.name, capped (p.value.toString(), kMaxSettingsChars));
        o->setProperty ("truncated", true);
    }
    return v;
}

juce::var ExecutorRead::rackVar (const RackRead& r, const LevellingRead& lv)
{
    auto* o = new juce::DynamicObject();
    auto* ch = new juce::DynamicObject();
    ch->setProperty ("uid", r.uid.isNotEmpty() ? r.uid : juce::String ("self"));
    ch->setProperty ("name", r.name);
    ch->setProperty ("kind", r.kind);
    o->setProperty ("channel", juce::var (ch));
    o->setProperty ("remote", r.remote);
    if (r.revision >= 0) o->setProperty ("revision", r.revision);
    juce::Array<juce::var> slots;
    for (const auto& s : r.slots)
    {
        auto* so = new juce::DynamicObject();
        so->setProperty ("n", s.n);
        so->setProperty ("name", s.name);
        const auto role = roleOfBuiltin (s.name);
        if (role.isNotEmpty()) so->setProperty ("role", role);
        so->setProperty ("bypassed", s.bypassed);
        so->setProperty ("wet", (int) std::lround (s.wet * 100.0f));
        so->setProperty ("keepLevel", s.keepLevel);
        so->setProperty ("builtin", s.builtin);
        // settings: the dial summary (what landed, one composer) when there is one, else the prose the slot carries
        const auto settings = s.dialSummary.isNotEmpty() ? s.dialSummary : s.settings;
        if (settings.isNotEmpty()) so->setProperty ("settings", capped (settings, kMaxSettingsChars));
        if (s.preTrimDb != 0.0f) so->setProperty ("inDb", r1 (s.preTrimDb));
        if (s.outGainDb != 0.0f) so->setProperty ("outDb", r1 (s.outGainDb));
        if (s.pictureValid && s.grKnown) so->setProperty ("grDb", r1 (s.grDb));
        slots.add (juce::var (so));
    }
    o->setProperty ("slots", juce::var (slots));
    o->setProperty ("masterWet", (int) std::lround (r.masterWet * 100.0f));
    o->setProperty ("preGainDb", r1 (r.preGainDb));
    // the levelling record rides at RACK level (10 Oct 2026): no slot carries it, and no slot is looked for
    const auto lvv = levellingVar (lv);
    if (! lvv.isVoid()) o->setProperty ("levelling", lvv);
    return fitUnder8K (juce::var (o));
}

juce::var ExecutorRead::tracksVar (const std::vector<TrackRead>& tracks)
{
    juce::Array<juce::var> arr;
    for (const auto& t : tracks)
    {
        auto* to = new juce::DynamicObject();
        to->setProperty ("uid", t.uid);
        to->setProperty ("name", t.name);
        to->setProperty ("connected", t.connected);
        to->setProperty ("audio", t.audioFlowing);
        if (! t.fresh) to->setProperty ("gone", true);
        if (t.channels > 0) to->setProperty ("channels", t.channels);
        if (t.placement == 1) to->setProperty ("placement", "bus"); else if (t.placement == 2) to->setProperty ("placement", "insert");
        if (t.gainDb != 0.0f) to->setProperty ("gainDb", r1 (t.gainDb));
        arr.add (juce::var (to));
    }
    return juce::var (arr);
}

juce::var ExecutorRead::channelVar (const juce::String& name, const juce::String& kind, const juce::String& uid,
                                    const std::vector<TrackRead>& tracks, const juce::String& project)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("uid", uid.isNotEmpty() ? uid : juce::String ("self"));
    o->setProperty ("name", name);
    o->setProperty ("kind", kind);
    o->setProperty ("links", tracksVar (tracks));
    if (project.isNotEmpty()) o->setProperty ("project", project);
    return fitUnder8K (juce::var (o));
}

juce::var ExecutorRead::analysisVar (const AnalysisRead& a)
{
    auto* o = new juce::DynamicObject();
    setLevel (o, "integratedLufs", a.integratedLufs);
    setLevel (o, "loudest3sLufs", a.shortTermMaxLufs);      // the loudest 3 s window = the short-term max
    setLevel (o, "shortTermMaxLufs", a.shortTermMaxLufs);
    setLevel (o, "peakDbtp", a.truePeakMaxDb);
    if (a.psrDb > -900.0f) o->setProperty ("psrDb", r1 (a.psrDb));
    if (a.lraLu > 0.0f) o->setProperty ("lraLu", r1 (a.lraLu));
    o->setProperty ("oversCount", a.oversCount);
    if (a.haveBands)
    {
        juce::Array<juce::var> bands;
        for (int i = 0; i < kMaxBands; ++i)
        {
            auto* b = new juce::DynamicObject();
            b->setProperty ("band", kBandNames[i]);
            b->setProperty ("vsAverageDb", r1 (a.bandRelDb[(size_t) i]));
            bands.add (juce::var (b));
        }
        o->setProperty ("spectrum", juce::var (bands));
    }
    o->setProperty ("heardSeconds", r1 (a.heardSeconds));
    o->setProperty ("playing", a.playing);
    if (a.ageMs > 0) o->setProperty ("readingAgeSeconds", (int) (a.ageMs / 1000));
    return fitUnder8K (juce::var (o));
}

juce::var ExecutorRead::levelsVar (const RackRead& r, const echojay::LevelTally::Snapshot& in, const echojay::LevelTally::Snapshot& out)
{
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> slots;
    for (const auto& s : r.slots)
    {
        auto* so = new juce::DynamicObject();
        so->setProperty ("n", s.n);
        so->setProperty ("name", s.name);
        if (s.pictureValid)
        {
            setLevel (so, "inLufs", s.inLufs);   setLevel (so, "outLufs", s.outLufs);
            setLevel (so, "inDbtp", s.inTpDb);   setLevel (so, "outDbtp", s.outTpDb);
            if (s.grKnown) so->setProperty ("grDb", r1 (s.grDb));
        }
        else so->setProperty ("reading", "none");
        slots.add (juce::var (so));
    }
    o->setProperty ("slots", juce::var (slots));
    auto* c = new juce::DynamicObject();
    if (in.known)  setLevel (c, "inLufs", in.levelDb);
    if (out.known) setLevel (c, "outLufs", out.levelDb);
    if (in.known && out.known) c->setProperty ("deltaDb", r1 (out.levelDb - in.levelDb));
    setLevel (c, "outDbtp", out.truePeakDb);
    c->setProperty ("heardSeconds", r1 (out.heardSeconds));
    o->setProperty ("chain", juce::var (c));
    return fitUnder8K (juce::var (o));
}

juce::var ExecutorRead::inventoryVar (const juce::StringArray& names)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("count", names.size());
    juce::Array<juce::var> arr;
    for (const auto& n : names) arr.add (n);
    o->setProperty ("names", juce::var (arr));
    return fitUnder8K (juce::var (o));
}

// =============================================================================
//  startContext
// =============================================================================
juce::var ExecutorRead::startContext()
{
    auto* o = new juce::DynamicObject();
    const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();
    juce::String name = src_.ownName ? src_.ownName() : juce::String();
    juce::String kind = src_.ownKind ? src_.ownKind() : juce::String ("channel");
    if (target_.isNotEmpty())
    {
        const auto r = targetRack();
        if (r.valid) { name = r.name; kind = r.kind; }
        else for (const auto& t : tracks) if (t.uid == target_) { name = t.name; kind = "channel"; }
    }
    auto* ch = new juce::DynamicObject();
    ch->setProperty ("uid", target_.isNotEmpty() ? target_ : juce::String ("self"));
    ch->setProperty ("name", name);
    ch->setProperty ("kind", kind);
    ch->setProperty ("links", tracksVar (tracks));
    o->setProperty ("channel", juce::var (ch));
    const auto lvv = levellingVar (targetLevelling());
    if (! lvv.isVoid()) o->setProperty ("levelling", lvv);
    juce::Array<juce::var> caps; caps.add ("look"); caps.add ("check"); caps.add ("wait_for_playback"); caps.add ("checkpoint");
    o->setProperty ("capabilities", juce::var (caps));
    o->setProperty ("agentMode", true);
    if (src_.projectName && src_.projectName().isNotEmpty()) o->setProperty ("project", src_.projectName());
    return fitUnder8K (juce::var (o));
}

// =============================================================================
//  look
// =============================================================================
void ExecutorRead::look (const ToolCall& call, Done done)
{
    const auto what = argString (call, "what").trim().toLowerCase();
    const auto uid = resolveChannelArg (call);

    if (what == "rack" || what == "get_rack")
    {
        RackRead r = uid.isNotEmpty() ? (src_.linkRack ? src_.linkRack (uid) : RackRead()) : (src_.ownRack ? src_.ownRack() : RackRead());
        if (! r.valid) { done (ToolOutcome::failure ("unknown_channel", r.why.isNotEmpty() ? r.why : "no rack can be read for \"" + uid + "\"")); return; }
        done (ToolOutcome::success (rackVar (r, levellingFor (uid))));
        return;
    }
    if (what == "channel" || what == "list_tracks")
    {
        const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();
        if (what == "list_tracks")
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("count", (int) tracks.size());
            o->setProperty ("tracks", tracksVar (tracks));
            done (ToolOutcome::success (fitUnder8K (juce::var (o))));
            return;
        }
        juce::String name = src_.ownName ? src_.ownName() : juce::String(), kind = src_.ownKind ? src_.ownKind() : juce::String ("channel");
        if (uid.isNotEmpty())
        {
            const auto r = src_.linkRack ? src_.linkRack (uid) : RackRead();
            if (r.valid) { name = r.name; kind = r.kind; }
            else
            {
                bool found = false;
                for (const auto& t : tracks) if (t.uid == uid) { name = t.name; kind = "channel"; found = true; }
                if (! found) { done (ToolOutcome::failure ("unknown_channel", "no Link with uid \"" + uid + "\" is in the registry")); return; }
            }
        }
        done (ToolOutcome::success (channelVar (name, kind, uid, tracks, src_.projectName ? src_.projectName() : juce::String())));
        return;
    }
    if (what == "analysis" || what == "analyse" || what == "analyze")
    {
        const auto a = uid.isNotEmpty() ? (src_.linkAnalysis ? src_.linkAnalysis (uid) : AnalysisRead()) : (src_.ownAnalysis ? src_.ownAnalysis() : AnalysisRead());
        if (! a.valid)
        {
            if (a.why.isNotEmpty() && ! a.why.containsIgnoreCase ("no audio")) { done (ToolOutcome::failure ("unknown_channel", a.why)); return; }
            done (notPlaying ("the analysis")); return;
        }
        done (ToolOutcome::success (analysisVar (a)));
        return;
    }
    if (what == "levels")
    {
        RackRead r = uid.isNotEmpty() ? (src_.linkRack ? src_.linkRack (uid) : RackRead()) : (src_.ownRack ? src_.ownRack() : RackRead());
        if (! r.valid) { done (ToolOutcome::failure ("unknown_channel", r.why)); return; }
        echojay::LevelTally::Snapshot in, out;
        if (uid.isEmpty()) { if (src_.chainInLoop) in = src_.chainInLoop(); if (src_.chainOut) out = src_.chainOut(); }
        done (ToolOutcome::success (levelsVar (r, in, out)));
        return;
    }
    if (what == "inventory")
    {
        const auto names = src_.inventory ? src_.inventory() : juce::StringArray();
        if (names.isEmpty()) { done (ToolOutcome::failure ("inventory_unavailable", "the installed plugin list is not bound in this build")); return; }
        done (ToolOutcome::success (inventoryVar (names)));
        return;
    }
    if (what == "maps" || what == "saved_chains")
    {
        done (ToolOutcome::failure ("server_tool", "look(" + what + ") is answered by the server, not the plugin"));
        return;
    }
    done (ToolOutcome::failure ("unknown_what", "look knows rack, channel, list_tracks, analysis, levels, inventory; not \"" + what + "\""));
}

// =============================================================================
//  check
// =============================================================================
void ExecutorRead::check (const ToolCall& call, Done done)
{
    const auto what = argString (call, "what").trim().toLowerCase();
    const int slotArg = (int) argNumber (call, "slot", 0);

    if (what == "level" || what == "measure")
    {
        if (target_.isNotEmpty())
        {
            // a remote rack: the Link publishes its chain figures in the frame; the loop's own window is local only
            const auto a = targetAnalysis();
            if (! a.valid || a.heardSeconds < 1.0f) { done (notPlaying ("a level check on the Link")); return; }
            auto* o = new juce::DynamicObject();
            setLevel (o, "outLufs", a.integratedLufs);
            setLevel (o, "loudest3sLufs", a.shortTermMaxLufs);
            setLevel (o, "outDbtp", a.truePeakMaxDb);
            o->setProperty ("heardSeconds", r1 (a.heardSeconds));
            o->setProperty ("source", "link_frame");
            addLevelling (o, targetLevelling());
            done (ToolOutcome::success (fitUnder8K (juce::var (o))));
            return;
        }
        const auto in = src_.chainInLoop ? src_.chainInLoop() : echojay::LevelTally::Snapshot();
        const auto out = src_.chainOut ? src_.chainOut() : echojay::LevelTally::Snapshot();
        if (! out.known) { done (notPlaying ("a level check")); return; }
        auto* o = new juce::DynamicObject();
        if (in.known) setLevel (o, "inLufs", in.levelDb);
        setLevel (o, "outLufs", out.levelDb);
        if (in.known) o->setProperty ("deltaDb", r1 (out.levelDb - in.levelDb));
        setLevel (o, "loudest3sLufs", out.maxShortTermDb);
        setLevel (o, "outDbtp", out.truePeakDb);
        o->setProperty ("heardSeconds", r1 (out.heardSeconds));
        o->setProperty ("source", "chain_tallies");
        addLevelling (o, targetLevelling());
        done (ToolOutcome::success (fitUnder8K (juce::var (o))));
        return;
    }
    if (what == "gr")
    {
        const auto r = targetRack();
        if (! r.valid) { done (ToolOutcome::failure ("unknown_channel", r.why)); return; }
        if (slotArg < 1 || slotArg > (int) r.slots.size()) { done (ToolOutcome::failure ("unknown_slot", "slot " + juce::String (slotArg) + " is not on the rack (1.." + juce::String ((int) r.slots.size()) + ")")); return; }
        const auto& s = r.slots[(size_t) slotArg - 1];
        if (! s.pictureValid || ! s.grKnown) { done (notPlaying ("gain reduction on slot " + juce::String (slotArg))); return; }
        auto* o = new juce::DynamicObject();
        o->setProperty ("slot", slotArg); o->setProperty ("name", s.name); o->setProperty ("grDb", r1 (s.grDb));
        setLevel (o, "inLufs", s.inLufs); setLevel (o, "outLufs", s.outLufs);
        done (ToolOutcome::success (juce::var (o)));
        return;
    }
    if (what == "true_peak")
    {
        const auto r = targetRack();
        if (! r.valid) { done (ToolOutcome::failure ("unknown_channel", r.why)); return; }
        if (r.slots.empty()) { done (ToolOutcome::failure ("empty_rack", "the rack has no slots")); return; }
        const int n = slotArg >= 1 && slotArg <= (int) r.slots.size() ? slotArg : (int) r.slots.size();
        const auto& s = r.slots[(size_t) n - 1];
        const auto a = targetAnalysis();
        auto* o = new juce::DynamicObject();
        o->setProperty ("slot", n); o->setProperty ("name", s.name);
        if (s.pictureValid) setLevel (o, "peakDbtp", s.outTpDb);
        else if (a.valid) setLevel (o, "peakDbtp", a.truePeakMaxDb);
        if (a.valid) o->setProperty ("oversCount", a.oversCount);
        if (! o->hasProperty ("peakDbtp")) { done (notPlaying ("a true-peak check")); return; }
        done (ToolOutcome::success (juce::var (o)));
        return;
    }
    if (what == "spectrum")
    {
        const auto a = targetAnalysis();
        if (! a.valid || ! a.haveBands) { done (notPlaying ("a spectrum reading")); return; }
        auto v = analysisVar (a);
        auto* o = new juce::DynamicObject();
        o->setProperty ("spectrum", v.getProperty ("spectrum", {}));
        o->setProperty ("heardSeconds", r1 (a.heardSeconds));
        done (ToolOutcome::success (juce::var (o)));
        return;
    }
    if (what == "balance")
    {
        // this channel against the mix bus: the channel is the target (a Link's frame), the mix is this instance when
        // it sits on the mix bus. Two readings from two hosts: no cross-track sample alignment is assumed (plan 5).
        const auto ownKind = src_.ownKind ? src_.ownKind() : juce::String();
        if (target_.isEmpty() || ! (ownKind == "mix_bus" || ownKind == "master"))
        { done (ToolOutcome::failure ("no_mix_reading", "balance needs a Link channel as the session's target and this instance on the mix bus")); return; }
        const auto ch = targetAnalysis();
        const auto mix = src_.ownAnalysis ? src_.ownAnalysis() : AnalysisRead();
        if (! ch.valid || ! mix.valid) { done (notPlaying ("a balance check")); return; }
        auto* o = new juce::DynamicObject();
        setLevel (o, "channelLufs", ch.integratedLufs);
        setLevel (o, "mixLufs", mix.integratedLufs);
        o->setProperty ("deltaDb", r1 (ch.integratedLufs - mix.integratedLufs));
        done (ToolOutcome::success (juce::var (o)));
        return;
    }
    if (what == "compare_to_checkpoint")
    {
        const auto token = argString (call, "checkpoint").trim();
        const Checkpoint* cp = nullptr;
        if (token.isNotEmpty()) cp = checkpoint (token);
        else if (! checkpoints_.empty()) cp = &checkpoints_.rbegin()->second;   // the newest when none is named
        if (cp == nullptr) { done (ToolOutcome::failure ("unknown_checkpoint", token.isEmpty() ? "no checkpoint has been captured" : "no checkpoint \"" + token + "\"")); return; }
        const auto now = targetRack();
        auto* o = new juce::DynamicObject();
        o->setProperty ("checkpoint", cp->token);
        o->setProperty ("label", cp->label);
        o->setProperty ("secondsAgo", (int) ((juce::Time::currentTimeMillis() - cp->timeMs) / 1000));
        juce::StringArray thenNames, nowNames;
        for (const auto& s : cp->rack.slots) thenNames.add (s.name + (s.bypassed ? " (byp)" : ""));
        for (const auto& s : now.slots)      nowNames.add (s.name + (s.bypassed ? " (byp)" : ""));
        bool rackChanged = thenNames != nowNames || std::abs (cp->rack.masterWet - now.masterWet) > 0.005f || std::abs (cp->rack.preGainDb - now.preGainDb) > 0.05f;
        if (! rackChanged)
            for (size_t i = 0; i < now.slots.size() && i < cp->rack.slots.size(); ++i)
                if (std::abs (now.slots[i].wet - cp->rack.slots[i].wet) > 0.005f || now.slots[i].dialSummary != cp->rack.slots[i].dialSummary
                    || std::abs (now.slots[i].preTrimDb - cp->rack.slots[i].preTrimDb) > 0.05f || std::abs (now.slots[i].outGainDb - cp->rack.slots[i].outGainDb) > 0.05f)
                { rackChanged = true; break; }
        o->setProperty ("rackChanged", rackChanged);
        o->setProperty ("slotsThen", thenNames.joinIntoString (" > "));
        o->setProperty ("slotsNow", nowNames.joinIntoString (" > "));
        // the levelling record then and now: the landed gain is the figure a check wants to see move (or not)
        {
            const auto lvNow = targetLevelling();
            if (std::isfinite (cp->levelling.landedGainDb)) o->setProperty ("landedGainThenDb", r1 (cp->levelling.landedGainDb));
            if (std::isfinite (lvNow.landedGainDb))         o->setProperty ("landedGainNowDb", r1 (lvNow.landedGainDb));
            if (std::isfinite (cp->levelling.landedGainDb) && std::isfinite (lvNow.landedGainDb))
                o->setProperty ("landedGainDeltaDb", r1 (lvNow.landedGainDb - cp->levelling.landedGainDb));
            if (lvNow.option.isNotEmpty() && lvNow.option != cp->levelling.option) o->setProperty ("optionChanged", true);
        }
        // levels: the checkpoint's chain-out snapshot against now's (own rack), or the frames (a Link)
        if (target_.isEmpty())
        {
            const auto out = src_.chainOut ? src_.chainOut() : echojay::LevelTally::Snapshot();
            if (cp->out.known) setLevel (o, "outLufsThen", cp->out.levelDb);
            if (out.known) setLevel (o, "outLufsNow", out.levelDb);
            if (cp->out.known && out.known) o->setProperty ("deltaDb", r1 (out.levelDb - cp->out.levelDb));
            setLevel (o, "peakThenDbtp", cp->out.truePeakDb); setLevel (o, "peakNowDbtp", out.truePeakDb);
        }
        else
        {
            const auto a = targetAnalysis();
            if (cp->analysis.valid) setLevel (o, "outLufsThen", cp->analysis.integratedLufs);
            if (a.valid) setLevel (o, "outLufsNow", a.integratedLufs);
            if (cp->analysis.valid && a.valid) o->setProperty ("deltaDb", r1 (a.integratedLufs - cp->analysis.integratedLufs));
            setLevel (o, "peakThenDbtp", cp->analysis.truePeakMaxDb); if (a.valid) setLevel (o, "peakNowDbtp", a.truePeakMaxDb);
        }
        done (ToolOutcome::success (fitUnder8K (juce::var (o))));
        return;
    }
    if (what == "playback")
    {
        done (ToolOutcome::failure ("client_tool", "wait_for_playback is run by the client, never by the executor"));
        return;
    }
    done (ToolOutcome::failure ("unknown_what", "check knows level, gr, true_peak, spectrum, balance, compare_to_checkpoint; not \"" + what + "\""));
}

// =============================================================================
//  the mutating half: not in this phase
// =============================================================================
void ExecutorRead::doOp (const ToolCall& call, Done done)
{
    done (ToolOutcome::failure ("not_in_phase", "do(" + argString (call, "op") + ") is not executed by this build; Session A wires it over the remote-control channel"));
}
void ExecutorRead::undoStep (const juce::String&, Done done)
{
    done (ToolOutcome::failure ("not_in_phase", "nothing was applied by this build, so there is nothing to undo"));
}
void ExecutorRead::undoToCheckpoint (const juce::String&, Done done)
{
    done (ToolOutcome::failure ("not_in_phase", "nothing was applied by this build, so there is nothing to undo"));
}

// =============================================================================
//  checkpoints
// =============================================================================
juce::String ExecutorRead::captureCheckpoint (const juce::String& label)
{
    Checkpoint cp;
    cp.token = "cp_" + juce::String (++checkpointSeq_);
    cp.label = label;
    cp.targetUid = target_;
    cp.timeMs = juce::Time::currentTimeMillis();
    cp.rack = targetRack();
    if (target_.isEmpty() && src_.chainOut) cp.out = src_.chainOut();
    cp.analysis = targetAnalysis();
    cp.levelling = targetLevelling();
    checkpoints_[cp.token] = cp;
    return cp.token;
}

const ExecutorRead::Checkpoint* ExecutorRead::checkpoint (const juce::String& token) const
{
    const auto it = checkpoints_.find (token);
    return it == checkpoints_.end() ? nullptr : &it->second;
}

// =============================================================================
//  the playback window
// =============================================================================
void ExecutorRead::beginPlaybackWindow()
{
    windowOpen_ = true;
    if (target_.isEmpty())
    {
        if (src_.beginWindow) src_.beginWindow();   // the loop's list: out tally, loop-owned in tally, both short-term maxes - never the song's integrated reading
        return;
    }
    // a Link: its tallies are its own; the window is measured as the frame's heardSeconds beyond this base
    const auto a = targetAnalysis();
    linkHeardBase_ = a.valid ? a.heardSeconds : 0.0f;
}

PlaybackReading ExecutorRead::readPlayback()
{
    PlaybackReading r;
    r.transportKnown = src_.transportKnown ? src_.transportKnown() : false;
    r.playing = src_.transportPlaying ? src_.transportPlaying() : false;
    if (target_.isEmpty())
    {
        const auto out = src_.chainOut ? src_.chainOut() : echojay::LevelTally::Snapshot();
        r.heardAboveSeconds = out.heardAboveSeconds;
        if (out.known && finiteLevel (out.levelDb)) r.integratedLufs = out.levelDb;
        if (std::isfinite (out.maxShortTermDb)) r.loudestShortTermLufs = out.maxShortTermDb;
        if (finiteLevel (out.truePeakDb)) r.truePeakDbtp = out.truePeakDb;
        return r;
    }
    const auto a = targetAnalysis();
    if (! a.valid) return r;
    r.heardAboveSeconds = juce::jmax (0.0f, a.heardSeconds - linkHeardBase_);
    if (a.playing) r.playing = true;             // the frame's own "audio flowing" counts as playing for a remote rack
    if (finiteLevel (a.integratedLufs)) r.integratedLufs = a.integratedLufs;
    if (finiteLevel (a.shortTermMaxLufs)) r.loudestShortTermLufs = a.shortTermMaxLufs;
    if (finiteLevel (a.truePeakMaxDb)) r.truePeakDbtp = a.truePeakMaxDb;
    return r;
}

// =============================================================================
//  binding to the real objects (A: the member names below are the ones read on 9 Oct; adjust at first compile)
// =============================================================================
namespace
{
    juce::String kindOf (ChannelType t)
    {
        if (t == ChannelType::FullMix)   return "mix_bus";
        if (t == ChannelType::MasterBus) return "master";
        if (EchoJayProcessor::isBusRole (t)) return "bus";
        return "channel";
    }

    RackRead rackFromHost (const ChainHost& host, const juce::String& name, const juce::String& kind)
    {
        RackRead r;
        r.valid = true; r.remote = false;
        r.name = name; r.kind = kind;
        r.revision = host.getChainRevision();
        r.masterWet = host.getMasterWet();
        r.preGainDb = host.getPreGainDb();
        const int n = host.getNumSlots();
        for (int i = 0; i < n; ++i)
        {
            const auto info = host.getSlotInfo (i);
            SlotRead s;
            s.n = i + 1;
            s.name = info.name; s.format = info.format; s.settings = info.settings;
            s.bypassed = info.bypassed; s.keepLevel = info.keepLevel;
            s.wet = info.wet; s.preTrimDb = info.preTrimDb; s.outGainDb = info.outGainDb;
            s.builtin = host.isBuiltinSlot (i);
            s.dialSummary = host.dialSummaryRow (i);
            s.pictureText = info.pictureText;
            const auto pic = host.slotPicture (i);
            s.pictureValid = pic.valid; s.grKnown = pic.grKnown; s.grDb = pic.grDb;
            s.inLufs = pic.inLufs; s.outLufs = pic.outLufs; s.inTpDb = pic.inTpDb; s.outTpDb = pic.outTpDb;
            r.slots.push_back (s);
        }
        return r;
    }

    RackRead rackFromSidecar (const LinkShm::RackSidecar& sc, const juce::String& displayName)
    {
        RackRead r;
        r.valid = sc.valid; r.remote = true;
        if (! sc.valid) { r.why = "the Link's rack sidecar could not be read"; return r; }
        r.uid = sc.uid; r.name = displayName.isNotEmpty() ? displayName : sc.name; r.kind = "channel";
        r.revision = sc.revision; r.masterWet = sc.masterWet; r.preGainDb = sc.preGainDb;
        int i = 0;
        for (const auto& ss : sc.slots)
        {
            SlotRead s;
            s.n = ++i;
            s.name = ss.name; s.format = ss.format; s.settings = ss.settings;
            s.bypassed = ss.bypassed; s.wet = ss.wet; s.preTrimDb = ss.preTrimDb; s.outGainDb = ss.outGainDb;
            s.builtin = ChainHost::isBuiltinName (ss.name);
            r.slots.push_back (s);
        }
        return r;
    }

    AnalysisRead analysisFromFrame (const LinkMeterFrame& f, juce::uint32 ageMs)
    {
        AnalysisRead a;
        a.valid = f.heardSeconds > 0.0f || f.integrated > -100.0f;
        if (! a.valid) { a.why = "no audio has been published by the Link yet"; return a; }
        a.integratedLufs = f.integrated; a.shortTermMaxLufs = f.shortTermMax; a.truePeakMaxDb = f.truePeakMax; a.lraLu = f.lra;
        a.haveBands = true;
        for (int i = 0; i < 6; ++i) a.bandRelDb[(size_t) i] = f.bandRel[i];
        a.heardSeconds = f.heardSeconds;
        a.playing = f.audioStale == 0;
        a.ageMs = ageMs;
        return a;
    }
}

Sources bindToProcessor (EchoJayProcessor& proc)
{
    Sources s;
    auto* p = &proc;
    // identity through the PUBLIC getters (getChannelType / getCustomChannelName / getProjectName); the members are private
    auto ownName = [p] { const auto t = p->getChannelType(); return t == ChannelType::Other && p->getCustomChannelName().isNotEmpty() ? p->getCustomChannelName() : channelTypeNames[(int) t]; };
    auto ownKind = [p] { return kindOf (p->getChannelType()); };
    s.ownName  = ownName;
    s.ownKind  = ownKind;
    s.projectName = [p] { return p->getProjectName(); };
    s.ownRack  = [p, ownName, ownKind] { return rackFromHost (p->getChainHost(), ownName(), ownKind()); };
    s.tracks   = [p]
    {
        std::vector<TrackRead> out;
        for (const auto& l : p->getLinkSlotInfos())
        {
            TrackRead t;
            t.uid = l.uid; t.name = p->resolveLinkDisplayName (l.uid).isNotEmpty() ? p->resolveLinkDisplayName (l.uid) : l.name;
            t.connected = l.connected; t.audioFlowing = l.audioFlowing; t.active = l.active; t.fresh = l.heartbeatFresh;
            t.channels = l.channels; t.placement = l.placement; t.gainDb = l.gainDb;
            out.push_back (t);
        }
        return out;
    };
    s.linkRack = [p] (const juce::String& uid)
    {
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || uid.isEmpty()) { RackRead r; r.why = "no shared directory: the Link channel is not available in this host"; return r; }
        bool known = false;
        for (const auto& l : p->getLinkSlotInfos()) if (l.uid == uid) known = true;
        if (! known) { RackRead r; r.why = "no Link with uid \"" + uid + "\" is in the registry"; return r; }
        return rackFromSidecar (LinkShm::readRackSidecar (dir, uid), p->resolveLinkDisplayName (uid));
    };
    s.ownAnalysis = [p]
    {
        AnalysisRead a;
        const auto md = p->getMeterEngine().getMeterData();
        const auto out = p->getChainHost().getChainOutLevels();
        a.heardSeconds = out.heardSeconds;
        a.transportKnown = p->isTransportKnown(); a.playing = p->isTransportPlaying();
        a.valid = out.heardSeconds > 0.0f || md.integrated > -100.0f;
        if (! a.valid) { a.why = "no audio has been heard yet"; return a; }
        a.integratedLufs = md.integrated; a.shortTermMaxLufs = md.shortTermMax;
        a.truePeakMaxDb = juce::jmax (md.truePeakMaxL, md.truePeakMaxR);
        a.psrDb = md.psr; a.lraLu = md.loudnessRange; a.oversCount = md.oversCount;
        const auto mw = p->getMeterEngine().reduceMacroWindow (p->spectrumUsesAverage());
        a.haveBands = mw.valid;
        if (mw.valid) for (size_t i = 0; i < 6; ++i) a.bandRelDb[i] = mw.rel[i];
        return a;
    };
    s.linkAnalysis = [p] (const juce::String& uid)
    {
        LinkMeterFrame f; juce::uint32 age = 0;
        int regIdx = -1;
        for (const auto& l : p->getLinkSlotInfos()) if (l.uid == uid) regIdx = l.regIdx;
        if (regIdx < 0) { AnalysisRead a; a.why = "no Link with uid \"" + uid + "\" is in the registry"; return a; }
        if (p->readLinkMeterFrame (regIdx, f) && f.audioStale == 0) return analysisFromFrame (f, 0);
        if (p->linkLastGoodFrame (uid, f, age)) return analysisFromFrame (f, age);   // the last-good latch: honest about its age
        AnalysisRead a; a.why = "no audio has been published by the Link yet"; return a;
    };
    // THE LEVELLING RECORD. Sean, 10 Oct 2026: the EchoJay Level slot is gone; levelling drives the rack OUT gain
    // (match) or the final limiter's IN gain (a target) and its record is stored at rack level. A is building that
    // record; until it lands this reads what exists today - the loop's option / target / state and the stored
    // level record's in / out figures - through ONE seam, so when A's record arrives only these two lambdas change
    // and nothing the executor sends changes shape. A: fill `drives`, `landedGainDb` and `converged` from the record.
    s.ownLevelling = [p]
    {
        LevellingRead l;
        auto& loop = p->loudnessLoop();
        const auto rec = p->levelRecordFor ({});
        const auto word = loop.loudnessOption();          // the brief's word today; the contract's option once the record carries it
        l.present = loop.everArmed() || word.isNotEmpty() || rec.valid;
        if (! l.present) return l;
        if (word == "keep" || word == "match") l.option = "match";
        else if (word == "dynamic") l.option = "dynamic";
        else if (word.isNotEmpty()) l.option = "pushed";   // commercial / pushed / explicit map to pushed (CONTRACT_LEVEL_PARAMS)
        if (loop.everArmed() && std::isfinite (loop.target()) && loop.target() < -0.5f) l.targetLufs = loop.target();
        l.drives = l.option == "match" ? "rack_out" : (std::isfinite (l.targetLufs) ? "limiter_in" : juce::String());
        if (loop.everArmed()) l.landedGainDb = loop.currentGainDb();   // A: the record's landed gain once it exists
        if (echojay::LevelRecord::has (rec.intLufs)) l.inLufs = rec.intLufs;   // the own record is the chain INPUT (the song)
        const auto out = p->getChainHost().getChainOutLevels();
        if (out.known) l.outLufs = out.levelDb;
        l.converged = loop.everArmed() && ! loop.isArmed() && ! loop.hasProposal();
        l.updatedMs = rec.updatedMs;
        return l;
    };
    s.linkLevelling = [p] (const juce::String& uid)
    {
        LevellingRead l;
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || uid.isEmpty()) return l;
        const auto sc = LinkShm::readRackSidecar (dir, uid);
        if (! sc.valid) return l;
        const auto rec = echojay::LevelRecord::fromVar (sc.levels);   // the Link's stored record (21t-i); A adds option/target/landed to the sidecar
        if (! rec.valid) return l;
        l.present = true;
        if (echojay::LevelRecord::has (rec.intLufs)) l.outLufs = rec.intLufs;   // a Link's record is taken after its chain
        if (auto* o = sc.levels.getDynamicObject())
        {   // A's rack-level fields, read when present and ignored when not (an older Link publishes none)
            if (o->hasProperty ("option"))   l.option = o->getProperty ("option").toString();
            if (o->hasProperty ("target"))   l.targetLufs = (float) (double) o->getProperty ("target");
            if (o->hasProperty ("drives"))   l.drives = o->getProperty ("drives").toString();
            if (o->hasProperty ("landedDb")) l.landedGainDb = (float) (double) o->getProperty ("landedDb");
            if (o->hasProperty ("inLufs"))   l.inLufs = (float) (double) o->getProperty ("inLufs");
            if (o->hasProperty ("converged")) l.converged = (bool) o->getProperty ("converged");
        }
        l.updatedMs = rec.updatedMs;
        return l;
    };
    s.chainInLoop = [p] { return p->getChainHost().getChainInLoopLevels(); };
    s.chainOut    = [p] { return p->getChainHost().getChainOutLevels(); };
    s.beginWindow = [p]
    {
        // LoudnessLoop::startWindow's list, verbatim: the loop-owned tallies only. chainInTally_ (the song's
        // integrated reading, getChainInLevels) and resetAllLevels() are NOT called - Sean's 13:22 rule, 08c F2-3.
        auto& h = p->getChainHost();
        h.setChainOutCountFloor (LoudnessLoop::kCountFloorLufs);
        h.resetChainOutLevels();
        h.resetChainInLoopLevels();
        h.resetChainOutShortTermMax();
        h.resetChainInShortTermMax();
    };
    s.transportKnown   = [p] { return p->isTransportKnown(); };
    s.transportPlaying = [p] { return p->isTransportPlaying(); };
    s.chainRevision    = [p] { return p->getChainHost().getChainRevision(); };
    // s.inventory: bound by the editor (hook E2b) - the installed list lives with the scanner
    return s;
}

} // namespace echojay::agent
