#pragma once
// =============================================================================
//  EJParamPlainNames — parameter IDs, in words a person reads.
//
//  SEAN, 15 Oct 2026: *"The not-confirmed line shows raw parameter IDs
//  ("band1.freq_hz, band1.gain_db, band1.q"). Show plain names ("the frequency,
//  gain and Q of both bands")."*
//
//  The ids are the WIRE's vocabulary and they are right there - they join the
//  server's map to the plugin's parameters, and the model needs them. What was
//  wrong is that a sentence addressed to a person was built out of them. So both
//  exist from here on: the result object keeps the ids for the model, and the
//  LINE uses these names.
//
//  ITS OWN HEADER FOR ONE REASON: so a guard can drive it. A renderer reachable
//  only by linking the executor is a renderer with no test, which is how the
//  chat's markdown ended up eating Sean's track names.
//
//  TWO RULES, BOTH FROM HIS EXAMPLE:
//    1. a leaf becomes a WORD, not a de-underscored id: freq_hz -> "frequency",
//       not "freq hz". A table, because the unit suffixes (_hz, _db, _ms) are
//       noise to a reader and the stems are not always English either.
//    2. GROUPS COLLAPSE. band1.freq_hz + band2.freq_hz is "the frequency of both
//       bands", not two entries. That is the whole difference between his example
//       and what the line said.
//
//  An id this table does not know is rendered as its stem with the underscores
//  opened out and any unit suffix dropped - never dropped from the sentence,
//  because a parameter that did not land must be named even if awkwardly.
// =============================================================================
#include <JuceHeader.h>

namespace echojay::params
{

/** The leaf of an id - "freq_hz" from "band1.freq_hz" - as a word. */
inline juce::String plainLeaf (const juce::String& leafIn)
{
    const auto leaf = leafIn.trim().toLowerCase();
    // The ones a person actually sees in this product, deliberately short: the sentence reads
    // "the frequency, gain and Q", never "the frequency parameter".
    static const std::pair<const char*, const char*> kWords[] = {
        { "freq_hz",        "frequency" },   { "freq",       "frequency" },
        { "gain_db",        "gain" },        { "gain",       "gain" },
        { "q",              "Q" },
        { "threshold_db",   "threshold" },   { "threshold",  "threshold" },
        { "ratio",          "ratio" },
        { "attack_ms",      "attack" },      { "attack_db",  "attack" },  { "attack", "attack" },
        { "release_ms",     "release" },     { "release",    "release" },
        { "sustain_db",     "sustain" },     { "sustain",    "sustain" },
        { "makeup_db",      "makeup" },      { "makeup",     "makeup" },
        { "knee_db",        "knee" },
        { "mix",            "mix" },         { "wet",        "wet" },
        { "output_db",      "output" },      { "input_db",   "input" },
        { "lookahead_ms",   "lookahead" },
        { "hpf_hz",         "sidechain high-pass" },
        { "lpf_hz",         "low-pass" },
        { "range_db",       "range" },
        { "mode",           "mode" },        { "character",  "character" },
        { "detector",       "detector" },
        { "stereo_link",    "stereo link" },
        { "pre_gain_db",    "pre-gain" },    { "pre_gain",   "pre-gain" },
        { "out_gain_db",    "output gain" },
    };
    for (const auto& w : kWords)
        if (leaf == w.first) return w.second;

    // Unknown: open the underscores and drop a trailing unit, so it still reads as words rather
    // than as an identifier. Never dropped from the sentence - an unnamed failure is worse.
    auto out = leaf;
    for (const char* unit : { "_hz", "_db", "_ms", "_pct", "_s" })
        if (out.endsWith (unit)) { out = out.dropLastCharacters ((int) juce::String (unit).length()); break; }
    return out.replaceCharacter ('_', ' ');
}

/** "a", "a and b", "a, b and c" - no Oxford comma, which is this product's house style. */
inline juce::String joinWords (const juce::StringArray& wordsIn)
{
    juce::StringArray words;
    for (const auto& w : wordsIn) if (w.trim().isNotEmpty()) words.addIfNotAlreadyThere (w.trim());
    if (words.isEmpty()) return {};
    if (words.size() == 1) return words[0];
    const auto last = words[words.size() - 1];
    words.remove (words.size() - 1);
    return words.joinIntoString (", ") + " and " + last;
}

/** How a GROUP prefix reads: "band1" -> "band 1". Numbers are separated from the stem so the
    collapse below can say "both bands" rather than "band1 and band2". */
inline juce::String plainGroup (const juce::String& groupIn, int* numberOut = nullptr)
{
    const auto g = groupIn.trim();
    if (numberOut != nullptr) *numberOut = -1;
    int i = g.length();
    while (i > 0 && juce::CharacterFunctions::isDigit (g[i - 1])) --i;
    if (i == g.length()) return g.replaceCharacter ('_', ' ');          // no trailing number
    const auto stem = g.substring (0, i).replaceCharacter ('_', ' ');
    const auto num  = g.substring (i);
    if (numberOut != nullptr) *numberOut = num.getIntValue();
    return stem + " " + num;
}

/** THE SENTENCE. A list of ids becomes the phrase Sean asked for.

    band1.freq_hz, band1.gain_db, band1.q, band2.freq_hz, band2.gain_db, band2.q
      -> "the frequency, gain and Q of both bands"
    band1.freq_hz, band1.q
      -> "the frequency and Q of band 1"
    threshold_db, ratio, attack_ms
      -> "the threshold, ratio and attack"

    Mixed groups and bare leaves are joined with a semicolon, because two different shapes in one
    comma list is where a sentence like this stops being readable. */
inline juce::String plainParamList (const juce::StringArray& ids)
{
    juce::StringArray bare;                                  // leaves with no group
    juce::StringArray groupOrder;                            // groups, in first-seen order
    std::map<juce::String, juce::StringArray> byGroup;        // group -> its leaves, in order

    for (const auto& idRaw : ids)
    {
        auto id = idRaw.trim();
        if (id.isEmpty()) continue;
        // "slot 1 attack_db" and the like: take the last whitespace-separated token as the id.
        if (id.containsChar (' ')) id = id.fromLastOccurrenceOf (" ", false, false);
        if (id.containsChar ('.'))
        {
            const auto g = id.upToFirstOccurrenceOf (".", false, false);
            const auto leaf = id.fromFirstOccurrenceOf (".", false, false);
            if (! byGroup.count (g)) groupOrder.add (g);
            byGroup[g].addIfNotAlreadyThere (plainLeaf (leaf));
        }
        else
            bare.addIfNotAlreadyThere (plainLeaf (id));
    }

    juce::StringArray pieces;
    if (! bare.isEmpty()) pieces.add ("the " + joinWords (bare));

    // COLLAPSE: groups that asked for the SAME leaves read as one phrase. This is the rule his
    // example turns on - six ids over two bands are one thing said once, not two lists.
    juce::StringArray done;
    for (const auto& g : groupOrder)
    {
        if (done.contains (g)) continue;
        juce::StringArray sameAs { g };
        for (const auto& other : groupOrder)
            if (other != g && ! done.contains (other) && byGroup[other] == byGroup[g])
                sameAs.add (other);
        for (const auto& s : sameAs) done.add (s);

        const auto leaves = "the " + joinWords (byGroup[g]);
        if (sameAs.size() == 1)
            pieces.add (leaves + " of " + plainGroup (g));
        else
        {
            // "both bands" reads better than "all 2 bands"; beyond two, say how many.
            int dummy = -1;
            auto stem = plainGroup (sameAs[0], &dummy);
            stem = stem.upToLastOccurrenceOf (" ", false, false);        // drop the number
            if (stem.isEmpty()) stem = "group";
            pieces.add (leaves + (sameAs.size() == 2 ? " of both " + stem + "s"
                                                     : " of all " + juce::String (sameAs.size())
                                                       + " " + stem + "s"));
        }
    }
    return pieces.joinIntoString ("; ");
}

} // namespace echojay::params
