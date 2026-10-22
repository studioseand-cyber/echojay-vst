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
#include "EchoJayParamApply.h"   // readSettingsShape: the ONE reading of a settings payload

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
/** ---- WHAT HAPPENED, IN ONE WORD (18 Oct 2026, Sean in Logic) ------------------------------
    *"The step detail under an add reads like a developer log: 'the rack took it and now holds 1
    slot(s); read back 1 slot(s) on the rack; read back Tube-Tech CL 1B: ...'. Make it a plain
    sentence, e.g. 'Added. Read back: threshold -12, ratio 4:1, attack 10, gain 2. Release couldn't
    be read.'"*

    A CLOSED LIST, and "Done" for anything not on it: a tense invented from an op name ("bypassed",
    "set_rack_outed") is the kind of sentence this product keeps having to apologise for. The model
    still gets the structural line it needs - the row and the model are two audiences, and this is
    only ever the row's half. */
inline juce::String opDoneWord (const juce::String& op)
{
    const auto o = op.trim().toLowerCase();
    if (o == "add")      return "Added";
    if (o == "remove")   return "Removed";
    if (o == "set")      return "Set";
    if (o == "build")    return "Built";
    if (o == "move")     return "Moved";
    if (o == "bypass")   return "Bypassed";
    if (o == "unbypass") return "Switched back on";
    if (o == "rename")   return "Renamed";
    if (o == "replace")  return "Replaced";
    return "Done";
}

/** The row's sentence for a `do` that landed: what happened, what was read back, and what could
    not be read - in that order, each its own sentence, and no ids. `figures` and `unreadable` are
    already in plain words (plainParamList / the executor's *Plain lists). */
inline juce::String landedRowSentence (const juce::String& op, const juce::String& what,
                                       const juce::String& figures, const juce::String& unreadable,
                                       int slotLanded = 0)
{
    juce::String s = opDoneWord (op);
    if (what.trim().isNotEmpty()) s << " " << what.trim();
    // WHERE IT LANDED (his 11:27 item 2, 21 Oct 2026). The position is part of what the read-back
    // READ, so it belongs in the same sentence as the figures it read at that position - a slot
    // number in a separate line is a number the reader has to trust; here it is attached to the
    // name it was read with. Zero means the rack did not say, and then the sentence says nothing.
    if (slotLanded >= 1) s << " in slot " << slotLanded;
    s << ".";
    if (figures.trim().isNotEmpty()) s << " Read back: " << figures.trim() << ".";
    if (unreadable.trim().isNotEmpty())
    {
        auto u = unreadable.trim();
        s << " " << u.substring (0, 1).toUpperCase() << u.substring (1) << " couldn't be read.";
    }
    return s;
}

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

/** ---- A SLOT'S SETTINGS, IN WORDS, FROM THE PAYLOAD (20 Oct 2026, B's note) ------------------
    B: *"the agent-off request's current chain now lists slots with no settings ... so B can't
    compute what a compressor is doing. Send each slot's current control values again."*

    The agent's add op carries `settings_structured` (the dial payload) and used to carry no display
    string at all, so the Link had nothing to publish on its sidecar and the chain feed printed a
    bare slot. This composes that string from the values being asked for.

    ONE READING OF THE PAYLOAD, readSettingsShape's - the same one countRequestedSettings and the
    edit card's line use, so a wrapper is a container and never a control. A WRAPPER PRINTED AS A
    CONTROL is the "params Object 0x66cf3ee0" defect, twice over (the proposal card on 24 Aug, and
    controls_norm on his 18:13 card), which is why this does not walk the object itself.

    Values go through plainValuePhrase, so the words here are the same words every other surface
    uses for the same control. A non-numeric value (a mode name) is printed as given; an object or
    an array is skipped entirely, because a var's toString() on one is a pointer.                   */
/** ---- ONE PHRASE PER CONTROL, NOT PER CHANNEL (Sean's 19:5x item 2, 20 Oct 2026) -------------

    *"The add step's read-back dumps every parameter raw: "optical threshold 1 1, optical gain 1 2,
    discrete threshold 1 1, discrete ratio 1 2 ..." for the Shadow Hills. Use the plain settings
    summary: the few controls that matter, in plain words, once per control and not per channel."*

    The phrases themselves were already plain - plainValuePhrase produced every one of them. What
    made that unreadable is that the Shadow Hills exposes its controls PER CHANNEL ("Optical
    Threshold 1", "Optical Threshold 2", ...), the server dialled them all, and the read-back
    faithfully confirmed each one. Every phrase was true; the list was useless.

    So: a trailing channel index is stripped from the control's name, the first phrase for each
    distinct control is kept, and the rest are dropped - the second channel of a stereo pair says
    nothing the first did not. Then the list is CAPPED, with the remainder counted rather than
    truncated, because "and 14 more" is information and a sentence that stops mid-list is not.

    Pure and static so a guard can drive it on his own Shadow Hills strings. */
inline juce::String plainReadBackList (const juce::StringArray& phrases, int maxShown = 6)
{
    // ---- SPLIT EACH PHRASE INTO name / channel / value (his 21:03 item 1, 20 Oct 2026) --------
    //
    // *"The phrase keeps the channel number: "discrete threshold 1 10". When channels collapse, drop
    // the channel number from the phrase: "discrete threshold 10". If the channels differ, say so
    // ("discrete threshold 10 left, 8 right") rather than hiding one."*
    //
    // A phrase is "<name> <value>", and a per-channel control's NAME itself ends in an index - so
    // "discrete threshold 1 10" is name "discrete threshold", channel 1, value 10. Popping the
    // trailing number-ish tokens gives both: the LAST is the value, and the one before it (when
    // there is one) is the channel. A control whose name legitimately contains a number keeps it,
    // because the pop stops at the first non-numeric token ("band 1 gain 3" -> name "band 1 gain").
    //
    // ---- AND A SIDE CAN BE A WORD, NOT A NUMBER (his 11:27 item 1, 21 Oct 2026) ---------------
    //
    // *"The read-back collapse doesn't catch L/R-named channels ("left reduction 2.7", "l threshold
    // 4.5"). Treat L/R/left/right like trailing channel numbers, and say if the sides differ."*
    //
    // Both of his examples put the side FIRST, which is why the number-popping above never saw it:
    // it only ever looked at the tail. So a side token is taken from EITHER end of the name - a
    // plugin writes "left reduction" or "reduction L" with equal cheer - and once stripped it is a
    // channel exactly like "1"/"2", so every rule below applies to it unchanged: one side keeps its
    // label, two that agree collapse to one phrase, two that differ are BOTH said.
    //
    // A side is only ever a WHOLE token (so "low" and "release" are untouched), and it is never
    // taken when it is all the name there is: a control genuinely called "L" would otherwise
    // collapse into a nameless value, which is worse than the repetition it would save.
    struct Part { juce::String name, chan, value; };
    const auto sideWord = [] (const juce::String& tok) -> juce::String
    {
        const auto t = tok.trim().toLowerCase().removeCharacters ("()[]:.,");
        if (t == "l" || t == "left")  return "left";
        if (t == "r" || t == "right") return "right";
        return {};
    };
    const auto isSide = [] (const juce::String& chan)
    { return chan == "left" || chan == "right"; };
    const auto split = [&sideWord] (const juce::String& phrase)
    {
        Part p;
        juce::StringArray tok;
        tok.addTokens (phrase.trim(), " ", {});
        tok.removeEmptyStrings();
        juce::StringArray nums;
        while (tok.size() > 1 && tok[tok.size() - 1].containsOnly ("0123456789.-+"))
        { nums.insert (0, tok[tok.size() - 1]); tok.remove (tok.size() - 1); }
        if (nums.size() >= 1) p.value = nums[nums.size() - 1];
        if (nums.size() >= 2) p.chan  = nums[nums.size() - 2];
        // The side, from either end, and only when a name survives taking it.
        if (p.chan.isEmpty() && tok.size() > 1)
        {
            if (const auto s = sideWord (tok[0]); s.isNotEmpty())
            { p.chan = s; tok.remove (0); }
            else if (const auto e = sideWord (tok[tok.size() - 1]); e.isNotEmpty())
            { p.chan = e; tok.remove (tok.size() - 1); }
        }
        p.name = tok.joinIntoString (" ");
        return p;
    };

    // Grouped BY NAME, in the order they arrived, keeping every channel's value so they can be
    // compared rather than one being hidden - which is the half of his ruling that matters most.
    juce::StringArray names;
    std::vector<juce::StringArray> chans, values;
    juce::StringArray loose;                 // phrases with nothing to key on: kept verbatim
    for (const auto& raw : phrases)
    {
        const auto t = raw.trim();
        if (t.isEmpty()) continue;
        const auto p = split (t);
        if (p.name.isEmpty() || p.value.isEmpty()) { loose.add (t); continue; }
        const int at = names.indexOf (p.name);
        if (at < 0) { names.add (p.name); chans.push_back ({ p.chan }); values.push_back ({ p.value }); }
        else { chans[(size_t) at].add (p.chan); values[(size_t) at].add (p.value); }
    }

    juce::StringArray kept;
    for (int i = 0; i < names.size(); ++i)
    {
        const auto& vs = values[(size_t) i];
        const auto& cs = chans[(size_t) i];
        bool allSame = true;
        for (const auto& v : vs) if (v != vs[0]) allSame = false;
        if (vs.size() == 1)
        {
            // ONE channel only. The number STAYS: a single channel's value is not the control's
            // value, and dropping it would claim more than was written - which is exactly the
            // question he asked of the log ("confirm both channels were written").
            kept.add (cs[0].isEmpty()   ? names[i] + " " + vs[0]
                      : isSide (cs[0])  ? names[i] + " " + vs[0] + " on the " + cs[0]
                                        : names[i] + " " + cs[0] + " " + vs[0]);
        }
        else if (allSame)
        {
            // COLLAPSED, and they agree: one phrase, no channel number - his words exactly.
            kept.add (names[i] + " " + vs[0]);
        }
        else if (vs.size() == 2)
        {
            // THEY DIFFER: say both. Hiding one is the thing his ruling forbids. When the sides
            // NAMED THEMSELVES, use their own words rather than the arrival order - a plugin that
            // reads back right before left would otherwise have its two sides swapped in the
            // sentence, which is a confident lie about which channel holds which value.
            const bool named = isSide (cs[0]) && isSide (cs[1]) && cs[0] != cs[1];
            kept.add (named ? names[i] + " " + vs[0] + " " + cs[0] + ", " + vs[1] + " " + cs[1]
                            : names[i] + " " + vs[0] + " left, " + vs[1] + " right");
        }
        else
        {
            // More than two and not all equal: name each by its channel rather than guess at
            // left/right, which only means something for a pair.
            juce::StringArray each;
            for (int k = 0; k < vs.size(); ++k)
                each.add (cs[k].isEmpty()  ? vs[k]
                          : isSide (cs[k]) ? vs[k] + " " + cs[k]
                                           : vs[k] + " on " + cs[k]);
            kept.add (names[i] + " " + each.joinIntoString (", "));
        }
    }
    kept.addArray (loose);
    if (kept.isEmpty()) return {};
    if (maxShown <= 0 || kept.size() <= maxShown) return kept.joinIntoString (", ");
    juce::StringArray head;
    for (int i = 0; i < maxShown; ++i) head.add (kept[i]);
    return head.joinIntoString (", ") + ", and " + juce::String (kept.size() - maxShown) + " more";
}

inline juce::String plainSettingsSummary (const juce::var& settingsStructured)
{
    const auto sh = echojay::readSettingsShape (settingsStructured);
    juce::StringArray parts;
    for (const auto& leaf : sh.leaves)
    {
        if (leaf.name == "dropped_controls") continue;
        if (leaf.value.isObject() || leaf.value.isArray()) continue;      // never a pointer
        if (leaf.value.isDouble() || leaf.value.isInt() || leaf.value.isInt64())
            parts.add (plainValuePhrase (leaf.name, (double) leaf.value));
        else if (leaf.value.isBool())
            parts.add (semanticLabel (leaf.name) + ((bool) leaf.value ? " on" : " off"));
        else if (const auto t = leaf.value.toString().trim(); t.isNotEmpty())
            parts.add (semanticLabel (leaf.name) + " " + t);
    }
    // EQ bands are counted, not listed: the LIVE bands are the read-back (ChainHost::
    // slotLiveBandText, his 17:09), and a list composed from the request here would be the
    // "confirmed against an echo of itself" fault this build has been removing all week.
    if (auto* o = settingsStructured.getDynamicObject())
        if (auto* ba = o->getProperty ("eq_bands").getArray(); ba != nullptr && ! ba->isEmpty())
            parts.add (juce::String (ba->size()) + (ba->size() == 1 ? " EQ band" : " EQ bands"));
    return parts.joinIntoString (", ");
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
