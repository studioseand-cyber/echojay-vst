#pragma once
#include <JuceHeader.h>

/*  EJChainRole.h - 21t-m item 5 (29 Sep 2026 ruling): THE CHAIN'S ROLE HAS THREE SOURCES, NOT ONE.

    The 29 Sep bus fix read only the start-prompt channelType, so a track literally NAMED "Mix Bus" with the
    prompt unanswered was still a channel - which is Sean's own demo session, where the [KEY] block said
    "declared Mix Bus" while the level restore treated it as a channel and landed on it.

    THE RULING, verbatim: "the chain is a bus if ANY of these says so: (a) the start-prompt choice (channelType
    is any *Bus, MasterBus or FullMix); (b) the Link's placement selector says bus; (c) the host track name reads
    as one - contains 'bus', 'master', 'mix bus', '2-bus', 'stem', 'sum', 'print', case-insensitive, whole words.
    One function, chainRole(), returning kind (bus/channel) and which source decided it; every consumer reads
    that, nothing reads channelType directly for role. Where sources disagree, bus wins and the block says which
    source said bus - never silently."

    The DECISION lives here, as a pure function of three inputs, so the plugin, the Link and a guard all reach
    the same verdict from the same words and none of them has to own the table.
*/
namespace echojay
{

struct ChainRole
{
    enum class Kind { channel, bus };
    Kind         kind = Kind::channel;
    juce::String from;        // "prompt" | "placement" | "name", empty when nothing said bus
    juce::String trackName;   // whatever the host reported, for the wire and the line

    bool isBus() const noexcept { return kind == Kind::bus; }
    juce::String kindText() const { return kind == Kind::bus ? "bus" : "channel"; }

    /** The wire shape, ruled: {"kind":"bus"|"channel","from":"prompt"|"placement"|"name","name":"<track name>"}.
        `from` is omitted on a channel, because nothing decided it - "channel" is what is left when no source
        says bus, and a from of "prompt" on a channel would claim a decision that was never taken. */
    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("kind", kindText());
        if (from.isNotEmpty())      o->setProperty ("from", from);
        if (trackName.isNotEmpty()) o->setProperty ("name", trackName);
        return juce::var (o);
    }

    /** One line for the block header and the log, naming the source when one said bus. */
    juce::String text() const
    {
        juce::String s = kindText();
        if (kind == Kind::bus && from.isNotEmpty())
        {
            s << " (said by the " << (from == "prompt"    ? "channel-type prompt"
                                   :  from == "placement" ? "Link's placement selector"
                                                          : "host track name");
            if (from == "name" && trackName.isNotEmpty()) s << " \"" << trackName << "\"";
            s << ")";
        }
        return s;
    }
};

/** THE NAME TEST, ruled: whole words, case-insensitive. The listed phrases "mix bus" and "2-bus" are already
    caught by the word "bus" once the name is split on anything that is not a letter or a digit, which is what
    makes "2-bus", "Mix Bus" and "BUS 3" all read the same. Split-on-non-alphanumeric is also what keeps
    "Bussing", "Mastered vox" and "Printer" out: they are single words that are not in the table. */
inline bool trackNameReadsAsBus (const juce::String& raw)
{
    static const char* kWords[] = { "bus", "master", "stem", "sum", "print" };
    const auto lower = raw.toLowerCase();
    juce::String word;
    auto isBusWord = [] (const juce::String& w)
    {
        for (auto* k : kWords) if (w == k) return true;
        return false;
    };
    for (auto c : lower)
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c)) word << juce::String::charToString (c);
        else { if (isBusWord (word)) return true; word.clear(); }
    }
    return isBusWord (word);
}

/** THE DECISION. promptIsBus = the start-prompt choice is any *Bus, MasterBus or FullMix; placement = the Link's
    placement selector (1 = bus, 2 = channel/insert, 0 = unset, and V2 has none so it passes 0); trackName = what
    the host reported. Bus wins over channel, and the source reported is the FIRST in the ruled order (a, b, c)
    that said bus - a decision the ruling does not name, taken so the answer is deterministic when two agree. */
/** THE VOCAL/RHYTHM NAME TEST, ruled 29 Sep 2026: the same whole-word split as the bus test. A bus whose name
    says vocals or drums is a bus and is NOT the music. */
inline bool nameReadsAsVocalOrRhythm (const juce::String& raw)
{
    static const char* kWords[] = { "vocal", "vocals", "vox", "bv", "bvs", "harmony", "harmonies",
                                    "drum", "drums", "perc", "percussion" };
    const auto lower = raw.toLowerCase();
    juce::String word;
    auto isOne = [] (const juce::String& w)
    {
        for (auto* k : kWords) if (w == k) return true;
        return false;
    };
    for (auto c : lower)
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c)) word << juce::String::charToString (c);
        else { if (isOne (word)) return true; word.clear(); }
    }
    return isOne (word);
}

/** IS THIS CHAIN THE MUSIC? (29 Sep 2026 ruling, narrowing chainRole.) Self key detection runs, and the reading
    ranks as a PRIMARY key source, when the chain is a BUS and nothing says it is a vocal or rhythm bus: the
    prompt answered VocalBus or DrumBus, or the name reads as vocals or drums. So "Mix Bus" with the prompt
    unanswered IS the music and "Master" is; "Vocal Bus", a VocalBus prompt answer and "Drum Bus" are not.

    This is the ONLY place the music question is decided. Nothing reads channelType for it: the prompt's two
    disqualifying answers come in as booleans, exactly as promptIsBus does for the role. */
inline bool decideChainIsMusic (const ChainRole& role, bool promptIsVocalBus, bool promptIsDrumBus,
                                const juce::String& trackName)
{
    if (! role.isBus())                        return false;   // a channel is never the music
    if (promptIsVocalBus || promptIsDrumBus)   return false;   // the user said which bus, and it is not the music
    if (nameReadsAsVocalOrRhythm (trackName))  return false;   // ...or the name did
    return true;
}

inline ChainRole decideChainRole (bool promptIsBus, int placement, const juce::String& trackName)
{
    ChainRole r;
    r.trackName = trackName.trim();
    if (promptIsBus)                            { r.kind = ChainRole::Kind::bus; r.from = "prompt";    return r; }
    if (placement == 1)                         { r.kind = ChainRole::Kind::bus; r.from = "placement"; return r; }
    if (trackNameReadsAsBus (r.trackName))      { r.kind = ChainRole::Kind::bus; r.from = "name";      return r; }
    return r;
}

} // namespace echojay
