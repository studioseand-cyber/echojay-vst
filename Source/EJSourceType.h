#pragma once
// =============================================================================================
//  EJSourceType - what a channel IS, and where that answer came from
// =============================================================================================
//  Sean's round (b), 20 Oct 2026, approved: *"A source type for each Link: the same type setting
//  the V2 has, guessed automatically from the track name (and B's classifier if that's cheap), with
//  Sean able to change it on the Link. Sent as channelRole on both roads."*
//
//  WHY THIS EXISTS AT ALL. B picked a delay for "Nafe Main_01" with no idea it was a vocal, and the
//  honest finding was that the fact did not exist: agent-off's channelRole was literally
//  {"kind":"channel","name":"Nafe Main_01"} - a kind and a name - and a Link's registry row carries
//  a PLACEMENT (insert / bus / send), which is where it sits, not what it is. The only source type
//  this product held was the V2's own ChannelType selector.
//
//  ---- THE RULE SEAN APPROVED: user > classifier > name, AND ABSENT WHEN UNSURE ---------------
//
//  A guess and a decision must not be the same value, so the answer travels with its PROVENANCE
//  and a lower one never overwrites a higher one:
//
//    User        he set it on the Link. Wins always, survives a rename, and is the only one that
//                persists in the Link's state.
//    Classifier  B's answer for this track name, cached by name.
//    Name        the local guess below.
//    (absent)    nobody could say. channelRole then carries kind and name exactly as it does today.
//
//  ABSENT IS A REAL ANSWER AND THE MOST IMPORTANT ONE. A wrong type is worse than none: B picking a
//  vocal chain for a drum bus is a worse failure than B asking. So the guesser is built to DECLINE,
//  and the leg that matters most is the one asserting it declines on his own track names.
//
//  WHOLE WORDS ONLY, which is why this tokenises instead of searching. A bassoon is not a bass and a
//  vocoder is not a vocal; substring matching is how a guesser earns distrust it cannot recover
//  from. Numeric tokens are ignored, so the host's own numbering ("_1_01", "88_7even") costs
//  nothing and needs no stripping rules of its own.
#include <JuceHeader.h>
#include "PluginProcessor.h"   // ChannelType, channelTypeNames

namespace echojay::sourcetype
{

/** The COARSE kind of a channel type - "channel" | "bus" | "mix_bus" | "master" - which is what
    channelRole has always carried and what Sean's post-add-match ruling of 20 Oct keys on ("no
    post-add level match on a mix bus or master channel").

    Here rather than file-static in EJAgentExecutorRead.cpp, where the only copy lived: the editor
    needs the same answer for that ruling, and two spellings of "is this the master?" is the kind of
    duplicate this week has been spent undoing. */
inline juce::String kindTextForChannelType (ChannelType t)
{
    if (t == ChannelType::FullMix)   return "mix_bus";
    if (t == ChannelType::MasterBus) return "master";
    if (EchoJayProcessor::isBusRole (t)) return "bus";
    return "channel";
}

/** Where a source type came from. The ORDER IS THE PRECEDENCE - a lower one never overwrites a
    higher one - so the enum's own values rank, and a comparison is the whole rule. */
enum class From { None = 0, Name = 1, Classifier = 2, User = 3 };

inline juce::String wireNameForFrom (From f)
{
    switch (f)
    {
        case From::User:       return "user";
        case From::Classifier: return "classifier";
        case From::Name:       return "name";
        case From::None:       break;
    }
    return {};
}

inline From fromWireName (const juce::String& s)
{
    const auto t = s.trim().toLowerCase();
    if (t == "user")       return From::User;
    if (t == "classifier") return From::Classifier;
    if (t == "name")       return From::Name;
    return From::None;
}

/** The wire spelling of a ChannelType, DERIVED from its own display name rather than kept in a
    second table: lower-cased, "/" dropped, spaces and hyphens to underscores. "Bass / 808" ->
    "bass_808", "Hi-Hat" -> "hi_hat", "Mix Bus" -> "mix_bus". One derivation means the two cannot
    drift the first time a type is renamed. */
inline juce::String wireNameFor (ChannelType t)
{
    const int i = (int) t;
    if (i < 0 || i >= channelTypeNames.size()) return {};
    auto s = channelTypeNames[i].toLowerCase();
    s = s.replace ("/", " ").replace ("-", " ");
    juce::StringArray parts;
    parts.addTokens (s, " ", {});
    parts.removeEmptyStrings();
    return parts.joinIntoString ("_");
}

/** ...and back, for restoring a persisted value or reading one off the wire. Returns false when the
    name is not one of ours, which a reader must treat as "not stated" and never as a default. */
inline bool channelTypeForWireName (const juce::String& wire, ChannelType& out)
{
    const auto want = wire.trim().toLowerCase();
    if (want.isEmpty()) return false;
    for (int i = 0; i < channelTypeNames.size(); ++i)
        if (wireNameFor ((ChannelType) i) == want) { out = (ChannelType) i; return true; }
    return false;
}

/** The family a type belongs to. Used ONLY for the conflict test below: two matches inside one
    family are still a disagreement about the subtype, but two matches across families is a name
    that is describing two different things and must not be guessed at. */
enum class Family { Unknown, Vocal, Drums, Bass, KeysGuitar, Synth, StringsBrass, Fx, Bus };

inline Family familyOf (ChannelType t)
{
    switch (t)
    {
        case ChannelType::LeadVocal: case ChannelType::BackingVocal:
        case ChannelType::Adlibs:    case ChannelType::VocalBus:        return Family::Vocal;
        case ChannelType::Kick:      case ChannelType::Snare:
        case ChannelType::HiHat:     case ChannelType::Overheads:
        case ChannelType::DrumBus:   case ChannelType::Percussion:      return Family::Drums;
        case ChannelType::Bass808:   case ChannelType::BassGuitar:
        case ChannelType::SubBass:   case ChannelType::SynthBass:       return Family::Bass;
        case ChannelType::Piano:     case ChannelType::Keys:
        case ChannelType::AcousticGuitar: case ChannelType::ElectricGuitar:
        case ChannelType::GuitarBus:                                    return Family::KeysGuitar;
        case ChannelType::SynthLead: case ChannelType::SynthPad:
        case ChannelType::SynthPluck: case ChannelType::SynthBus:       return Family::Synth;
        case ChannelType::Strings:   case ChannelType::Brass:
        case ChannelType::Woodwind:  case ChannelType::Orchestral:      return Family::StringsBrass;
        case ChannelType::FX:        case ChannelType::Reverb:
        case ChannelType::Delay:     case ChannelType::Foley:
        case ChannelType::Ambient:                                      return Family::Fx;
        case ChannelType::FullMix:   case ChannelType::MasterBus:
        case ChannelType::InstrumentBus: case ChannelType::MusicBus:    return Family::Bus;
        case ChannelType::Other:     break;
    }
    return Family::Unknown;
}

/** What the guesser decided, and WHY - because a declined guess has to be explainable. `have` false
    with a reason is the normal, correct outcome for a name that does not say. */
struct Guess
{
    bool        have = false;
    ChannelType type = ChannelType::Other;
    From        from = From::None;
    juce::String why;
};

namespace detail
{
    /** A phrase (one or more whole words) and the type it names. ORDER MATTERS ONLY BY LENGTH: the
        table is searched longest-phrase-first, so "bass guitar" is a BassGuitar and never a
        collision between "bass" and "guitar", and "drum bus" is a DrumBus rather than a
        disagreement. That tiering is what lets the conflict test below stay strict. */
    struct Pat { const char* phrase; ChannelType type; };

    inline const std::vector<Pat>& patterns()
    {
        static const std::vector<Pat> p =
        {
            // ---- two words and more, matched first -------------------------------------------
            { "lead vocal",      ChannelType::LeadVocal },
            { "lead vox",        ChannelType::LeadVocal },
            { "backing vocal",   ChannelType::BackingVocal },
            { "backing vox",     ChannelType::BackingVocal },
            { "back vox",        ChannelType::BackingVocal },
            { "vocal bus",       ChannelType::VocalBus },
            { "vox bus",         ChannelType::VocalBus },
            { "ad libs",         ChannelType::Adlibs },
            { "hi hat",          ChannelType::HiHat },
            { "drum bus",        ChannelType::DrumBus },
            { "drums bus",       ChannelType::DrumBus },
            { "bass guitar",     ChannelType::BassGuitar },
            { "sub bass",        ChannelType::SubBass },
            { "synth bass",      ChannelType::SynthBass },
            { "synth lead",      ChannelType::SynthLead },
            { "synth pad",       ChannelType::SynthPad },
            { "synth pluck",     ChannelType::SynthPluck },
            { "synth bus",       ChannelType::SynthBus },
            { "acoustic guitar", ChannelType::AcousticGuitar },
            { "acoustic gtr",    ChannelType::AcousticGuitar },
            { "electric guitar", ChannelType::ElectricGuitar },
            { "electric gtr",    ChannelType::ElectricGuitar },
            { "guitar bus",      ChannelType::GuitarBus },
            { "gtr bus",         ChannelType::GuitarBus },
            { "instrument bus",  ChannelType::InstrumentBus },
            { "music bus",       ChannelType::MusicBus },
            { "master bus",      ChannelType::MasterBus },
            { "stereo out",      ChannelType::MasterBus },
            { "mix bus",         ChannelType::FullMix },
            // ---- single whole words ------------------------------------------------------------
            { "vox",             ChannelType::LeadVocal },
            { "vocal",           ChannelType::LeadVocal },
            { "vocals",          ChannelType::LeadVocal },
            { "adlib",           ChannelType::Adlibs },
            { "adlibs",          ChannelType::Adlibs },
            { "kick",            ChannelType::Kick },
            { "bd",              ChannelType::Kick },
            { "snare",           ChannelType::Snare },
            { "sd",              ChannelType::Snare },
            { "hat",             ChannelType::HiHat },
            { "hats",            ChannelType::HiHat },
            { "hihat",           ChannelType::HiHat },
            { "overhead",        ChannelType::Overheads },
            { "overheads",       ChannelType::Overheads },
            { "ohs",             ChannelType::Overheads },
            // "drums" alone is taken as the DRUM BUS - a track called "Drums" in a session with
            // separate kick and snare tracks is the bus they feed, and that is the reading that has
            // never surprised anyone. "drum" singular likewise.
            { "drum",            ChannelType::DrumBus },
            { "drums",           ChannelType::DrumBus },
            { "perc",            ChannelType::Percussion },
            { "percussion",      ChannelType::Percussion },
            // "Bass / 808" is the list's own GENERIC bass - its label says so - so a bare "bass" or
            // "808" lands there rather than declining over a subtype nobody stated.
            { "bass",            ChannelType::Bass808 },
            { "808",             ChannelType::Bass808 },
            // NOTE: no bare "sub". It is in the noise list below (every session has a "Sub" group),
            // so a single-word entry here would be dead code pretending to work. "Sub Bass" is
            // caught by the phrase above, and "808 Sub" by "808".
            { "piano",           ChannelType::Piano },
            { "keys",            ChannelType::Keys },
            { "strings",         ChannelType::Strings },
            { "brass",           ChannelType::Brass },
            { "horns",           ChannelType::Brass },
            { "sax",             ChannelType::Woodwind },
            { "flute",           ChannelType::Woodwind },
            { "clarinet",        ChannelType::Woodwind },
            { "orchestra",       ChannelType::Orchestral },
            { "orchestral",      ChannelType::Orchestral },
            { "fx",              ChannelType::FX },
            { "reverb",          ChannelType::Reverb },
            { "verb",            ChannelType::Reverb },
            { "delay",           ChannelType::Delay },
            { "foley",           ChannelType::Foley },
            { "ambient",         ChannelType::Ambient },
            { "master",          ChannelType::MasterBus },
            { "mix",             ChannelType::FullMix },
        };
        return p;
    }

    /** Words that are COMMON ENOUGH TO MEAN NOTHING, and are therefore not evidence. "main" is the
        one from his own session - "Nafe Main_01" could be a lead vocal or a bus, so it declines, and
        that is the correct answer rather than a coin toss. The rest are the words every session uses
        for structure. A declining word does not veto a real match beside it: "Vocal Main" is still a
        vocal; it only means the word on its own is not a guess. */
    inline bool meansNothing (const juce::String& w)
    {
        static const juce::StringArray noise
        {
            "main", "mains", "track", "audio", "inst", "instrument", "stem", "stems", "bus",
            "buss", "group", "sub", "aux", "send", "return", "print", "comp", "edit", "final",
            "new", "copy", "dup", "take", "alt", "auto", "rough", "ref", "temp", "test", "pre",
            "post", "left", "right", "mid", "side", "wet", "dry", "in", "out", "a", "b", "the"
        };
        return noise.contains (w, /*ignoreCase*/ true);
    }

    /** Lower-cased whole words, numbers dropped. The host's numbering ("_1_01", "88_7even") needs no
        rules of its own: a token that is all digits is not a word, and "7even" simply matches
        nothing. */
    inline juce::StringArray wordsOf (const juce::String& name)
    {
        auto s = name.toLowerCase();
        juce::String cleaned;
        for (auto c : s)
            cleaned << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c)
                                                                      : juce::String (" "));
        juce::StringArray w;
        w.addTokens (cleaned, " ", {});
        w.removeEmptyStrings();
        // ---- A NUMBER IS NOT A WORD, UNLESS IT IS ONE (leg ST8c, 20 Oct 2026) ----------------
        //
        // Dropping all-digit tokens is what makes the host's numbering free: "_1_01", "88_7even",
        // the trailing "2" on a duplicate. But "808" is a SOURCE in this product's own vocabulary -
        // the generic bass type's label is literally "Bass / 808" - so a blanket drop made the
        // pattern entry for it unreachable. Dead code pretending to work, which is the exact trap I
        // wrote a note about for "sub" one line above the table and then walked into here.
        //
        // Caught by leg ST8c asserting that "Kick 808" DECLINES: it came back "kick", because the
        // 808 had been thrown away before the matcher ever saw it. The leg was right - a track that
        // names a kick AND an 808 is naming two things, and a wrong type is worse than none.
        //
        // So a numeric token survives when it is itself a known pattern word, and nothing else
        // changes: "88", "2" and "01" are still not words.
        for (int i = w.size(); --i >= 0;)
        {
            if (! w[i].containsOnly ("0123456789")) continue;
            bool isAWord = false;
            for (const auto& p : patterns())
                if (! juce::String (p.phrase).containsChar (' ') && juce::String (p.phrase) == w[i])
                { isAWord = true; break; }
            if (! isAWord) w.remove (i);
        }
        return w;
    }
}

/** THE GUESS, from a track name alone. Declines unless exactly one type is named at the most
    specific tier that matched. Pure and static so a guard drives the shipped function against his
    own session's track names. */
inline Guess guessFromName (const juce::String& trackName)
{
    Guess g;
    const auto words = detail::wordsOf (trackName);
    if (words.isEmpty()) { g.why = "the name has no words in it"; return g; }
    const juce::String joined = words.joinIntoString (" ");

    // ---- TIER 1: PHRASES, longest first ------------------------------------------------------
    // A phrase match is more specific than any single word inside it, so it ends the search: "bass
    // guitar" must not become a disagreement between "bass" and "guitar", and "drum bus" must not
    // become one between "drum" and the noise word "bus".
    //
    // Matched on the WORD-JOINED string with spaces either side, so it is still whole words: "subbass"
    // does not match "sub bass", and "crossbass guitar" does not match "bass guitar".
    {
        std::vector<ChannelType> hits;
        size_t bestWords = 0;
        for (const auto& p : detail::patterns())
        {
            const juce::String phrase (p.phrase);
            if (! phrase.containsChar (' ')) continue;             // tier 1 is phrases only
            if (! (" " + joined + " ").contains (" " + phrase + " ")) continue;
            const size_t n = (size_t) juce::StringArray::fromTokens (phrase, " ", {}).size();
            if (n > bestWords) { bestWords = n; hits.clear(); }
            if (n == bestWords) hits.push_back (p.type);
        }
        if (! hits.empty())
        {
            bool oneType = true;
            for (const auto& t : hits) if (t != hits.front()) oneType = false;
            if (oneType)
            {
                g.have = true; g.type = hits.front(); g.from = From::Name;
                g.why = "the name says \"" + joined + "\"";
                return g;
            }
            g.why = "the name matches more than one thing at once (\"" + joined + "\"), so it is not "
                    "guessed - a wrong type is worse than none";
            return g;
        }
    }

    // ---- TIER 2: SINGLE WHOLE WORDS ----------------------------------------------------------
    std::vector<ChannelType> hits;
    juce::StringArray hitWords;
    for (const auto& w : words)
    {
        if (detail::meansNothing (w)) continue;
        for (const auto& p : detail::patterns())
        {
            const juce::String phrase (p.phrase);
            if (phrase.containsChar (' ')) continue;
            if (phrase != w) continue;                              // WHOLE WORD, never a substring
            hits.push_back (p.type);
            hitWords.addIfNotAlreadyThere (w);
            break;
        }
    }
    if (hits.empty())
    {
        g.why = "nothing in \"" + joined + "\" names a source";
        return g;
    }
    // ONE DISTINCT TYPE, or nothing. Two subtypes inside one family ("Kick Snare") is a track
    // describing two things, and two families ("Vocal Drums") plainly so - both decline, because the
    // guesser's job is to be right or silent.
    bool oneType = true;
    for (const auto& t : hits) if (t != hits.front()) oneType = false;
    if (! oneType)
    {
        juce::StringArray named;
        for (const auto& t : hits) named.addIfNotAlreadyThere (wireNameFor (t));
        g.why = "\"" + joined + "\" names " + juce::String (named.size()) + " different sources ("
              + named.joinIntoString (", ") + "), so it is not guessed";
        return g;
    }
    g.have = true; g.type = hits.front(); g.from = From::Name;
    g.why = "the name says \"" + hitWords.joinIntoString (", ") + "\"";
    return g;
}

/** THE PRECEDENCE, as one function so it cannot be re-stated differently anywhere.
    True when `incoming` may replace what is already held. A guess never overwrites a decision; the
    SAME provenance may refresh itself (B re-answering, or a rename re-guessing). */
inline bool mayReplace (From held, From incoming)
{
    return (int) incoming >= (int) held;
}

}   // namespace echojay::sourcetype
