#pragma once

// =============================================================================
//  ROUND 6a's PINS: BAND NAMES FROM ONE TABLE (kn).
// =============================================================================
//
// kn PIN1   the table: B1's ids in order, the six names, the labels;
// kn PIN2   every C4 sentence, for every band, names it from the table, and the
//           verb agrees with it;
// kn PIN3   the report (its lines, its sentences and its payload) carries no
//           raw band id;
// kn PIN4   the context.match block: no raw id outside C3's own "band" fields,
//           over the synthetic pair and every staging fixture rebuilt;
// kn PIN5   the proposal's capped-gap note names the band from the table.
//
// A RAW ID is one of B1's identifiers where a name belongs: "lowMid", "highMid"
// (any case, camelCase included), "low" or "mid" as a word of their own (any
// case), and "sub" or "air" in lower case without "the" before them. "the sub"
// and "the air" ARE the table's names, and "Sub" and "Air" its labels.
// =============================================================================

#include "EJMatchAttribution.h"
#include "EJMatchReport.h"
#include "EJMatchBlock.h"
#include "k5d_pins.h"   // blockInFromFixture, rerouteFromShares, fixtureDir

namespace k6apins
{
using namespace echojay;

/** Every raw band id in s, as found. */
inline juce::StringArray rawBandIds (const juce::String& s)
{
    juce::StringArray out, words;
    juce::String w;
    auto flush = [&] { if (w.isNotEmpty()) words.add (w); w.clear(); };
    for (auto p = s.getCharPointer(); ! p.isEmpty(); ++p)
    {
        const auto c = *p;
        if (juce::CharacterFunctions::isLetter (c) || c == '-') w += juce::String::charToString (c);
        else flush();
    }
    flush();
    for (int i = 0; i < words.size(); ++i)
    {
        const auto& t = words[i];
        const auto lc = t.toLowerCase();
        const bool article = i > 0 && words[i - 1].equalsIgnoreCase ("the");
        if (lc == "lowmid" || lc == "highmid" || lc == "low" || lc == "mid") out.add (t);
        else if ((t == "sub" || t == "air") && ! article) out.add (t);
    }
    return out;
}

/** Every string value under v, with its path; C3's "band" identifiers are
    skipped when skipBandKey is set (they are ids on the wire by contract). */
inline void stringsOf (const juce::var& v, const juce::String& path, bool skipBandKey,
                       juce::StringArray& out)
{
    if (v.isString()) { out.add (path + "=" + v.toString()); return; }
    if (auto* a = v.getArray())
    {
        for (int i = 0; i < a->size(); ++i)
        {
            const juce::var e = a->getReference (i);
            stringsOf (e, path + "[" + juce::String (i) + "]", skipBandKey, out);
        }
        return;
    }
    if (auto* o = v.getDynamicObject())
        for (const auto& p : o->getProperties())
        {
            if (skipBandKey && p.name.toString() == "band" && p.value.isString()) continue;
            stringsOf (p.value, path + "." + p.name.toString(), skipBandKey, out);
        }
}

/** The strings among `all` that carry a raw id, for the row's detail. */
inline juce::StringArray withRawIds (const juce::StringArray& all)
{
    juce::StringArray bad;
    for (const auto& s : all) if (! rawBandIds (s).isEmpty()) bad.add (s);
    return bad;
}

template <typename Check>
inline void runK6aPins (Check&& check)
{
    // kn PIN1 -- THE TABLE.
    {
        const char* names[6]  = { "the sub", "the lows", "the low-mids", "the mids", "the high-mids", "the air" };
        const char* labels[6] = { "Sub", "Lows", "Low-mids", "Mids", "High-mids", "Air" };
        const bool plural[6]  = { false, true, true, true, true, false };
        bool ok = true;
        juce::String got;
        for (int b = 0; b < 6; ++b)
        {
            ok = ok && juce::String (matchBandEntry (b).id) == macroBandName (b)
                    && matchBandName (b) == names[b] && matchBandLabel (b) == labels[b]
                    && matchBandEntry (b).plural == plural[b] && matchReportBandLabel (b) == labels[b];
            got << matchBandEntry (b).id << "=" << matchBandName (b) << "/" << matchBandLabel (b) << " ";
        }
        ok = ok && matchBandName (6).isEmpty() && matchBandName (-1).isEmpty() && matchBandLabel (6).isEmpty();
        check (ok, "kn PIN1: one table maps B1's six ids, in order, to \"the sub\", \"the lows\", \"the low-mids\", "
                   "\"the mids\", \"the high-mids\", \"the air\", and the report's labels are the table's", got);
    }

    // kn PIN2 -- EVERY C4 SENTENCE, EVERY BAND: the table's name, no raw id,
    // and a verb that agrees.
    {
        juce::StringArray all, missing;
        for (int b = 0; b < 6; ++b)
        {
            const juce::StringArray s {
                attribLowCoverageSentence (b), attribUnknownSentence ("Bass 2", b),
                attribSingleSentence ("Kick", 0.5, b), attribPairSentence ("Gtr L", "Gtr R", 0.75, b),
                attribSpreadSentence (b), attribInfeasibleSentence ("Kick", false, 0.5, b, 3.0f, 3.5f),
                attribCoverageSentence (0.8, b) };
            for (const auto& x : s)
            {
                all.add (x);
                if (! x.containsIgnoreCase (matchBandName (b))) missing.add (x);
            }
        }
        const auto bad = withRawIds (all);
        const bool agree = attribSpreadSentence (2) == "The low-mids are spread across several tracks."
                        && attribSpreadSentence (5) == "The air is spread across several tracks."
                        && attribLowCoverageSentence (1) == "Most of the lows come from tracks without an EchoJay Link."
                        && attribLowCoverageSentence (0) == "Most of the sub comes from tracks without an EchoJay Link."
                        && attribPairSentence ("Gtr L", "Gtr R", 0.75, 2)
                               == "Gtr L and Gtr R hold 75% of the low-mids between them.";
        check (bad.isEmpty() && missing.isEmpty() && agree && all.size() == 42,
               "kn PIN2: every C4 sentence names its band from the table (\"the low-mids\"), never B1's id, and the "
               "verb agrees with it (\"the low-mids are\", \"the air is\")",
               (bad.isEmpty() ? missing : bad).joinIntoString (" | "));
    }

    // kn PIN3 -- THE REPORT: lines, sentences and payload, every band moving.
    {
        MatchReportIn in;
        in.referenceName = "Song X";
        in.mix.integrated = -11.8f; in.ref.integrated = -9.7f;
        in.hasMatchPlan = true;
        for (int b = 0; b < 6; ++b)
        {
            AttribIn a;
            a.band = b; a.busPower = 1.0; a.busHopCount = 100; a.D = 3.0f;
            auto link = [] (const char* uid, const char* name, double p)
            {
                AttribLinkIn l;
                l.uid = uid; l.name = name; l.postTrimPower = p; l.hopCount = 100; l.frameCoverage = 1.0;
                return l;
            };
            if (b % 2 == 0) a.links = { link ("u1", "Kick", 0.9) };                          // rule 6: one track
            else            a.links = { link ("u1", "Kick", 0.35), link ("u2", "Bass", 0.35) }; // rule 7: a pair
            MatchReportBandIn bi;
            bi.mixMinusRefDb = 3.0f;
            bi.attribution = attributeBandMove (a);
            in.bands[(size_t) b] = bi;
        }
        const auto rep = buildMatchReport (in);
        juce::StringArray all = matchReportAllStrings (rep, matchPlanCardAgentOffer (true, false));
        for (const auto& l : rep.lines) { all.add (l.label); all.add (l.text); all.addArray (l.detail); }
        stringsOf (juce::JSON::parse (matchReportPayload (in, rep)), "payload", false, all);
        int named = 0;
        for (const auto& l : rep.lines)
            for (const auto& d : l.detail)
                if (l.band >= 0 && d.contains (matchBandName (l.band))) { ++named; break; }
        const auto bad = withRawIds (all);
        check (bad.isEmpty() && named == 6,
               "kn PIN3: the report's lines, C4's sentences under them and the match_report payload name every band "
               "from the table, never by B1's id",
               juce::String (named) + " named; " + bad.joinIntoString (" | "));
    }

    // kn PIN4 -- THE BLOCK: no raw id outside C3's "band" fields.
    {
        juce::StringArray all;
        int rebuilt = 0, reasonsOurs = 0;
        const char* files[11] = { "air-shelf.json", "attribution-pair.json", "bus-partial.json", "bus-widened.json",
                                  "clean.json", "mapped-stand-in.json", "push-warning.json", "refusals-only.json",
                                  "track-cant-air.json", "track-near-limit.json", "v1-refused.json" };
        for (const auto* f : files)
        {
            const juce::var fx = juce::JSON::parse (k5dpins::fixtureDir().getChildFile (f).loadFileAsString());
            const juce::var req = fx.getProperty ("request", juce::var());
            auto in = k5dpins::blockInFromFixture (req.getProperty ("match", juce::var()));
            k5dpins::rerouteFromShares (in);
            for (const auto& m : in.moves)
                if (m.attribution && (m.attribution->target == "track" || m.attribution->target == "pair"))
                    ++reasonsOurs;
            const auto built = buildMatchBlock (in);
            if (built.readable) ++rebuilt;
            stringsOf (built.block, f, true, all);
        }
        const auto bad = withRawIds (all);
        check (bad.isEmpty() && rebuilt == 11 && reasonsOurs >= 3,
               "kn PIN4: the context.match block carries no raw band id outside C3's \"band\" fields, over every "
               "staging fixture rebuilt with our own reasons",
               juce::String (rebuilt) + " rebuilt, " + juce::String (reasonsOurs) + " of our reasons; "
                   + bad.joinIntoString (" | "));
    }

    // kn PIN5 -- THE PROPOSAL'S CAPPED-GAP NOTE, on the low-mids: NOT the sub,
    // whose id and name are the same word (round 6a's N05 survived on the sub).
    {
        MatchSide m, r;
        m.macro = { 0.0f, 0.0f, 9.0f, 0.0f, 0.0f, 0.0f };
        m.hasMacro = true; m.macroReduction = SpectralReduction::WholeWindowAverage;
        m.durationSeconds = 60.0f; m.integrated = -14.0f; m.truePeak = -1.0f;
        r.macro.fill (0.0f); r.hasMacro = true; r.macroReduction = SpectralReduction::WholeFileAverage;
        r.durationSeconds = 180.0f; r.integrated = -14.0f; r.truePeak = -1.0f;
        const auto p = computeMatchProposal (m, r);
        bool said = false;
        for (const auto& n : p.notes) said = said || n.startsWith ("The low-mids gap is");
        const auto bad = withRawIds (p.notes);
        check (said && bad.isEmpty(),
               "kn PIN5: the proposal's capped-gap note names the band from the table (\"The low-mids gap is ...\")",
               p.notes.joinIntoString (" | "));
    }
}

} // namespace k6apins
