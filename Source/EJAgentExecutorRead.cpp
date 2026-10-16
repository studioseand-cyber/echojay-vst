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

// ITEM 6(a), 12 Oct 2026. THIS RETURNED THE MODEL'S RAW STRING, WHICH IS WHY THE KICK COULD NOT BE REACHED.
// Sean asked for "a touch more top end on the kick" with the V2 on Mix Bus. The start context already sends every
// Link's real uid AND its track name (tracksVar), but the model answered with "88_7even Kick_2_01" - a NAME-shaped
// string - and this function handed it straight to the registry, which refused it: `no Link with uid "..." is in
// the registry`. Nothing anywhere resolved a name to a uid.
//
// So: uid first (exact, and that must stay first - a uid is unambiguous), then the track name, case-insensitively,
// then a containment match in EITHER direction so "kick" finds "88_7even Kick_2" and "88_7even Kick_2_01" finds
// "88_7even Kick_2". A containment match is only accepted when it is UNIQUE; two candidates mean the executor
// hands the string back unresolved and the caller's failure lists the tracks, because quietly picking one of two
// channels is how an agent edits the wrong thing.
// ---- ITEM 1 (Sean, 13 Oct 2026): THE LABEL IS NOT PART OF THE UID -------------------------------------------
//
// The agent sent  channel: "uid bb411b8240"  and the registry holds  bb411b8240, so resolution failed and the
// whole round failed with it: `no Link with uid "uid bb411b8240" is in the registry`. The uid was RIGHT. The
// word in front of it was the problem, and I had written a resolver that compared the argument to the registry
// with no normalisation at all - so the one spelling a model most naturally produces, having just read "uid
// bb411b8240" out of the start context, was the one spelling that could not work.
//
// Normalised before any comparison, and the normalisation is tried ALONGSIDE the raw string rather than instead
// of it, so a track genuinely named "uid something" still resolves by its name.
juce::String normaliseChannelArg (const juce::String& in)
{
    auto s = in.trim().unquoted().trim();
    for (const char* lab : { "uid", "id", "track", "channel", "link" })
    {
        const juce::String l (lab);
        if (! s.startsWithIgnoreCase (l) || s.length() <= l.length()) continue;
        // The label counts only when a SEPARATOR follows it, so "linkwood" is not stripped to "wood".
        const auto sep = s[l.length()];
        if (sep != ':' && sep != '=' && ! juce::CharacterFunctions::isWhitespace (sep)) continue;
        const auto rest = s.substring (l.length() + 1).trim().unquoted().trim();
        if (rest.isNotEmpty()) return rest;
    }
    return s;
}

juce::String ExecutorRead::resolveChannelArg (const ToolCall& call) const
{
    const auto raw = argString (call, "channel").trim();
    if (raw.isEmpty() || raw == "self") return target_;

    const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();

    // Every spelling that means the same channel, in one list, tried most-exact first. The RAW string comes
    // first at every stage so a stripped label can never beat a real match on the whole argument.
    juce::StringArray forms;
    forms.addIfNotAlreadyThere (raw);
    forms.addIfNotAlreadyThere (normaliseChannelArg (raw));

    for (const auto& f : forms)
        for (const auto& t : tracks) if (t.uid == f) return t.uid;                    // a uid, exactly
    for (const auto& f : forms)
        for (const auto& t : tracks) if (t.name == f) return t.uid;                    // a name, exactly, case kept

    // case-insensitive name, then a unique containment match, over every form
    for (const auto& f : forms)
    {
        const auto want = f.toLowerCase();
        for (const auto& t : tracks) if (t.name.toLowerCase() == want) return t.uid;
    }
    for (const auto& f : forms)
    {
        const auto want = f.toLowerCase();
        juce::String hit; int hits = 0;
        for (const auto& t : tracks)
        {
            const auto have = t.name.toLowerCase();
            if (have.isEmpty()) continue;
            if (have.contains (want) || want.contains (have)) { hit = t.uid; ++hits; }
        }
        if (hits == 1) return hit;
    }
    // Unresolved or ambiguous. The RAW argument goes back, not my normalised form, so the failure quotes the
    // model exactly what it sent - a sentence that silently reports a different string than the one that was
    // tried is how a loop spends six rounds on a typo it cannot see.
    return raw;
}

// ITEM 6(d): A READ FAILURE HAS TO BE READABLE ENOUGH TO STOP THE LOOP. The agent retried for six rounds against
// `no Link with uid "..." is in the registry`, because that sentence does not say what WOULD work. Every
// unknown_channel failure now carries the channels that exist, so the next turn can either name one or stop.
juce::String ExecutorRead::knownChannelsSentence() const
{
    const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();
    if (tracks.empty()) return " No Links are in the registry at all, so no other channel can be addressed - "
                               "work on this channel or stop.";
    juce::StringArray rows;
    for (const auto& t : tracks) rows.add ("\"" + t.name + "\" (uid " + t.uid + ")");
    // RULED (Sean, 14 Oct): AN UNKNOWN TRACK OFFERS TAP CHOICES, NOT "TYPE THE EXACT NAME".
    // This sentence used to end "Name one of those, by uid or by its exact name, or stop" - which pulls
    // against B's own rule ("never ask the user for a uid or an id; if two tracks could genuinely be meant,
    // ask with their names as the choices") and the model echoes the shape of whatever it was last told.
    // That is how Sean was asked for "the exact vocal track name or uid" on 13 Oct. The names are right
    // here; what was missing was telling the model what to DO with them.
    return " The channels that exist are: " + rows.joinIntoString ("; ")
         + ". Ask the user which one, with those NAMES as the tap choices - never a uid, which the user never "
           "sees and cannot read off their screen. Or stop: this list is complete and will not change by "
           "retrying.";
}

// ---- ITEM 2 (Sean, 13 Oct 2026) ---------------------------------------------------------------------------
// The name of the channel this call acts on WHEN IT IS NOT THE OPEN ONE, so the plan card can say "on
// 88_7even Kick_2_01" and the user's Apply is a decision about a named track rather than a blank cheque.
// Returns "" for the open channel, and "" for a channel that does not resolve - an unresolved target is not
// something to put in front of a user as if it were a destination.
juce::String ExecutorRead::describeOtherChannel (const ToolCall& call)
{
    const auto asked = resolveChannelArg (call);
    if (asked.isEmpty() || asked == target_) return {};
    const auto tracks = src_.tracks ? src_.tracks() : std::vector<TrackRead>();
    for (const auto& t : tracks) if (t.uid == asked) return t.name.isNotEmpty() ? t.name : t.uid;
    return {};
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

ToolOutcome ExecutorRead::notPlaying (const juce::String& what) const
{
    // ---- FIX 3, SECOND HALF (Sean's rule, 14 Oct): IF NOTHING IS PLAYING, ASK - DO NOT FAIL -------------
    //
    // "No audio has been heard for X yet" read as a dead end, so the model treated it as one: Sean's kick
    // turn spent nine rounds on it. The remedy is not softer words, it is a DIFFERENT OUTCOME when the
    // transport is simply stopped - there is nothing wrong, the user has not pressed play - and the one
    // thing to do about it is ask them.
    //
    // The two cases are told apart by the transport, which the plugin knows for certain:
    //   stopped          -> needs_playback: ask the user to press play (an ASK, not a failure)
    //   playing but mute -> not_playing: the old sentence, which is now true rather than misleading
    const bool known   = src_.transportKnown && src_.transportKnown();
    const bool playing = src_.transportPlaying && src_.transportPlaying();
    if (known && ! playing)
    {
        auto o = ToolOutcome::failureFor ("needs_playback",
                     "Nothing is playing, so " + what + " cannot be measured yet. ASK THE USER to press play "
                     "(and to let the loudest part through), then wait_for_playback and read it again. This is "
                     "not an error and nothing is wrong - do not report it as a failure and do not retry the "
                     "read until they have.",
                     "Press play and I'll measure that.");
        auto* e = new juce::DynamicObject();
        e->setProperty ("retryable", true);
        e->setProperty ("transport", "stopped");
        e->setProperty ("ask_user", "press play");
        e->setProperty ("hint", "ask the user to press play, then wait_for_playback");
        o.result = juce::var (e);
        return o;
    }
    auto o = ToolOutcome::failure ("not_playing", "No audio has been heard for " + what + " yet"
                                   + (known ? juce::String (" - the transport is running, so the channel may be "
                                                            "silent or muted") : juce::String()) + ".");
    auto* e = new juce::DynamicObject();
    e->setProperty ("hint", "ask the user to play the loudest section, then wait_for_playback");
    o.result = juce::var (e);
    return o;
}

// ---- ITEM 8 (Sean, 13 Oct 2026) ---------------------------------------------------------------------------
// THE EXACT WORDING, in one place, so B can teach the agent not to plan it and the two never drift apart.
// `retryable:false` and the hint are the part that matters as much as the sentence: Sean's loop spent eighteen
// rounds on "slot 2 is not on the rack (1..0)" because that error reads like a mistake worth correcting. This
// one reads like a fact, and says what to do instead.
bool ExecutorRead::measuringALink (const ToolCall& call) const
{
    const auto asked = resolveChannelArg (call);
    return asked.isNotEmpty();      // "" is this instance's own rack; anything else is a Link uid
}

ToolOutcome ExecutorRead::notFromALink (const juce::String& what)
{
    auto o = ToolOutcome::failureFor ("not_available_on_link",
                                   what + " cannot be read from a Link yet - only from the rack this EchoJay "
                                   "window has open. This will not work on a retry.",
                                   what + " cannot be read from a Link yet.");
    auto* e = new juce::DynamicObject();
    e->setProperty ("retryable", false);
    e->setProperty ("hint", "do not plan a measurement on another track's Link: either work from what look(rack) "
                            "reports, or ask the user to open that track's own EchoJay window and measure there");
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
        if (s.pictureValid && s.grKnown)
        {
            // grNowDb, NOT grDb (Sean's checks 2 and 3, 15 Oct). The old name is retired from BOTH
            // reads rather than only from check(gr): one quantity with two names across two reads is
            // how a reader keeps quoting the momentary figure by accident. A caller that still asks
            // for "grDb" now gets nothing, which is safer than silently getting the misleading number.
            so->setProperty ("grNowDb", r1 (s.grDb));                    // this instant only
            if (s.grWindowFrames > 0.0)                                  // ...and what the window saw
            {
                so->setProperty ("grLoudestDb", r1 (s.grLoudestDb));
                so->setProperty ("grTypicalDb", r1 (s.grTypicalDb));
                so->setProperty ("grSpikeMaxDb", r1 (s.grPeakDb));   // spike warnings only
            }
        }
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
            if (s.grKnown)
            {
                so->setProperty ("grNowDb", r1 (s.grDb));                // see the note above: one name
                if (s.grWindowFrames > 0.0)
                {
                    so->setProperty ("grLoudestDb", r1 (s.grLoudestDb));
                    so->setProperty ("grTypicalDb", r1 (s.grTypicalDb));
                    so->setProperty ("grSpikeMaxDb", r1 (s.grPeakDb));   // spike warnings only
                }
            }
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

// ---- ITEM 9 (Sean, 13 Oct 2026): THE INVENTORY WAS A WINDOW WITH NO WAY TO MOVE IT ------------------------
//
// Sean asked "what compressors do I have" and got Acme, Abbey Road... - the alphabetical first slice of 1,598
// plugins. The cause was the 8 KB result rule meeting a list that cannot fit in it: fitUnder8K halved `names`
// until the JSON fitted and set truncated:true, which is honest, but `count` then reported the TRIMMED size
// and there was no filter and no offset. So the agent could see that it had been cut off and had no way to ask
// for the rest, and no way to ask for compressors specifically.
//
// Three parameters and three figures, and `total` is now the count of what MATCHED, before trimming, which is
// the number an agent needs to know whether it has covered the list.
juce::var ExecutorRead::inventoryVar (const juce::StringArray& names)
{
    return inventoryPage (names, {}, 0, kInventoryPage);
}

juce::var ExecutorRead::inventoryPage (const juce::StringArray& all, const juce::String& match,
                                       int offset, int limit)
{
    juce::StringArray hit;
    if (match.isEmpty()) hit = all;
    else
    {
        // Every token must appear somewhere in the name, case-insensitively: "fab comp" finds
        // "FabFilter Pro-C 2" only if both words are in it. Substring rather than prefix because a model asks
        // for "compressor" and the name says "Pro-C 2 Compressor".
        juce::StringArray want; want.addTokens (match.toLowerCase(), " ", "");
        want.removeEmptyStrings();
        for (const auto& n : all)
        {
            const auto low = n.toLowerCase();
            bool allIn = true;
            for (const auto& w : want) if (! low.contains (w)) { allIn = false; break; }
            if (allIn) hit.add (n);
        }
    }

    const int total = hit.size();
    const int off   = juce::jlimit (0, juce::jmax (0, total), offset);
    const int lim    = juce::jlimit (1, kInventoryPageMax, limit <= 0 ? kInventoryPage : limit);

    juce::Array<juce::var> arr;
    int returned = 0;
    for (int i = off; i < total && returned < lim; ++i, ++returned) arr.add (hit[i]);

    auto* o = new juce::DynamicObject();
    o->setProperty ("total",  total);            // how many MATCHED, before any paging or trimming
    o->setProperty ("offset", off);
    o->setProperty ("limit",  lim);
    if (match.isNotEmpty()) o->setProperty ("match", match);
    o->setProperty ("names", juce::var (arr));

    // `count` is NOT the page length. Before paging existed this function set count = names.size() BEFORE
    // fitUnder8K halved the array, so every caller that has ever read `count` read the number of plugins
    // INSTALLED, with `truncated` saying the names were only a slice of it. look(list_tracks) uses `count` the
    // same way. So count stays the total: a server that reads it to answer "how many do I have" keeps getting
    // the honest answer, and `returned` is the new field for the length of this page.
    auto v = fitUnder8K (juce::var (o));

    // Derived AFTER the trim, from the array that actually survived it - fitUnder8K may halve `names` again on
    // a page of very long names, and a `returned`/`hasMore` computed before that would be a lie about the very
    // thing the agent uses to decide whether to ask for the next page.
    const int kept = (v.getProperty ("names", {}).getArray() != nullptr)
                   ? v.getProperty ("names", {}).getArray()->size() : 0;
    if (auto* vo = v.getDynamicObject())
    {
        vo->setProperty ("returned", kept);
        vo->setProperty ("count",    total);
        vo->setProperty ("hasMore",  off + kept < total);
        if (off + kept < total) vo->setProperty ("truncated", true);   // its old meaning: you are not seeing all of it
    }
    return v;
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
    // ---- SEAN'S #8 (16 Oct 2026): WHAT CANNOT BE DONE, SAID BEFORE IT IS TRIED ------------------
    //
    // *"check(gr) on a Link: the agent called it twice, failed twice ('cannot be read from a Link
    // yet'), then said a retry won't work while offering 'Try once more' and asking 'What now?'.
    // Expose 'GR not readable on a Link' as a capability so the agent never calls it there."*
    //
    // The refusal already carries retryable:false and a hint (item 8, 13 Oct), which is the right
    // answer to a call that has happened. This is the answer BEFORE it happens: the start context
    // states the limit, keyed to THIS turn's target, so a plan that would have measured a Link's
    // gain reduction is never made. `cannot` is a list so the next limit of this kind has a home,
    // and FOR_B §10 asks B to read it. Said whether or not the target is a Link, because the model
    // can address another channel mid-turn.
    {
        juce::Array<juce::var> cannot;
        auto* gr = new juce::DynamicObject();
        gr->setProperty ("what", "gr");
        gr->setProperty ("where", "link");
        gr->setProperty ("why", "gain reduction is not published by a Link yet - only the rack this "
                                "EchoJay window has open can be measured");
        gr->setProperty ("retryable", false);
        gr->setProperty ("instead", "work from what look(rack) reports, or ask the user to open that "
                                    "track's own EchoJay window");
        cannot.add (juce::var (gr));
        o->setProperty ("cannot", juce::var (cannot));
        // The same fact in the flat form, for a reader that does not walk `cannot`.
        o->setProperty ("grReadableOnLink", false);
        o->setProperty ("targetIsLink", target_.isNotEmpty());
    }
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
        if (! r.valid) { done (ToolOutcome::failureFor ("unknown_channel", (r.why.isNotEmpty() ? r.why : "no rack can be read for \"" + uid + "\"") + knownChannelsSentence(),
                                                        "I could not read that rack.")); return; }
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
                if (! found) { done (ToolOutcome::failureFor ("unknown_channel", "no Link with uid \"" + uid + "\" is in the registry." + knownChannelsSentence(),
                                                              "I could not find that channel.")); return; }
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
        if (! r.valid) { done (ToolOutcome::failureFor ("unknown_channel", r.why + knownChannelsSentence(),
                                                        "I could not read that rack.")); return; }
        echojay::LevelTally::Snapshot in, out;
        if (uid.isEmpty()) { if (src_.chainInLoop) in = src_.chainInLoop(); if (src_.chainOut) out = src_.chainOut(); }
        done (ToolOutcome::success (levelsVar (r, in, out)));
        return;
    }
    if (what == "inventory")
    {
        const auto names = src_.inventory ? src_.inventory() : juce::StringArray();
        if (names.isEmpty()) { done (ToolOutcome::failure ("inventory_unavailable",
            "the installed plugin list is empty: no plugin scan has completed on this machine yet, so there is "
            "nothing to choose from. This will not change by retrying - work with EchoJay's own devices or stop.")); return; }
        // ITEM 9: `match`, `offset`, `limit` - the exact names in docs/CONTRACT_LINK_COMMANDS.md for B.
        // `name` and `q` are accepted as spellings of `match` because a model that has not read the schema
        // reaches for those, and silently ignoring a filter it believed it sent is the failure this item is.
        juce::String match = argString (call, "match");
        if (match.isEmpty()) match = argString (call, "name");
        if (match.isEmpty()) match = argString (call, "q");
        // argNumber's fallback doubles as "absent": offset 0 and limit kInventoryPage are the defaults, so a
        // missing argument and an argument that says the default are the same request.
        const int offset = (int) argNumber (call, "offset", 0.0);
        const int limit  = (int) argNumber (call, "limit",  (double) kInventoryPage);
        auto page = inventoryPage (names, match.trim(), offset, limit);
        // A filter that matched nothing is a RESULT, not a failure - "you have no de-essers" is an answer the
        // agent can act on, and a failure here would send it round again on the same question.
        done (ToolOutcome::success (page));
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
        // ITEM 8: BEFORE the rack, before the slot. The old order asked "is slot 2 on the rack I have open?"
        // about a slot the plan meant on a different rack, and answered truthfully about the wrong thing.
        if (measuringALink (call)) { done (notFromALink ("Gain reduction")); return; }
        const auto r = targetRack();
        if (! r.valid) { done (ToolOutcome::failureFor ("unknown_channel", r.why + knownChannelsSentence(),
                                                        "I could not read that rack.")); return; }
        if (slotArg < 1 || slotArg > (int) r.slots.size()) { done (ToolOutcome::failure ("unknown_slot", "slot " + juce::String (slotArg) + " is not on the rack (1.." + juce::String ((int) r.slots.size()) + ")")); return; }
        const auto& s = r.slots[(size_t) slotArg - 1];
        // ---- SEAN'S RULING, 15 Oct 2026: NO ESTIMATES, EVER ------------------------------------
        // "check(gr) returns a figure only for those; for anything else it returns not_certified with
        // no number." An EchoJay device reports its own reduction; a third party must have passed the
        // EJ Maps sweep, which is what the server gates a comp profile on. Everything else gets a
        // refusal with no figure in it - not a guess, and not a zero that reads like a measurement.
        // ---- SEAN'S #19 (16 Oct 2026): NOT CERTIFIED IS ANSWERED FIRST, AND IS NOT "SILENCE" ----
        //
        // *"check(gr) on an uncertified third-party (Renaissance Axx, own rack, audio playing, Axx
        // showing 15.5 dB attenuation) returned 'No audio has been heard for gain reduction on slot 1
        // yet, the channel may be silent or muted'. False reason."*
        //
        // The gate below required grKnown - a READING - before it would say "not certified". An
        // uncertified third party has no reading by construction, so it never reached this branch and
        // fell through to the not-playing answer, which invented a reason that was not true and sent
        // the user looking for a muted channel. Certification is a property of the PLUGIN, not of
        // whether audio has been heard, so it is decided here, before any listening question.
        // A BUILT-IN IS CERTIFIED BY CONSTRUCTION (16 Oct 2026, caught by the gate). Dropping the
        // old `pictureValid && grKnown` condition made the refusal fire for an EchoJay device that
        // simply had not been heard yet - "not certified" about our own compressor, which is both
        // wrong and exactly the kind of confident wrong answer this round is about. The refusal is
        // for a THIRD PARTY whose map has not passed the sweep; a device falls through to the
        // playback answer, which is the true one when nothing has been measured.
        if (! s.builtin && ! s.grCertified)
        {
            done (ToolOutcome::failureFor ("not_certified",
                        "gain reduction is not readable for \"" + s.name + "\": it is not an EchoJay device "
                        "and its parameter map is not certified for gain reduction (it has not passed the "
                        "EJ Maps compressor sweep). There is no figure to give and an estimate is not "
                        "acceptable. Say that you cannot measure this one, and do not infer a figure from "
                        "the slot's input and output levels.",
                        "I cannot measure gain reduction on " + s.name + " - it is not a plugin EchoJay has "
                        "measured, so any figure would be a guess."));
            return;
        }
        // A CERTIFIED slot with no reading yet IS the not-playing case, and that answer is true of it.
        if (! s.pictureValid || ! s.grKnown) { done (notPlaying ("gain reduction on slot " + juce::String (slotArg))); return; }
        // ---- SEAN'S CHECKS 2 AND 3 (15 Oct 2026): THE WINDOW, NOT THE INSTANT -------------------
        //
        // This used to return `grDb` alone - the reduction at the single instant the tool call ran,
        // which is systematically just after "Got it, you can stop", in the release tail. Sean's meter
        // showed about 5 dB on the loud parts and the agent reported 0.1 dB, then 2.1 dB. A momentary
        // sample of a time-varying quantity is not a measurement of it.
        //
        // So the device accumulates over the listen window and this reports what it saw: the PEAK and
        // the TYPICAL (the median of the frames where audio was present), each NAMED, with the current
        // value kept and labelled as the momentary one so nothing can quote it as "the" figure.
        auto* o = new juce::DynamicObject();
        o->setProperty ("slot", slotArg); o->setProperty ("name", s.name);
        // The one sentence a person reads, written in both branches below and landed on the row.
        juce::String grSentence;
        const bool haveWindow = s.grWindowFrames > 0.0;
        if (haveWindow)
        {
            // ---- SEAN'S RULING, 15 Oct 2026: THE LOUDEST HITS, NOT THE ABSOLUTE MAX --------------
            // "the absolute-max peak (8 dB) is a single transient instant nobody can see on the meter;
            // I see 2-4, hitting 5. Report 'loudest hits' as the 95th percentile of active blocks
            // instead, keep the true max internal for spike warnings only."
            // ---- SEAN'S #17 (16 Oct 2026): THE INSTANT MAX IS NOT PUBLISHED AT ALL --------------
            //
            // *"'loudest hits' never arrives. The agent said 'The peak reading still isn't coming
            // through' and gave only the typical (about 3 dB), then quoted 'the instant hitting up to
            // 6 dB', which was meant to stay internal. Fix the loudest-hits figure (95th percentile)
            // and never expose the instant max."*
            //
            // Two faults, one cause: the payload carried FIVE figures and told the model in prose
            // which two to use. The instant (grNowDb) and the single deepest transient
            // (grSpikeMaxDb) were there to be quoted, and were quoted; meanwhile the field a prompt
            // written against the old contract looks for - "peak" - was the name I had taken away
            // from the thing it used to mean.
            //
            // So: the instant and the transient are GONE from the result - a figure that must not be
            // quoted has no business being sent - and `grPeakDb`/`peakDb` now ALIAS the loudest hits,
            // so a reader asking for "the peak" gets the 95th percentile rather than a transient.
            // There is also one ready-made SENTENCE, because the surest way to stop a figure being
            // misdescribed is to hand over the words with it.
            o->setProperty ("grLoudestDb", r1 (s.grLoudestDb));   // 95th percentile - THE figure
            o->setProperty ("grPeakDb",    r1 (s.grLoudestDb));   // the old name, now the right figure
            o->setProperty ("peakDb",      r1 (s.grLoudestDb));
            o->setProperty ("grHeldDb",    r1 (s.grHeldDb));      // what the tile and the meter show
            o->setProperty ("grTypicalDb", r1 (s.grTypicalDb));   // median - where the needle sits
            o->setProperty ("grWindowFrames", s.grWindowFrames);
            o->setProperty ("measured", "over the whole listen window, only while audio was playing");
            grSentence = "The loudest hits are about " + juce::String (r1 (s.grLoudestDb), 1)
                       + " dB of gain reduction and the typical amount is about "
                       + juce::String (r1 (s.grTypicalDb), 1) + " dB.";
            o->setProperty ("sentence", grSentence);
            o->setProperty ("note", "Quote the `sentence` field, or build from grLoudestDb (the loudest "
                                    "hits: the 95th percentile of the blocks with audio, which is what a "
                                    "person sees on the meter) and grTypicalDb (the typical amount). There "
                                    "is deliberately no instantaneous or single-transient figure in this "
                                    "result: those are not measurements of what the compressor is doing "
                                    "and must not be reported.");
        }
        else
        {
            // NOTHING WAS MEASURED, which is not 0 dB of reduction. Saying so is the whole point: a
            // window with no frames is the absence of a reading, and the default has to be the safe
            // answer rather than the convenient one.
            // SEAN'S #17: not even here. The instantaneous figure was the one thing this branch had
            // to offer and it is precisely the figure he has twice seen quoted as a measurement.
            o->setProperty ("measured", "nothing yet - no audio has been through this slot since the "
                                        "listen window began");
            o->setProperty ("listenFirst", true);
            grSentence = "I have not heard anything through this compressor yet, so I cannot say how "
                         "much it is doing.";
            o->setProperty ("sentence", grSentence);
            o->setProperty ("note", "There is no figure to give. Run wait_for_playback with audio playing "
                                    "and ask again: a gain reduction is measured over a listen window, so "
                                    "the listen comes FIRST and the check after it.");
        }
        setLevel (o, "inLufs", s.inLufs); setLevel (o, "outLufs", s.outLufs);
        // ---- THE ROW SAYS WHAT WAS READ (16 Oct 2026, pass 12) ---------------------------------
        // The rig drew this step as `[done] detail=""`: a gain-reduction check that worked and told
        // the person NOTHING, because the whole answer travelled in the result for the model to
        // phrase. The ready-made sentence is written for a person - that is why it exists - so it
        // is also the landed line. The model's instructions stay in `note`, off the row.
        done (ToolOutcome::success (juce::var (o), {}, grSentence));
        return;
    }
    if (what == "true_peak")
    {
        if (measuringALink (call)) { done (notFromALink ("A true-peak reading")); return; }
        const auto r = targetRack();
        if (! r.valid) { done (ToolOutcome::failureFor ("unknown_channel", r.why + knownChannelsSentence(),
                                                        "I could not read that rack.")); return; }
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
        if (measuringALink (call)) { done (notFromALink ("The analyser's spectrum")); return; }
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
    // SEAN'S CHECKS 2 AND 3 (15 Oct 2026): the gain-reduction window starts HERE, with the loudness
    // one. It is reset for either target: a listen on a Link still listens through this instance's own
    // rack, and leaving yesterday's peak in place would be the opposite of the fault being fixed.
    if (src_.beginGrWindow) src_.beginGrWindow();
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
            s.grPeakDb = pic.grPeakDb; s.grTypicalDb = pic.grTypicalDb; s.grLoudestDb = pic.grLoudestDb;
            s.grHeldDb = pic.grHeldDb; s.grCertified = pic.grCertified;
            s.grWindowFrames = (double) pic.grWindowFrames;
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
    // ITEM 6(c), 12 Oct 2026: THE INVENTORY WAS NEVER BOUND. `Sources::inventory` was declared, documented
    // ("left for the ..."), and never assigned here - so look(inventory) answered "the installed plugin list is
    // not bound in this build" on every build that has ever shipped, and the agent could not see what Sean owns.
    // maybeReloadEntriesCache() first, for the same reason the Link's v:2 path needed it: entries_ is filled from
    // the shared chain_entries.xml and is EMPTY until something asks for it, so an unprimed list would read as
    // "nothing installed" rather than "not read yet".
    s.inventory = [p]
    {
        auto& host = p->getChainHost();
        host.maybeReloadEntriesCache();
        juce::StringArray names;
        for (const auto& d : host.getFilteredPlugins ({})) names.addIfNotAlreadyThere (d.name);
        // ---- SEAN'S CHECK 5, PART 2 (15 Oct 2026): NEVER OFFER A PLUGIN THAT WILL NOT LOAD ------
        //
        // "Record plugins that fail to load (unlicensed / won't instantiate) ... so they're never
        // offered again in a pick." kHs Transient Shaper was offered, failed to instantiate on the
        // Link with "An OS error occurred during initialisation of the plug-in (-1)", and was then
        // offered AGAIN in the next round's pick - because the inventory is a list of what is
        // INSTALLED and nothing had ever recorded what does not run.
        //
        // The mark is cleared the moment the plugin loads for real, so this removes nothing
        // permanently: a user who signs into iLok gets it back on the next successful load. And a
        // plugin the user NAMES is still used as named - the add path does not consult this list,
        // which is Sean's standing ruling about a named plugin.
        const auto wontLoad = ChainHost::loadFailedPluginNames();
        for (const auto& bad : wontLoad) names.removeString (bad);
        if (! wontLoad.isEmpty())
            EchoJay_NSLog (("EJAgent: the inventory withholds " + juce::String (wontLoad.size())
                            + " plugin(s) that did not load here (" + wontLoad.joinIntoString (", ")
                            + ") - they are not offered in a pick until they load").toRawUTF8());
        return names;
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
    // FIX 1(b), 14 Oct: the read-back a `set` needs. Two roads, because a Link is another process.
    s.slotParam = [p] (const juce::String& uid, int slot1Based, const juce::String& paramId, double& out) -> bool
    {
        if (slot1Based < 1 || paramId.isEmpty()) return false;
        if (uid.isEmpty())
            return p->getChainHost().slotParamValue (slot1Based - 1, paramId, out);   // the device's own value
        // ---- A LINK, AND SEAN'S #1 (16 Oct 2026): THE SIDECAR'S `landed` FIELD, NOTHING ELSE --------
        //
        // This used to parse numbers out of the slot's `settings` TEXT. That text is composed from
        // formatSemanticSetting(semantic, requestedValue) - the value the turn ASKED for - so the
        // read-back was comparing a request with an echo of itself and could confirm a setting that
        // never reached the plugin. It is why three third-party adds on the Kick's Link reported
        // honestly-worded results while MTransient, bx_townhouse and X-Comp all sat at their defaults:
        // the dial failed for want of a map, and the only thing that could have caught it was reading
        // the same string the request wrote.
        //
        // `landed` carries the numbers the PLUGIN shows (or a built-in device's own getParamValue), set
        // by ChainHost on both apply roads. Absent or missing this id = NOT READABLE, which the caller
        // reports as `not_confirmed` - a different statement from "it did not land", and the only
        // honest one when a Link's binary predates the field.
        int err = 0; const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty()) return false;
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (! rc.valid || slot1Based > (int) rc.slots.size()) return false;
        const auto landed = rc.slots[(size_t) (slot1Based - 1)].landedValues;
        if (landed.isEmpty()) return false;
        for (const auto& pair : juce::StringArray::fromTokens (landed, ";", ""))
        {
            const auto id = pair.upToFirstOccurrenceOf ("=", false, false).trim();
            if (id != paramId) continue;
            const auto num = pair.fromFirstOccurrenceOf ("=", false, false).trim();
            if (num.isEmpty()) return false;
            out = num.getDoubleValue();
            return true;
        }
        return false;
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
    s.beginGrWindow = [p]
    {
        const int n = p->getChainHost().resetGrWindows();
        EchoJay_NSLog (("EJAgent: a listen began - the gain-reduction window was reset on "
                        + juce::String (n) + " slot(s) (Sean's checks 2 and 3, 15 Oct)").toRawUTF8());
    };
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
