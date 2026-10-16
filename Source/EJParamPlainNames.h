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

/** ---- SEAN'S #6 (16 Oct 2026): A VALUE, IN WORDS AND ITS OWN UNIT -----------------------------
    *"Step details are raw text ('slot 2 threshold_db -14.00, slot 2 ratio 4.00...'). Plain words:
    'Compressor: threshold -14 dB, ratio 4:1, attack 15 ms...'."*

    The id names the parameter and its suffix names the unit, so both are already here; what was
    missing was anything that put them together for a reader. The number is printed as short as it
    can be without losing anything - 4 not 4.00, 14.5 not 14.50 - because the trailing zeros were
    half of what made the line look like a dump.

    A control NAME (a third party's "Threshold", which carries no suffix) keeps its own name and
    takes its unit from the caller, who has the map entry. */
inline juce::String plainNumber (double v)
{
    auto t = juce::String (v, 2);
    if (t.containsChar ('.')) t = t.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    return t.isEmpty() || t == "-0" ? juce::String ("0") : t;
}

inline juce::String plainValuePhrase (const juce::String& idIn, double value,
                                      const juce::String& unitHint = {})
{
    const auto id   = idIn.trim();
    const auto leaf = id.contains (".") ? id.fromLastOccurrenceOf (".", false, false) : id;
    const auto low  = leaf.toLowerCase();
    // the name: the table's word for a known id, the control's own name otherwise
    juce::String name = plainLeaf (leaf);
    if (id.contains (".")) name = plainGroup (id.upToLastOccurrenceOf (".", false, false)) + " " + name;

    juce::String unit = unitHint.trim().toLowerCase();
    if (unit.isEmpty())
    {
        if (low == "ratio")            unit = "ratio";
        else if (low.endsWith ("_db")) unit = "db";
        else if (low.endsWith ("_ms")) unit = "ms";
        else if (low.endsWith ("_hz")) unit = "hz";
        else if (low.endsWith ("_pct")) unit = "pct";
        else if (low.endsWith ("_s"))  unit = "s";
        else if (low == "q")           unit = "q";
    }
    juce::String figure;
    if (unit == "ratio")      figure = plainNumber (value) + ":1";
    else if (unit == "db")    figure = plainNumber (value) + " dB";
    else if (unit == "ms")    figure = plainNumber (value) + " ms";
    else if (unit == "s")     figure = plainNumber (value) + " s";
    else if (unit == "pct")   figure = plainNumber (value) + "%";
    else if (unit == "hz")    figure = value >= 1000.0 ? plainNumber (value / 1000.0) + " kHz"
                                                       : plainNumber (value) + " Hz";
    else                      figure = plainNumber (value);
    return (name + " " + figure).trim();
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
/** ---- A WHOLE LINE, WITH ITS RAW IDS TURNED INTO WORDS (16 Oct 2026) --------------------------

    The joint live pass drew this step line, straight from the server's own plan summary:

        set slot 1: attack_ms 20, release_ms 60 - on 88_7even Kick_2_02

    Invariant I4 ("no raw parameter IDs") caught it, and the ids are the server's to stop sending -
    but a line the user reads is this build's responsibility whoever composed it, so the ids are
    turned into words here before anything draws them.

    DELIBERATELY CONSERVATIVE, because of the second half of that same line. A track can be called
    "88_7even Kick_2_02" and a plugin "Pro-C 3": underscores and digits are ordinary characters in a
    name, and mangling someone's track name would be a worse fault than printing an id. So a token is
    only rewritten when it is UNMISTAKABLY an id:

      - it is all LOWER-CASE letters and underscores and nothing else ("attack_ms", "threshold_db",
        "stereo_link"): a digit or a capital anywhere in the token means it is a name, or
      - it is a dotted id whose leaf is one of those ("band1.freq_hz" -> "band 1 frequency").

    Everything else is left exactly as it was written.                                              */
inline juce::String plainifyLine (const juce::String& lineIn)
{
    if (lineIn.isEmpty()) return lineIn;
    static const char* kUnits[] = { "_hz", "_db", "_ms", "_pct", "_s" };
    // ONE TEST, AND THE GUARD WROTE IT. My first cut also rewrote anything plainLeaf would change,
    // which turned "88_7even Kick_2_02" into "88 7even kick 2 02" - someone's track name, mangled,
    // which is the worse of the two faults by a long way. An id in this product is LOWER CASE
    // letters and underscores and nothing else: a digit or a capital means it is a name.
    const auto looksLikeBareId = [&] (const juce::String& t)
    {
        if (! t.containsChar ('_')) return false;
        for (int i = 0; i < t.length(); ++i)
        {
            const auto c = t[i];
            if (! ((c >= 'a' && c <= 'z') || c == '_')) return false;
        }
        juce::ignoreUnused (kUnits);
        return true;
    };
    juce::String out;
    int i = 0;
    const int n = lineIn.length();
    while (i < n)
    {
        // A token is a run of characters an id could be made of; everything else is copied through.
        if (! (juce::CharacterFunctions::isLetterOrDigit (lineIn[i]) || lineIn[i] == '_' || lineIn[i] == '.'))
        { out << lineIn[i]; ++i; continue; }
        int j = i;
        while (j < n && (juce::CharacterFunctions::isLetterOrDigit (lineIn[j]) || lineIn[j] == '_' || lineIn[j] == '.'))
            ++j;
        auto tok = lineIn.substring (i, j);
        // A trailing full stop belongs to the sentence, not to the token.
        juce::String trailer;
        while (tok.endsWithChar ('.')) { trailer = "." + trailer; tok = tok.dropLastCharacters (1); }
        juce::String rewritten;
        if (tok.containsChar ('.'))
        {
            const auto g    = tok.upToFirstOccurrenceOf (".", false, false);
            const auto leaf = tok.fromFirstOccurrenceOf (".", false, false);
            if (looksLikeBareId (leaf) || plainLeaf (leaf) != leaf)
            {
                int num = 0;
                const auto gp = plainGroup (g, &num);
                rewritten = (gp.isNotEmpty() ? gp + " " : juce::String()) + plainLeaf (leaf);
            }
        }
        else if (looksLikeBareId (tok))
            rewritten = plainLeaf (tok);
        out << (rewritten.isNotEmpty() ? rewritten : tok) << trailer;
        i = j;
    }
    return out;
}

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
