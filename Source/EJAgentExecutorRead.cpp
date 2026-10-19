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

// ---- WHAT A BUILD WOULD REMOVE (16 Oct 2026, Sean's step 7.4) -------------------------------
//
// His report: "make the kick punchier" on a Link already holding Transient Master and Renaissance
// Axx proposed "build 3 slots: EchoJay EQ, EchoJay Compressor, EchoJay Limiter", which REPLACES the
// existing chain, with no explanation. Whether the model should have proposed a rebuild at all is
// B's (FOR_B §17); what this build owes him either way is the DISCLOSURE, before the press.
//
// A `build` carries the whole rack it wants, so the answer is the live rack minus what the block
// names: anything on the rack today that the block does not ask for is coming off. Matched on name,
// loosely (the same comparison the read-back uses), because the block writes display names and so
// does the rack. Anything that is not a build, or a build that keeps everything, returns nothing.
juce::StringArray ExecutorRead::buildWouldRemove (const ToolCall& call)
{
    juce::StringArray going;
    if (argString (call, "op").trim().toLowerCase() != "build") return going;
    const auto chain = call.args.getDynamicObject() != nullptr
                           ? call.args.getDynamicObject()->getProperty ("chain") : juce::var();
    auto* arr = chain.getArray();
    if (arr == nullptr) return going;                 // no block: nothing to compare, nothing claimed
    // 19 Oct 2026: through the one resolver, now that check(gr) needs it too. This was the first
    // copy of the idiom and it was right; keeping it as a copy is what let the third one be wrong.
    const auto rack = rackForCall (call);
    if (! rack.valid) return going;                  // the rack cannot be read: claim nothing
    juce::StringArray wanted;
    for (const auto& e : *arr)
        if (auto* o = e.getDynamicObject()) wanted.add (o->getProperty ("name").toString().trim());
    for (const auto& sl : rack.slots)
    {
        bool kept = false;
        for (const auto& w : wanted)
            if (w.isNotEmpty() && namesMatchLoosely (sl.name, w)) { kept = true; break; }
        if (! kept && sl.name.isNotEmpty()) going.add (sl.name);
    }
    return going;
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

// ---- THE RACK THIS CALL IS ABOUT (19 Oct 2026) ------------------------------------------------
// See the header for why this is one function rather than a third copy. The one subtlety: an
// argument that does not resolve comes back from resolveChannelArg as the RAW string, and handing
// that to linkRack yields an invalid rack whose `why` quotes what the model sent - which is the
// refusal the caller wants, with knownChannelsSentence() appended. Answering about the open rack
// instead is the defect this replaces.
RackRead ExecutorRead::rackForCall (const ToolCall& call) const
{
    const auto asked = resolveChannelArg (call);
    if (asked.isEmpty() || asked == target_) return targetRack();
    return src_.linkRack ? src_.linkRack (asked) : RackRead();
}

juce::var ExecutorRead::channelVar (const RackRead& r)
{
    auto* ch = new juce::DynamicObject();
    ch->setProperty ("uid",  r.uid.isNotEmpty() ? r.uid : juce::String ("self"));
    ch->setProperty ("name", r.name);
    ch->setProperty ("kind", r.kind);
    return juce::var (ch);
}

// ---- ITEM 13 (18 Oct 2026): IS THE TARGET MEASURABLE AT ALL? ----------------------------------
// The open rack always is - the measurement is this process's own. A Link is, when its build
// publishes its slots' measurements (meterCapable), which every build from today does. Said about
// THE TARGET, so a start context can state a limit that is true rather than one that was.
bool ExecutorRead::linkMetersAvailable() const
{
    if (target_.isEmpty()) return true;
    const auto r = targetRack();
    return r.valid && r.meterCapable;
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
        // BUSY WINS OVER GONE (18 Oct 2026, queue item 13). They are mutually exclusive on purpose: a rack
        // that is rebuilding is reachable, it is simply not answering yet, and a reader told both would have
        // to decide which to believe.
        if (t.busy)          to->setProperty ("busy", true);
        else if (! t.fresh)  to->setProperty ("gone", true);
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

// ---- A LOUDEST 3 s BELOW THE INTEGRATED FIGURE IS NOT A READING (19 Oct 2026) ----------------
//
// Sean, 10:55: *"check(playback)'s loudest 3 s keeps coming out QUIETER than integrated (-17.5 vs
// -12.2 here, same every run). Loudest short-term can't be below integrated."*  The publisher half
// is fixed where it belongs (LinkProcessor now max-holds on the integrated figure's own clock), but
// an EchoJay already on another track keeps publishing the old pair, and a reader that quotes an
// impossible pair is worse than one that says it has no figure. The max of a quantity cannot be
// below its mean, so this withholds it and says why rather than printing either number as if the
// two described the same audio. Half a dB of slack: the two instruments round independently.
static void reportLoudest3s (juce::DynamicObject* o, const AnalysisRead& a,
                             void (*setter) (juce::DynamicObject*, const char*, float))
{
    const bool haveBoth = a.shortTermMaxLufs > -199.0f && a.integratedLufs > -199.0f;
    if (haveBoth && a.shortTermMaxLufs < a.integratedLufs - 0.5f)
    {
        o->setProperty ("loudest3sUnavailable",
                        "the loudest 3 s window on that track was measured over a different span "
                        "from its integrated level, so the two cannot be compared - that EchoJay is "
                        "an older build. Its integrated figure is sound.");
        EchoJay_NSLog (("EJAgentRead: WITHHELD loudest3sLufs - " + juce::String (a.shortTermMaxLufs, 1)
                        + " is below the integrated " + juce::String (a.integratedLufs, 1)
                        + " LUFS, which cannot be true of one span").toRawUTF8());
        return;
    }
    setter (o, "loudest3sLufs", a.shortTermMaxLufs);
}

juce::var ExecutorRead::analysisVar (const AnalysisRead& a)
{
    auto* o = new juce::DynamicObject();
    setLevel (o, "integratedLufs", a.integratedLufs);
    reportLoudest3s (o, a, &setLevel);                      // the loudest 3 s window = the short-term max
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
    return inventoryPage (names, {}, 0, kInventoryPage, juce::var());
}

juce::var ExecutorRead::inventoryPage (const juce::StringArray& all, const juce::String& match,
                                       int offset, int limit, juce::var notLoadable, bool census)
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
    // B's FOR_A 76, ask 1: a census page is allowed to be the whole list. An ordinary page keeps the
    // 200 it has always had, because an ordinary caller is still behind the server's own 8 KB trim.
    const int lim    = juce::jlimit (1, census ? kInventoryCensusMax : kInventoryPageMax,
                                     limit <= 0 ? kInventoryPage : limit);

    juce::Array<juce::var> arr;
    int returned = 0;
    for (int i = off; i < total && returned < lim; ++i, ++returned) arr.add (hit[i]);

    auto* o = new juce::DynamicObject();
    o->setProperty ("total",  total);            // how many MATCHED, before any paging or trimming
    o->setProperty ("offset", off);
    o->setProperty ("limit",  lim);
    if (match.isNotEmpty()) o->setProperty ("match", match);
    o->setProperty ("names", juce::var (arr));

    // ---- WHAT CANNOT LOAD, PAGE-SCOPED AND BOUNDED (17 Oct 2026, FOR_B §25) ---------------------
    //
    // 17b put EVERY unloadable plugin in this result, appended AFTER fitUnder8K had run - so on
    // Sean's machine, where 357 of 1,323 installed bundles have no arm64 slice, look(inventory)
    // came back about 49 KB and the server refused it in round 1 of every run. The rig has one or
    // two and never saw it.
    //
    // Three changes, and the third is the one that matters:
    //   * only the entries whose names are ON THIS PAGE, because that is what the model is looking
    //     at and all it can act on;
    //   * the reason capped, and at most twelve of them, so the field has a ceiling that does not
    //     depend on anyone's plugin folder;
    //   * `notLoadableTotal` carries the SCALE as a number instead of as bytes - the model can say
    //     "357 of your plugins will not run here" without being sent 357 of anything;
    //   * and all of it added HERE, before the single 8 KB gate below, which is the actual bug: a
    //     field added after the gate is a field no gate measures.
    //
    // ---- AND ON A CENSUS PAGE, ALL OF THEM, BY NAME (18 Oct 2026, B's FOR_A 76 ask 2) -----------
    //
    // B: *"the pages name at most twelve and declare notLoadableTotal; on Sean's run they named none
    // (total 180), so the census could only say '0 named that can't run - 180 plugins in all can't
    // run here and the plugin does not say which are compressors'."*
    //
    // Named none of 180 is not bad luck, it is the page-scope test meeting the inventory's own rule:
    // `names` is what can be OFFERED (getFilteredPlugins applies the withhold predicate), and a
    // plugin that cannot run here is withheld - so it is never in the page the scope test checks
    // against. The field could only ever have been empty on a real machine, and only the rig's one
    // or two self-inflicted load failures ever appeared in it.
    //
    // So a census page sends the list WHOLE and unscoped, each entry with the name, the reason, and
    // the TYPE the plugin actually knows from its own scan (the declared category and the format).
    // The role - "is it a compressor" - is deliberately NOT guessed here: third-party roles are the
    // server's to know from its maps, which is the rule roleOfBuiltin states at the top of this file.
    // The category is what the plugin itself declares, which is a fact, not a classification.
    if (! notLoadable.isVoid())
    {
        juce::Array<juce::var> listed;
        int totalBad = 0;
        if (auto* bad = notLoadable.getArray())
        {
            totalBad = bad->size();
            for (const auto& e : *bad)
            {
                if (! census && listed.size() >= 12) break;
                const auto nm = e.getProperty ("name", juce::var()).toString();
                if (nm.isEmpty()) continue;
                if (! census && ! arr.contains (juce::var (nm))) continue;
                auto* t = new juce::DynamicObject();
                t->setProperty ("name", nm);
                t->setProperty ("why", capped (e.getProperty ("why", juce::var()).toString(), 72));
                const auto ty = e.getProperty ("type", juce::var()).toString();
                if (ty.isNotEmpty()) t->setProperty ("type", capped (ty, 48));
                listed.add (juce::var (t));
            }
        }
        if (totalBad > 0) o->setProperty ("notLoadableTotal", totalBad);
        if (! listed.isEmpty())
        {
            o->setProperty ("notLoadable", juce::var (listed));
            o->setProperty ("notLoadableNote", census
                ? juce::String ("every plugin installed here that cannot run, by name, with the type it "
                                "declares - these are NOT in `names` (a plugin that cannot run is not "
                                "offered), so count them separately")
                : juce::String ("these are on this page and cannot run here - say so and offer a working "
                                "alternative of the same role"));
        }
    }

    // `count` is NOT the page length. Before paging existed this function set count = names.size() BEFORE
    // fitUnder8K halved the array, so every caller that has ever read `count` read the number of plugins
    // INSTALLED, with `truncated` saying the names were only a slice of it. look(list_tracks) uses `count` the
    // same way. So count stays the total: a server that reads it to answer "how many do I have" keeps getting
    // the honest answer, and `returned` is the new field for the length of this page.
    // ---- THE GATE, OR THE CENSUS CEILING ---------------------------------------------------------
    //
    // An ordinary page goes through the 8 KB gate it always has. A census page does not: the step
    // endpoint does not trim those (B's FOR_A 76), and fitUnder8K's trim is the wrong shape for one
    // anyway - it HALVES the array, so `offset + returned` would stop describing the slice the server
    // asked for. Instead the page is measured whole and names come off the END until it fits, which
    // leaves offset/returned/hasMore true and lets the server simply ask for the next offset.
    //
    // The var is created BEFORE the loop on purpose: `o` is a raw DynamicObject* until a var owns it,
    // and building a temporary var per iteration would hand it a reference count that drops to zero.
    juce::var v;
    if (census)
    {
        v = juce::var (o);                      // owns `o` from here; measured and mutated through both
        int rounds = 0;
        while (arr.size() > 1 && jsonBytes (v) > kCensusMaxBytes && ++rounds < 64)
        {
            arr.removeRange (juce::jmax (1, arr.size() * 9 / 10), arr.size());
            o->setProperty ("names", juce::var (arr));
        }
    }
    else
        v = fitUnder8K (juce::var (o));

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
    // ---- THE RECENT CHAT (16 Oct 2026, Sean's step 6, rule (a)) ---------------------------------
    // "every agent run gets the recent chat (my messages and the agent's last reply) as context, so
    // a reply continues the job it answers." He asked for gain reduction, the run asked him to play
    // and ENDED, he said "im playing it", and the next run had no idea what it was answering.
    // Sent whether or not the model uses it yet; using it is B's half (FOR_B §16).
    if (src_.recentChat != nullptr)
    {
        const auto rc = src_.recentChat();
        if (rc.isArray() && ! rc.getArray()->isEmpty()) o->setProperty ("recentChat", rc);
    }
    // ---- AND WHETHER THE TRANSPORT IS ALREADY PLAYING (rule (c)) -------------------------------
    // "if the transport is already playing with signal, listen instead of asking." The model cannot
    // know that unless it is told: his run asked him to start playback while playback was running.
    // `playing` is the host's own transport flag, `known` says whether the host tells us at all, and
    // `heardSeconds` is how much audio this rack has actually measured - the three facts between
    // them say "there is signal here now", which is the condition the rule turns on.
    {
        auto* tr = new juce::DynamicObject();
        const bool known   = src_.transportKnown   != nullptr && src_.transportKnown();
        const bool playing = src_.transportPlaying != nullptr && src_.transportPlaying();
        tr->setProperty ("known", known);
        tr->setProperty ("playing", playing);
        double heard = 0.0;
        if (src_.ownAnalysis != nullptr) { const auto a = src_.ownAnalysis(); heard = a.heardSeconds; }
        tr->setProperty ("heardSeconds", heard);
        tr->setProperty ("signalNow", playing && heard > 0.0);
        if (playing && heard > 0.0)
            tr->setProperty ("note", "audio is playing through this rack right now - measure it rather "
                                     "than asking for playback");
        o->setProperty ("transport", juce::var (tr));
    }
    juce::Array<juce::var> caps; caps.add ("look"); caps.add ("check"); caps.add ("wait_for_playback"); caps.add ("checkpoint");
    // ---- ITEM 13 (18 Oct 2026): link_meters, the flag B has been waiting for -------------------
    // A Link publishes its slots' measurements now, so a measurement on another track's Link is a
    // thing the server may PLAN. Announced as a capability rather than inferred from the absence of
    // a `cannot` entry: B asked for the positive statement, and a reader that walks neither list
    // still has the flat grReadableOnLink below.
    const bool linkMeters = linkMetersAvailable();
    if (linkMeters) caps.add ("link_meters");
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
        // ---- ITEM 13: AND IT IS NOW A FACT ABOUT THIS TARGET, NOT ABOUT LINKS -------------------
        // This entry was unconditional, which is how his agent learned a limit that has gone. It is
        // emitted only when the Link in front of us genuinely cannot be measured, and it says WHICH
        // of the two reasons it is - an older build over there, or no fresh reading yet - because
        // those have different answers and only one of them is worth retrying.
        if (! linkMeters && target_.isNotEmpty())
        {
            auto* gr = new juce::DynamicObject();
            gr->setProperty ("what", "gr");
            gr->setProperty ("where", "link");
            gr->setProperty ("why", "that track's EchoJay is an older build that does not publish its "
                                    "slots' measurements");
            gr->setProperty ("retryable", false);
            gr->setProperty ("instead", "ask the user to update EchoJay on that track, or measure from "
                                        "the rack this window has open");
            cannot.add (juce::var (gr));
        }
        o->setProperty ("cannot", juce::var (cannot));
        // The same fact in the flat form, for a reader that does not walk `cannot`.
        o->setProperty ("grReadableOnLink", linkMeters);
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
        // MARKED, NOT HIDDEN (item 3) - and handed to inventoryPage so it is inside the 8 KB gate
        // rather than appended after it, which is the defect FOR_B §25 is about.
        //
        // THE CENSUS (18 Oct 2026, B's FOR_A 76): the server's own inventory sweep stamps its calls
        // `tc_census_<n>` and does not trim their answers at 8 KB, so those - and only those - are
        // allowed the whole list and the whole cannot-run list. One marker, read the same way on both
        // sides; every other caller's bytes are exactly what they were.
        const bool census = isCensusCallId (call.id);
        auto page = inventoryPage (names, match.trim(), offset, limit,
                                   src_.notLoadable ? src_.notLoadable() : juce::var(), census);
        if (census)
            EchoJay_NSLog (("EJAgent: census page for " + call.id + " - asked limit=" + juce::String (limit)
                            + " offset=" + juce::String (offset) + ", returned "
                            + page.getProperty ("returned", juce::var()).toString() + " of "
                            + page.getProperty ("total", juce::var()).toString() + " name(s), "
                            + page.getProperty ("notLoadableTotal", juce::var()).toString()
                            + " cannot-run named, " + juce::String (jsonBytes (page)) + " bytes").toRawUTF8());
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
            reportLoudest3s (o, a, &setLevel);              // 19 Oct: never an impossible pair
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
        // ---- ITEM 13 (18 Oct 2026): A LINK CAN BE MEASURED NOW -------------------------------------
        //
        // *"On the Kick's Link, check gr fails with 'Gain reduction cannot be read from a Link yet', so
        // the certified-compressor measurement can't run on any Link."*
        //
        // It can, because the Link publishes its slots' own measurements (EJRackSidecarFill, from the
        // same slotPicture the open rack is read through). What remains refusable is NOT "a Link":
        //   - that Link's build does not publish meters at all (meterCapable false), and
        //   - the reading is older than the window a measurement may honestly be quoted from.
        // Each gets its own sentence, because "cannot be read from a Link yet" was true of neither and
        // sent him looking for a limit that no longer exists. ITEM 8's rule stands - the answer comes
        // before the rack and the slot - it is just a narrower answer.
        // ---- AND IT IS THE RACK THE CALL NAMED (19 Oct 2026) ------------------------------------
        // This was targetRack(): the channel argument was resolved only to decide WHETHER a Link
        // was involved, so a call about the kick was answered about whatever the window had open -
        // consistently, from one rack, and about the wrong one. Every read below comes off `r`, and
        // the input measurement comes off `r.uid`, so the whole answer belongs to the rack that was
        // asked about rather than to a uid resolved a second time beside it.
        const auto r = rackForCall (call);
        // ---- THE PLUGIN'S OWN TRUTH COMES FIRST (18 Oct 2026, after his third report) ------------
        //
        // He asked three times why a CL 1B on a Link said "no fresh reading", and the answer was not
        // freshness at all: NOTHING in this build reads a third party's gain reduction. slotPicture
        // produces a GR only for an EchoJay device, and grCertified is never set for anything else -
        // so the honest answer for his CL 1B is the not_certified one below, on a Link or on the open
        // rack alike. My Link gates were asked FIRST and replaced a true, actionable refusal with a
        // misleading one. Whether a plugin can be measured at all cannot change between now and a
        // retry; how old a reading is can. The unchangeable question is answered first - which is
        // ITEM 8's own rule, applied to the gate I added.
        const bool slotIsReadable = [&]
        {
            if (! r.valid || slotArg < 1 || slotArg > (int) r.slots.size()) return true;   // other gates answer
            const auto& sl = r.slots[(size_t) slotArg - 1];
            return sl.builtin || sl.grCertified;
        }();
        if (measuringALink (call) && r.valid && slotIsReadable)
        {
            if (! r.meterCapable)
            {
                done (ToolOutcome::failureFor ("link_has_no_meters",
                        "that Link's EchoJay does not publish its slots' measurements - it is an older "
                        "build. Ask the user to update EchoJay on that track, or measure from the rack "
                        "this window has open. This will not work on a retry.",
                        "That track's EchoJay is an older build that does not send its meters over yet."));
                return;
            }
            const auto ageMs = r.measuredAtMs > 0 ? (juce::Time::currentTimeMillis() - r.measuredAtMs)
                                                  : (juce::int64) -1;
            // ---- HIS RULE: JUDGED AGAINST THE SECTION HE PLAYED ---------------------------------
            // A reading stamped at or after the moment wait_for_playback opened its window is OF
            // that section, however long the round trip then took. The clock is only the fallback,
            // for a run that never waited for playback at all.
            const bool insideThePlaybackHeWaitedFor = playbackWindowStartedMs_ > 0
                                                   && r.measuredAtMs >= playbackWindowStartedMs_;
            if (! insideThePlaybackHeWaitedFor && (ageMs < 0 || ageMs > linkMeterStaleMs))
            {
                // A FIGURE WITH NO AGE IS AN ESTIMATE (Sean's 15 Oct ruling, applied to the Link road).
                auto o = ToolOutcome::failureFor ("link_meters_stale",
                        "that Link's measurements are " + (ageMs < 0 ? juce::String ("of unknown age")
                                                                     : juce::String (ageMs / 1000) + " s old")
                        + ", which is too old to quote as a reading. Ask the user to play the section and "
                          "call wait_for_playback, then measure again.",
                        "I have no fresh reading from that track yet - play the loudest section and I "
                        "will measure it.");
                auto* e = new juce::DynamicObject();
                e->setProperty ("retryable", true);
                e->setProperty ("hint", "ask the user to play, then wait_for_playback, then check(gr) again");
                o.result = juce::var (e);
                done (std::move (o));
                return;
            }
        }
        // ---- A RACK THAT COULD NOT BE READ IS A CHANNEL QUESTION (19 Oct 2026) ------------------
        // This used to answer notFromALink ("Gain reduction cannot be read from a Link yet"), which
        // ITEM 13 made untrue yesterday and which never addressed the actual case: the argument
        // named a channel that does not exist. Falling through to unknown_channel gives the model
        // r.why plus the list of channels that DO exist - the one refusal shape that lets a turn
        // name a real one or stop (item 6(d)). notFromALink stays where it is still true: a true
        // peak and a spectrum genuinely cannot be read from a Link.
        if (! r.valid) { done (ToolOutcome::failureFor ("unknown_channel", r.why + knownChannelsSentence(),
                                                        "I could not read that rack.")); return; }
        if (slotArg < 1 || slotArg > (int) r.slots.size()) { done (ToolOutcome::failure ("unknown_slot", "slot " + juce::String (slotArg) + " is not on the rack (1.." + juce::String ((int) r.slots.size()) + ")")); return; }
        const auto& s = r.slots[(size_t) slotArg - 1];
        // ---- WHAT THE SLOT IS, BEFORE WHAT IT HEARD (19 Oct 2026, his coverage ask) -------------
        //
        // Two slot types had no answer of their own, and both took the "no audio has been heard"
        // road - the false-reason shape of his #19, which sent him looking for a muted channel:
        //   an EMPTY slot    answered with the measurement under a BLANK name, so a figure was
        //                    attributed to nothing at all; and
        //   a BYPASSED slot  is not in circuit, so its tally is empty BY CONSTRUCTION - "nothing
        //                    has been heard" is true and useless, and "play the section again"
        //                    is advice that cannot work.
        // Both are properties of the slot, so both are answered before any listening question, the
        // same ordering ITEM 8 and his #19 both argue for. Neither is retryable.
        if (s.name.trim().isEmpty())
        {
            done (ToolOutcome::failureFor ("empty_slot",
                    "slot " + juce::String (slotArg) + " has nothing in it, so there is nothing to "
                    "measure. This will not change on a retry: name a slot that holds a plugin, or "
                    "ask what is on the rack.",
                    "There's nothing in slot " + juce::String (slotArg) + " to measure."));
            return;
        }
        if (s.bypassed)
        {
            done (ToolOutcome::failureFor ("slot_bypassed",
                    "slot " + juce::String (slotArg) + " (" + s.name + ") is BYPASSED, so no audio "
                    "passes through it and nothing can be measured there. This will not change on a "
                    "retry while it is bypassed: ask the user to switch it in, or measure another slot.",
                    s.name + " is bypassed, so there's nothing passing through it to measure."));
            return;
        }
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
        // ---- A THIRD PARTY GETS WHAT WE DO MEASURE (18 Oct 2026, Sean's ruling) ----------------
        //
        // *"EchoJay Compressor = its own meter. Certified third-party = Kathy's measured curve.
        // Anything else = no figure. So your part is small: check gr on a third-party slot returns
        // that slot's measured INPUT level over the listen window (loudest and typical, rms and
        // peak, with the window's start time so freshness is judged against the playback), plus the
        // slot's current read-back. B holds the profiles and decides whether a figure is allowed."*
        //
        // So the plugin stops deciding anything here. It does NOT read a third party's gain-reduction
        // meter, does NOT estimate one, and does not ask whether the plugin is certified - that is
        // B's question, with Kathy's curve behind it. What it hands back is the measurement it
        // actually has, named for what it is, with no field that could be mistaken for a reduction.
        if (! s.builtin)
        {
            // FROM THE RACK THAT ANSWERED (19 Oct 2026): r.uid, not target_. With target_ the
            // figures came off the open channel while the slot's name and read-back came off the
            // one the call named - two channels in one payload, if the names had ever differed.
            const auto lv = src_.slotInputLevels ? src_.slotInputLevels (r.uid, slotArg)
                                                 : Sources::SlotInputLevels();
            if (! lv.known)
            {
                done (notPlaying ("the input level on slot " + juce::String (slotArg)));
                return;
            }
            auto* io = new juce::DynamicObject();
            // WHICH CHANNEL ANSWERED (19 Oct 2026), on every gr payload: without it a routed read
            // and an unrouted one look identical to the model and to B, which is how the open rack
            // answered for the kick unnoticed.
            io->setProperty ("channel", channelVar (r));
            io->setProperty ("slot", slotArg);
            io->setProperty ("name", s.name);
            io->setProperty ("measurement", "slot_input_level");
            io->setProperty ("gainReduction", "not_read");       // said in the payload, not only in prose
            io->setProperty ("inputLoudestDb", r1 (lv.loudestDb));
            io->setProperty ("inputTypicalDb", r1 (lv.typicalDb));
            io->setProperty ("inputLoudestPeakDbtp", r1 (lv.loudestPeakDbtp));
            io->setProperty ("inputTypicalPeakDbtp", r1 (lv.typicalPeakDbtp));
            io->setProperty ("inputUnit", lv.kWeighted ? "LUFS" : "dBFS RMS");
            // ---- THE HOP PERCENTILES, IN KATHY'S UNIT (19 Oct 2026, his test 1) ---------------
            // *"loudest and typical were both exactly -15.5, twice, on a kick; that can't be
            // right"* - they were the 400 ms percentiles returned as 1 dB BIN CENTRES, so two
            // percentiles in one bin came back byte-identical. These are p90/p50 of the 100 ms
            // hops, unquantised, plain unweighted RMS, with the window they describe.
            if (lv.rmsKnown)
            {
                io->setProperty ("inputLoudestRmsDb", r1 (lv.loudestRmsDb));
                io->setProperty ("inputTypicalRmsDb", r1 (lv.typicalRmsDb));
                io->setProperty ("inputRmsUnit", "dBFS RMS");
                io->setProperty ("inputRmsWindowSeconds", r1 (lv.rmsSeconds));
            }
            // ---- AND THE OUT LEG AND THE CRESTS (19 Oct 2026, for B's no-profile fallback) ----
            // Named for what they are. In minus out is NOT here: that difference is the figure his
            // 18 Oct ruling took out, and B decides from the profile what may be said.
            if (lv.outRmsKnown)
            {
                io->setProperty ("outputLoudestRmsDb", r1 (lv.outLoudestRmsDb));
                io->setProperty ("outputTypicalRmsDb", r1 (lv.outTypicalRmsDb));
            }
            if (lv.inCrestKnown)  io->setProperty ("inputCrestDb",  r1 (lv.inCrestDb));
            if (lv.outCrestKnown) io->setProperty ("outputCrestDb", r1 (lv.outCrestDb));
            io->setProperty ("windowSeconds", r1 (lv.windowSeconds));
            // THE WINDOW'S OWN START, so B judges freshness against the playback rather than a clock
            // it cannot see (his ruling, same message).
            if (playbackWindowStartedMs_ > 0)
                io->setProperty ("windowStartedMs", (double) playbackWindowStartedMs_);
            if (s.settings.isNotEmpty()) io->setProperty ("readBack", s.settings);
            if (s.dialSummary.isNotEmpty()) io->setProperty ("readBackSummary", s.dialSummary);
            io->setProperty ("note", "EchoJay does not read this plugin's gain-reduction meter and does "
                                     "not estimate one. These are the levels measured at its INPUT and "
                                     "its OUTPUT over the same listen window, in plain unweighted dBFS "
                                     "RMS (p90 = loudest, p50 = typical, over 100 ms hops), with each "
                                     "leg's crest (peak minus RMS) and what the slot is set to. The "
                                     "difference between the two legs is not a reduction and is not "
                                     "given as one: any reduction figure must come from the profile for "
                                     "this plugin, not from here.");
            done (ToolOutcome::successFor (fitUnder8K (juce::var (io)), {},
                        // ---- THE ROW SAYS WHICH FIGURE IS WHICH (19 Oct 2026) ----------------
                        // *"RMS input + output and crest fields with the labelled step text."*
                        // Six figures on one row is unreadable unless each carries its label, and
                        // the unit is said ONCE at the end rather than after every number.
                        [&]
                        {
                            juce::String row = "slot " + juce::String (slotArg) + " (" + s.name + "): ";
                            if (lv.rmsKnown)
                                row << "input " << juce::String (lv.loudestRmsDb, 1) << " loudest / "
                                    << juce::String (lv.typicalRmsDb, 1) << " typical";
                            else
                                row << "input " << juce::String (lv.loudestDb, 1) << " loudest / "
                                    << juce::String (lv.typicalDb, 1) << " typical";
                            if (lv.outRmsKnown)
                                row << ", output " << juce::String (lv.outLoudestRmsDb, 1) << " loudest / "
                                    << juce::String (lv.outTypicalRmsDb, 1) << " typical";
                            row << " dBFS RMS";
                            if (lv.inCrestKnown)  row << ", crest in " << juce::String (lv.inCrestDb, 1);
                            if (lv.outCrestKnown) row << " out " << juce::String (lv.outCrestDb, 1);
                            if (lv.inCrestKnown || lv.outCrestKnown) row << " dB";
                            row << ", over " << juce::String (lv.rmsKnown ? lv.rmsSeconds : lv.windowSeconds, 1)
                                << " s; no GR meter is read for a third party";
                            return row;
                        }(),
                        "I measured what reaches " + s.name + " and what leaves it: in "
                        + juce::String (lv.rmsKnown ? lv.loudestRmsDb : lv.loudestDb, 1) + " loudest, "
                        + juce::String (lv.rmsKnown ? lv.typicalRmsDb : lv.typicalDb, 1) + " typical"
                        + (lv.outRmsKnown ? ("; out " + juce::String (lv.outLoudestRmsDb, 1) + " loudest, "
                                             + juce::String (lv.outTypicalRmsDb, 1) + " typical")
                                          : juce::String())
                        + " dBFS RMS. I cannot read its own gain-reduction meter."));
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
        o->setProperty ("channel", channelVar (r));          // 19 Oct: the same stamp on the device road
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
        // ---- AND A LINK'S TRUE PEAK, FOR THE SAME REASON (19 Oct 2026) -----------------------
        // *"The full analysis must be reachable for every Link, just like it is when you capture."*
        // The same stale gate as the spectrum one above: a Link's rack read carries each slot's own
        // outTpDb (the sidecar publishes it) and its frame carries truePeakMax, so both sources this
        // branch reads were already crossing. Not in his report, named here because it is the same
        // defect in the same vocabulary and would have been his next one.
        const auto r = rackForCall (call);
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
        // ---- A LINK'S SPECTRUM IS PUBLISHED, AND HAS BEEN (19 Oct 2026, his test 7) -----------
        //
        // *"check spectrum failed twice with 'The analyser's spectrum cannot be read from a Link
        // yet'. The full analysis must be reachable for every Link, just like it is when you
        // capture... check spectrum on a Link must answer from one of those, never 'cannot be
        // read'."*  And he named the evidence himself: look(analysis) read this very data on that
        // Link in the same session.
        //
        // The refusal was a leftover of the same kind item 13 removed from check(gr) yesterday: the
        // Link's meter frame carries the six pink-referenced bands (analysisFromFrame sets
        // haveBands and fills bandRelDb from the frame), and targetAnalysis() returns exactly that
        // for a Link - so the data the gate was refusing had already crossed. His ROUTE 2 in its
        // cheapest form: answer from what the Link publishes.
        const auto a = targetAnalysis();
        if (! a.valid || ! a.haveBands)
        {
            // AND THE REFUSAL IS ABOUT PLAYBACK, NOT ABOUT LINKS: "cannot be read from a Link" was
            // untrue and unactionable. This one says what to do and can change on a retry.
            done (ToolOutcome::failureFor ("no_spectrum_yet",
                    juce::String (target_.isNotEmpty() ? "that track's EchoJay has not published a "
                                                         "spectrum yet"
                                                       : "no spectrum has been measured here yet")
                    + " - ask the user to play a few seconds and try again.",
                    target_.isNotEmpty() ? "I haven't had a spectrum from that track yet - play a few "
                                           "seconds and I'll read it."
                                         : "I haven't measured a spectrum yet - play a few seconds."));
            return;
        }
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
    // ---- WHEN THE SECTION HE PLAYED BEGAN (18 Oct 2026, his ruling) ----------------------------
    // *"Freshness should be judged against the playback the agent just waited for, not against
    // 'now'. A reading taken after wait-for-playback started covers the section I played, so it's
    // the right one to quote even if it's 10 s old when check gr runs."*  He is right, and it is a
    // better rule than the clock: a Link's reading stamped inside this window IS of that section,
    // and the thing the 15 Oct ruling forbids is a window from BEFORE it.
    playbackWindowStartedMs_ = juce::Time::currentTimeMillis();
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
        // ONE TALLY, ONE RESET: on the open rack these two describe the same span (19 Oct).
        r.loudestWindowSeconds     = out.heardSeconds;
        r.integratedIsLongerWindow = false;
        return r;
    }
    const auto a = targetAnalysis();
    if (! a.valid) return r;
    r.heardAboveSeconds = juce::jmax (0.0f, a.heardSeconds - linkHeardBase_);
    if (a.playing) r.playing = true;             // the frame's own "audio flowing" counts as playing for a remote rack
    if (finiteLevel (a.integratedLufs)) r.integratedLufs = a.integratedLufs;
    if (finiteLevel (a.shortTermMaxLufs)) r.loudestShortTermLufs = a.shortTermMaxLufs;
    if (finiteLevel (a.truePeakMaxDb)) r.truePeakDbtp = a.truePeakMaxDb;
    // TWO INSTRUMENTS ON A LINK: the frame's integrated is kept since that rack last changed, which
    // is longer than this window and is why his loudest read BELOW it. Named, not guessed at.
    r.loudestWindowSeconds     = a.heardSeconds;
    r.integratedIsLongerWindow = true;
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
        r.meterCapable = sc.meterCapable; r.measuredAtMs = sc.measuredAtMs;
        int i = 0;
        for (const auto& ss : sc.slots)
        {
            SlotRead s;
            s.n = ++i;
            s.name = ss.name; s.format = ss.format; s.settings = ss.settings;
            s.bypassed = ss.bypassed; s.wet = ss.wet; s.preTrimDb = ss.preTrimDb; s.outGainDb = ss.outGainDb;
            s.builtin = ChainHost::isBuiltinName (ss.name);
            // ---- ITEM 13: THE LINK'S MEASUREMENTS (18 Oct 2026) ----------------------------------
            // Field for field from the sidecar, which carried slotPicture's own gates across. A Link
            // that does not publish them leaves every one at its no-reading floor, which is exactly
            // the state check(gr) already knows how to answer about.
            s.pictureValid  = ss.pictureValid;
            s.grKnown       = ss.grKnown;
            s.grCertified   = ss.grCertified;
            s.grDb          = ss.grDb;
            s.grPeakDb      = ss.grPeakDb;
            s.grTypicalDb   = ss.grTypicalDb;
            s.grLoudestDb   = ss.grLoudestDb;
            s.grHeldDb      = ss.grHeldDb;
            s.grWindowFrames = ss.grWindowFrames;
            s.inLufs  = ss.inLufs;  s.outLufs = ss.outLufs;
            s.inTpDb  = ss.inTpDb;  s.outTpDb = ss.outTpDb;
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
            t.connected = l.connected; t.audioFlowing = l.audioFlowing; t.active = l.active;
            // ---- LIVENESS IN TIME, NOT IN POLLS (18 Oct 2026, queue item 13) ---------------------------
            //
            // This read `l.heartbeatFresh`, and the field's own comment in PluginProcessor.h has said since
            // 4 October: "Use heartbeatAgeSeconds, not heartbeatFresh, for any decision about whether the
            // Link can answer". heartbeatFresh is `staleCycles < 6` - a count of CONSUMER POLLS with no
            // advance - and refreshLinkRegistry runs on the 1 Hz timer AND from several editor sites, so a
            // burst of activity (an agent round IS a burst of activity) runs that count up against a Link
            // that is alive and heartbeating normally. That is how the gate's S6 log came to carry "88_7even
            // Kick_2_01 isn't reachable from this session right now (it shows as gone)" about a Link that
            // three rows before and one row after talked to perfectly.
            //
            // So: wall clock, one shared floor, and the rack's own BUSY byte for the seconds after a rebuild
            // when a live Link genuinely cannot answer. A busy rack is not stale and not gone.
            t.busy  = l.busy;
            t.fresh = l.busy || (l.heartbeatAgeSeconds >= 0.0f
                                 && l.heartbeatAgeSeconds <= EchoJayProcessor::kLinkAnsweringWithinS);
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
    // ---- THE RECENT CHAT, FROM THE PROCESSOR'S OWN HISTORY (16 Oct 2026) -----------------------
    // The processor holds the conversation (chatHistory), which is what survives the window closing
    // and is the same list the transcript draws. The last few turns, oldest first, each capped so a
    // long answer cannot dominate the body; the agent run rows are included by their own prose,
    // because "the agent's last reply" is exactly what Sean's rule (a) names.
    s.recentChat = [p]
    {
        juce::Array<juce::var> turns;
        const auto& h = p->chatHistory;
        const int want = 6;
        const int from = juce::jmax (0, (int) h.size() - want);
        for (int i = from; i < (int) h.size(); ++i)
        {
            const auto& e = h[(size_t) i];
            auto text = e.content.trim();
            if (text.isEmpty()) continue;
            if (text.length() > 400) text = text.substring (0, 397).trim() + "...";
            auto* t = new juce::DynamicObject();
            t->setProperty ("role", e.role.isNotEmpty() ? e.role : juce::String ("assistant"));
            t->setProperty ("text", text);
            turns.add (juce::var (t));
        }
        return juce::var (turns);
    };
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
    // ---- WHAT CANNOT LOAD, AND WHY, BEFORE THE AGENT PLANS (17 Oct 2026, Sean's item 3) --------
    // Two sources, both already known to the plugin and neither reaching the model until now: the
    // session's remembered load failures (ChainHost::loadFailedPluginNames, each with the reason it
    // failed with) and the architecture verdict the rack's own validator uses - the one that said
    // "FabFilter Pro-C 2 is installed but its VST3 has no arm64 build" AFTER he had pressed Apply.
    s.notLoadable = [p] () -> juce::var
    {
        juce::Array<juce::var> out;
        // ---- WITH THE TYPE IT DECLARES (18 Oct 2026, B's FOR_A 76 ask 2) ------------------------
        // B could count none of Sean's 180 per type, because a name and a reason do not say what a
        // plugin is. `type` is the plugin's OWN declared category and format from the scan the host
        // already holds - a fact. Whether a "Dynamics" plugin is the compressor the user asked for
        // stays the server's call, from its maps.
        auto add = [&out, p] (const juce::String& name, const juce::String& why)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("name", name);
            o->setProperty ("loadable", false);
            o->setProperty ("why", why);
            const auto ty = p->getChainHost().scannedTypeNamed (name);
            if (ty.isNotEmpty()) o->setProperty ("type", ty);
            out.add (juce::var (o));
        };
        for (const auto& e : p->getChainHost().installedButCannotLoad())
            add (e.first, e.second);
        // SAY HOW MANY, because this is the field that broke his 17b: on his machine it is 357 and
        // on the rig's controlled list it is one or two, and nothing in any log said so.
        EchoJay_NSLog (("EJAgent: the inventory marks " + juce::String (out.size())
                        + " installed plugin(s) as unable to load here (page-scoped and capped in the "
                          "result; the count rides as notLoadableTotal)").toRawUTF8());
        return juce::var (out);
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
    // ---- IS THE DIAL STILL RUNNING ON THIS SLOT? (16 Oct 2026) ----------------------------------
    // The own rack answers from its ChainHost; a Link from its sidecar's additive `dialPending` key,
    // which an older Link does not write and which then reads false - the settled case, and exactly
    // the behaviour this build had before the field existed.
    s.slotDialPending = [p] (const juce::String& uid, int slot1Based) -> bool
    {
        if (slot1Based < 1) return false;
        if (uid.isEmpty())
            return p->getChainHost().getSlotInfo (slot1Based - 1).dialPending;
        int err = 0; const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty()) return false;
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (! rc.valid || slot1Based > (int) rc.slots.size()) return false;
        return rc.slots[(size_t) (slot1Based - 1)].dialPending;
    };
    // The knob's own resolution, for the read-back tolerance (18 Oct 2026). One road for both racks:
    // the map is in THIS process whether the slot is on the open rack or in a Link.
    s.paramStep = [p] (const juce::String& pluginName, const juce::String& paramId, double near) -> double
    {
        return p->getChainHost().mappedStepFor (pluginName, paramId, near);
    };
    // ---- WHAT A SLOT HEARD (18 Oct 2026, Sean's ruling) -------------------------------------
    // One road for both racks, as with paramStep: the open rack answers from its own per-slot
    // tallies, a Link from the measurement it publishes. This is what check(gr) hands back for a
    // third party - EchoJay does not read their meter and does not estimate one.
    s.slotInputLevels = [p] (const juce::String& uid, int slot1Based) -> Sources::SlotInputLevels
    {
        Sources::SlotInputLevels out;
        if (slot1Based < 1) return out;
        if (uid.isEmpty())
        {
            const auto lv = p->getChainHost().getSlotLevels (slot1Based - 1);
            if (! lv.measured || ! lv.in.known) return out;
            out.known      = true;
            out.kWeighted  = lv.in.kWeighted;
            out.loudestDb  = lv.in.p90;
            out.typicalDb  = lv.in.p50;
            out.windowSeconds = lv.in.windowSeconds;
            // 19 Oct 2026: the hop percentiles, through the Snapshot's own implementation
            out.rmsKnown   = lv.in.hopLevelPercentiles (out.typicalRmsDb, out.loudestRmsDb);
            out.rmsSeconds = lv.in.hopLevelSeconds();
            // 19 Oct 2026: the OUT leg of the same blend, over its own hops, and the two crests
            out.outRmsKnown   = lv.out.hopLevelPercentiles (out.outTypicalRmsDb, out.outLoudestRmsDb);
            out.inCrestKnown  = lv.in.hopCrest  (out.inCrestDb);
            out.outCrestKnown = lv.out.hopCrest (out.outCrestDb);
            juce::Array<float> hops;
            for (int k = 0; k < lv.in.hopTruePeakCount && k < (int) lv.in.hopTruePeakDb.size(); ++k)
                if (lv.in.hopTruePeakDb[(size_t) k] > -199.0f) hops.add (lv.in.hopTruePeakDb[(size_t) k]);
            if (! hops.isEmpty())
            {
                hops.sort();
                out.typicalPeakDbtp = hops[hops.size() / 2];
                out.loudestPeakDbtp = hops[juce::jmin (hops.size() - 1, (hops.size() * 9) / 10)];
            }
            else if (lv.in.truePeakDb > -199.0f)
                out.loudestPeakDbtp = out.typicalPeakDbtp = lv.in.truePeakDb;
            return out;
        }
        int err = 0; const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty()) return out;
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (! rc.valid || slot1Based > (int) rc.slots.size()) return out;
        const auto& ss = rc.slots[(size_t) (slot1Based - 1)];
        if (! ss.inLevelsKnown) return out;
        out.known           = true;
        out.kWeighted       = ss.inLevelsKWeighted;
        out.loudestDb       = ss.inLoudestDb;
        out.typicalDb       = ss.inTypicalDb;
        out.loudestPeakDbtp = ss.inLoudestPeakDbtp;
        out.typicalPeakDbtp = ss.inTypicalPeakDbtp;
        out.windowSeconds   = ss.inWindowSeconds;
        // 19 Oct 2026: the hop percentiles cross the sidecar as two figures and their window - the
        // ring itself stays on the track that measured it, because nothing over here could take a
        // different percentile over it than the one implementation already did.
        out.rmsKnown        = ss.inRmsKnown;
        out.loudestRmsDb    = ss.inLoudestRmsDb;
        out.typicalRmsDb    = ss.inTypicalRmsDb;
        out.rmsSeconds      = ss.inRmsSeconds;
        out.outRmsKnown     = ss.outRmsKnown;
        out.outLoudestRmsDb = ss.outLoudestRmsDb;
        out.outTypicalRmsDb = ss.outTypicalRmsDb;
        out.inCrestKnown    = ss.inCrestKnown;   out.inCrestDb  = ss.inCrestDb;
        out.outCrestKnown   = ss.outCrestKnown;  out.outCrestDb = ss.outCrestDb;
        return out;
    };
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
