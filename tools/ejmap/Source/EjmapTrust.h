/*
  EjmapTrust.h - THE TRUST LEVELS' DATA RULES (Kathy, 16 Oct 2026, "SORT ALL THREE"; docs/TRUST_LEVELS_v0.md)

  1. READ-BACK, per control. A read-back says: write the norm the map's anchors give for a value, read the plugin's own display, and the
     display shows that value. Two sources, one judge (judgeObservation):
       - EXISTING DATA (the credit): every display the measuring runs already read at a norm they wrote - the certification's fixtures
         (controls[].displayAt, every control at 3+ norms in one load) and every Phase B record's positions ({norm, display} beside a
         measured result), Sean's cert folders and the wizard's runs alike. Each reading is compared with what THIS map's anchors say at
         that norm, so a reading from another version proves this map only where the two agree.
       - THE SWEEP (the rest): the mapping sweep's one-load read-back (readbackTargets -> write -> read -> judge -> restore), saved on the
         control and sent with the map.
  2. THE ROLE RULE (Kathy's ruling, 16 Oct): a standard role is identified by NAME + UNIT - Threshold dB, Ratio x:1, Attack / Release
     ms, Makeup / Output / Gain dB, EQ Freq Hz / Gain dB / Q, Mix / Dry-Wet %, Delay Time ms or note, Decay / RT s, Pre-delay ms,
     Drive / Saturation %, Ceiling dB. A name that matches with the wrong unit, or two candidates for one role, is NOT identified: it
     still needs measurement or a human check.
  3. THE LEVEL: certified (the category's v1 rule passed: EjmapCertV1.h, or the compressor publish list) > mapped (roles identified by
     the rule or by measurement, every identified role read back, not held) > unsure. Each with its why.
  Everything here is pure; the sweep's in-instance loop and the --trust-levels command call it.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <set>
#include <vector>
#include <cmath>
#include <algorithm>

namespace ejmap::trust
{

//==============================================================================
// THE NUMBERS ON A DISPLAY. "-24.0 dB" -> -24, "2.00 k" / "1.5kHz" -> 2000 / 1500, "4.0:1" -> 4, "120 ms" -> 120, "1.2 s" -> 1.2
// (the unit family carries ms vs s); the U+2212 minus and the narrow no-break space Waves / kHs print are read as '-' and ' '.
inline bool numberOf (const juce::String& display, double& out)
{
    auto t = display.trim().replace (juce::String::charToString ((juce::juce_wchar) 0x2212), "-")
                     .replace (juce::String::charToString ((juce::juce_wchar) 0x202f), " ").replace (juce::String::charToString ((juce::juce_wchar) 0x00a0), " ");
    if (t.startsWithChar ('+')) t = t.substring (1);
    int i = 0; const int n = t.length();
    while (i < n && t[i] == ' ') ++i;
    const int start = i;
    if (i < n && t[i] == '-') ++i;
    bool digits = false;
    while (i < n && (juce::CharacterFunctions::isDigit (t[i]) || t[i] == '.')) { digits = digits || juce::CharacterFunctions::isDigit (t[i]); ++i; }
    if (! digits) return false;
    double v = t.substring (start, i).getDoubleValue();
    auto rest = t.substring (i).trimStart().toLowerCase();
    if (rest.startsWith ("k") && ! rest.startsWith ("kn")) v *= 1000.0;   // "2.00 k", "1.5kHz" (never "knee")
    out = v; return true;
}

//==============================================================================
// 1. THE JUDGE: one reading (a norm written, the display read) against a map control's anchors ([value, norm] pairs, the served
// order). The tolerance is the wizard's near-match rule: one display step (the reading's last printed digit) or 0.5% of the span,
// whichever is larger, capped at 1% of the span.
struct Anchor { double value = 0.0, norm = 0.0; };
inline std::vector<Anchor> anchorsOf (const juce::var& control)
{
    std::vector<Anchor> a;
    if (const auto* arr = control.getProperty ("anchors", {}).getArray())
        for (const auto& p : *arr)
            if (const auto* pr = p.getArray(); pr != nullptr && pr->size() >= 2) a.push_back ({ (double) (*pr)[0], (double) (*pr)[1] });
    std::sort (a.begin(), a.end(), [] (const Anchor& x, const Anchor& y) { return x.norm < y.norm; });
    return a;
}
inline bool valueAtNorm (const std::vector<Anchor>& a, double norm, double& out)
{
    if (a.size() < 2 || norm < a.front().norm - 1e-6 || norm > a.back().norm + 1e-6) return false;
    for (size_t i = 1; i < a.size(); ++i)
        if (norm <= a[i].norm + 1e-9)
        {
            const double span = a[i].norm - a[i - 1].norm;
            const double t = span > 1e-12 ? (norm - a[i - 1].norm) / span : 0.0;
            out = a[i - 1].value + t * (a[i].value - a[i - 1].value); return true;
        }
    out = a.back().value; return true;
}
inline double displayStep (const juce::String& display)
{
    const auto t = display.trim();
    const int dot = t.indexOfChar ('.');
    if (dot < 0) return 1.0;
    int d = 0; for (int i = dot + 1; i < t.length() && juce::CharacterFunctions::isDigit (t[i]); ++i) ++d;
    return std::pow (10.0, -d);
}
struct Verdict { bool judged = false, pass = false; double expected = 0.0, read = 0.0; juce::String why; };
inline Verdict judgeObservation (const std::vector<Anchor>& a, double norm, const juce::String& display)
{
    Verdict v;
    if (a.size() < 2) { v.why = "the map has no anchor table for it"; return v; }
    double lo = a.front().value, hi = a.front().value; for (const auto& p : a) { lo = std::min (lo, p.value); hi = std::max (hi, p.value); }
    if (hi - lo <= 1e-12) { v.why = "the map's anchors do not move"; return v; }
    if (! valueAtNorm (a, norm, v.expected)) { v.why = "norm " + juce::String (norm, 3) + " is outside the map's anchors"; return v; }
    if (! numberOf (display, v.read)) { v.why = "the display '" + display + "' carries no number"; return v; }
    // the map's anchors are raw parsed floats ("1.2k" was recorded as 1.2): read the display the same way when the anchors are small
    double raw = 0.0; if (std::abs (v.read) >= 1000.0 && hi < 1000.0 && numberOf (display.upToFirstOccurrenceOf ("k", false, true), raw)) v.read = raw;
    const double span = hi - lo, tol = std::min (std::max (displayStep (display), 0.005 * span), std::max (0.01 * span, displayStep (display)));
    v.judged = true; v.pass = std::abs (v.read - v.expected) <= tol + 1e-9;
    v.why = "norm " + juce::String (norm, 3) + ": the map says " + juce::String (v.expected, 3) + ", the display read '" + display.trim() + "'"
            + (v.pass ? "" : " (off by " + juce::String (std::abs (v.read - v.expected), 3) + ", tolerance " + juce::String (tol, 3) + ")");
    return v;
}
// a control is read back when at least 2 readings were judged and none failed; one failure is a failed read-back (said, kept)
struct ControlReadback { int judged = 0, failed = 0, superseded = 0; juce::String worst, source; bool proven() const { return judged >= 2 && failed == 0; } };   // superseded: old failures a newer clean read-back set aside
inline void addReading (ControlReadback& r, const Verdict& v, const juce::String& source)
{
    if (! v.judged) return;
    ++r.judged; if (! v.pass) { ++r.failed; r.worst = v.why; }
    if (! r.source.contains (source)) r.source << (r.source.isEmpty() ? "" : ",") << source;
}
// THE SWEEP'S TARGETS: three values at 25 / 50 / 75 % of the anchors' span, each with the norm the anchors give for it
inline std::vector<Anchor> readbackTargets (const std::vector<Anchor>& a)
{
    std::vector<Anchor> out; if (a.size() < 2) return out;
    for (double f : { 0.25, 0.5, 0.75 })
    {
        const double n = a.front().norm + f * (a.back().norm - a.front().norm);
        double v = 0.0; if (valueAtNorm (a, n, v)) out.push_back ({ v, n });
    }
    return out;
}

//==============================================================================
// 2. THE ROLE RULE
struct Role { const char* role; const char* words; const char* units; };   // words: '|'-separated substrings (lower case); units: '|'
inline const std::vector<Role>& roleTable()
{
    // ORDER MATTERS: the first role whose words match decides (makeup / output / ceiling before gain, pre-delay before delay time)
    static const std::vector<Role> t {
        { "predelay",   "pre-delay|predelay|pre delay|pre dly|predly",              "ms|s" },
        { "ceiling",    "ceiling",                                                   "db" },
        { "threshold",  "threshold|thresh|thrsh",                                    "db" },
        { "ratio",      " ratio",                                                    "ratio" },   // a word start: "satu-ratio-n" is not a ratio
        { "attack",     "attack|atk",                                                "ms|s" },
        { "release",    "release|recovery",                                          "ms|s" },
        { "makeup",     "makeup|make-up|make up",                                    "db" },
        { "output",     "output|out gain|out level|master out|master gain",          "db" },
        { "decay",      "decay|rt60|reverb time| rt|rt ",                            "ms|s" },
        { "delay_time", "delay time|time|delay|note|sync",                           "ms|s|note" },   // note values (1/4, 1/8D, 1/16T) count (Kathy, 16 Oct)
        { "freq",       "freq|frequency|frq",                                        "hz" },
        { "q",          "bandwidth| q |^q$",                                         "q" },
        { "mix",        "mix|dry/wet|dry-wet|dry wet|wet",                           "pct" },
        { "drive",      "drive|saturation|saturate",                                 "pct" },
        { "gain",       "gain",                                                      "db" },
    };
    return t;
}
// the unit a control's DISPLAY declared, plus the three the display shape shows: ratio ("x:1" - a numeric 'ratio' control from 1 up),
// Q (a numeric q / bandwidth control inside 0..100) and note (a mode control whose labels are note values: 1/4, 1/8D, 1/16T)
inline juce::String unitOf (const juce::var& c)
{
    const auto name = c.getProperty ("name", "").toString().toLowerCase();
    const auto u = c.getProperty ("unit", "").toString().toLowerCase();
    const bool identity = (bool) c.getProperty ("identity_display", false);
    if (c.getProperty ("kind", "").toString() == "mode")
    {
        // a stepped control's unit is the one most of its labels print: note values (1/4, 1/8D), or "60 Hz" / "1.5k", "+3 dB", "20 ms"
        int notes = 0, all = 0; std::map<juce::String, int> u;
        if (auto* lo = c.getProperty ("labels", {}).getDynamicObject())
            for (const auto& p : lo->getProperties())
            {
                ++all; const auto t = p.name.toString(), l = t.toLowerCase(); double v = 0.0;
                if (t.containsChar ('/') && t.retainCharacters ("0123456789").isNotEmpty()) { ++notes; continue; }
                if (! numberOf (t, v)) continue;
                if (l.contains ("hz") || l.trim().endsWith ("k")) ++u["hz"]; else if (l.contains ("db")) ++u["db"]; else if (l.contains ("ms")) ++u["ms"];
                else if (l.contains ("%")) ++u["pct"]; else if (l.trim().endsWith ("s")) ++u["s"];
            }
        if (all >= 2 && notes * 2 >= all) return "note";
        for (const auto& [k, n] : u) if (all >= 2 && n * 2 >= all) return k;
        return {};
    }
    if (identity) return {};   // the display gave no number of its own: no unit evidence
    if (u == "db" || u == "hz" || u == "ms" || u == "s" || u == "pct") return u;
    const auto a = anchorsOf (c); if (a.size() < 2) return {};
    double lo = a.front().value, hi = a.front().value; for (const auto& p : a) { lo = std::min (lo, p.value); hi = std::max (hi, p.value); }
    if (name.contains ("ratio") && lo >= 1.0 && hi > lo) return "ratio";
    const auto padded = " " + name + " ";
    if ((padded.contains (" q ") || name.contains ("bandwidth")) && lo > 0.0 && hi <= 100.0 && hi > lo) return "q";
    return {};
}
inline bool wordsMatch (const juce::String& lowerName, const char* words)
{
    const auto padded = " " + lowerName + " ";
    for (const auto& w : juce::StringArray::fromTokens (words, "|", ""))
    {
        if (w == "^q$") { if (lowerName == "q") return true; continue; }
        if (padded.contains (w)) return true;
    }
    return false;
}
struct RoleHit { juce::String role; bool unitOk = false; };
inline RoleHit roleOf (const juce::var& c)
{
    const auto name = c.getProperty ("name", "").toString().toLowerCase().replaceCharacters ("_", " ");
    for (const auto& r : roleTable())
        if (wordsMatch (name, r.words))
        {
            const auto u = unitOf (c);
            return { r.role, u.isNotEmpty() && juce::StringArray::fromTokens (r.units, "|", "").contains (u) };
        }
    return {};
}
// the section a control belongs to: its name with the role words and the unit taken out ("Band 2 Gain" -> "band 2"), so an EQ's
// freq / gain / Q pair by band and two thresholds in one section are two candidates for one role
inline juce::String sectionOf (const juce::String& name)
{
    auto s = " " + name.toLowerCase().replaceCharacters ("_-:()[]", "       ") + " ";
    for (const auto& r : roleTable()) for (const auto& w : juce::StringArray::fromTokens (r.words, "|", "")) if (w != "^q$" && w.trim().isNotEmpty()) s = s.replace (w.trim(), " ");
    for (const char* u : { " db ", " hz ", " ms ", " khz " }) s = s.replace (u, " ");
    return juce::StringArray::fromTokens (s, " ", "").joinIntoString (" ").trim();
}
struct Roles
{
    std::map<juce::String, juce::StringArray> identified;   // role -> control names (name + unit + unique in its section)
    juce::StringArray ambiguous, wrongUnit;                  // "role: a, b" / "name (role, unit seen 'x')"
    std::map<juce::String, juce::String> sectionFor;         // a person's section for a control, over what its name says
};
// measured: role -> controls a measurement identified (confirmed, or found on a control its name did not propose); dropped: controls a
// measurement REFUTED for the role their name proposed - the name rule never claims them back
struct Measured { std::map<juce::String, juce::StringArray> roles; std::set<juce::String> dropped;
                 std::map<juce::String, juce::String> section; };   // control -> its known section, when a person said which (16 Oct)
inline juce::String ruleRoleOfMeasured (const juce::String& recordRole, const juce::String& category)
{
    if (recordRole == "eq_gain") return "gain";
    if (recordRole == "frequency") return "freq";
    if (recordRole == "time") return category == "reverb" ? "decay" : "delay_time";
    return recordRole;   // threshold, ceiling, decay, mix, feedback, drive, q, attack, release, output, makeup, gain, input
}
inline Roles rolesOf (const juce::var& map, const Measured& measured = {})
{
    Roles R; std::map<juce::String, std::map<juce::String, juce::StringArray>> bySection;   // role -> section -> names
    const auto* co = map.getProperty ("controls", {}).getDynamicObject();
    std::set<juce::String> declined;
    if (const auto* d = map.getProperty ("declines", {}).getArray()) for (const auto& x : *d) declined.insert (x.getProperty ("control", x.getProperty ("name", "")).toString());
    if (co != nullptr)
        for (const auto& p : co->getProperties())
        {
            const auto& c = p.value; const auto name = c.getProperty ("name", p.name.toString()).toString();
            if (declined.count (name) || measured.dropped.count (name) || (bool) c.getProperty ("duplicate", false) || (int) c.getProperty ("lockstep_of", -1) >= 0) continue;
            const auto hit = roleOf (c); if (hit.role.isEmpty()) continue;
            if (! hit.unitOk) { R.wrongUnit.add (name + " (" + hit.role + ", unit '" + unitOf (c) + "')"); continue; }
            bySection[hit.role][sectionOf (name)].add (name);
        }
    for (const auto& [role, secs] : bySection)
        for (const auto& [sec, names] : secs)
        {
            if (names.size() == 1) R.identified[role].add (names[0]);
            else R.ambiguous.add (role + ": " + names.joinIntoString (", "));
        }
    for (const auto& [role, names] : measured.roles) for (const auto& n : names) R.identified[role].addIfNotAlreadyThere (n);   // measurement decides
    R.sectionFor = measured.section;
    return R;
}
// THE DIAL TARGETS: what a category needs identified (and read back) before it may be dialled. Each entry is alternatives ('|');
// "freq+gain" = an EQ band whose freq and gain share a section
inline juce::StringArray needsFor (const juce::String& category)
{
    static const std::map<juce::String, juce::StringArray> k {
        { "compressor",       { "threshold" } },
        { "limiter",          { "ceiling|threshold" } },
        { "eq",               { "freq+gain" } },
        { "de-esser",         { "threshold|freq" } },
        { "gate",             { "threshold" } },
        { "saturation",       { "drive" } },
        { "amp_sim",          { "drive|gain" } },
        { "reverb",           { "decay|mix|predelay" } },
        { "delay",            { "delay_time|mix" } },
        { "transient_shaper", { "attack|release|mix" } },
        { "channel_strip",    { "threshold|freq+gain" } },
    };
    const auto it = k.find (category.toLowerCase()); return it != k.end() ? it->second : juce::StringArray { "__never__" };   // pitch: the tuner spec decides
}
// the controls a need is met by (empty = not met)
// THE SECTION PREFIXES (Kathy's ruling, 16 Oct): a role word under a section prefix - Comp / Gate / Exp / EQ / Band N / LF / LMF / MF /
// HMF / HF / De-ess (and Limiter) - is identified within that section, so a strip has one threshold per section. The section of a
// control is what is left of its name without the role words (sectionOf); it is KNOWN when that is exactly one prefix (plus a number);
// "" is the plugin's own (no prefix); anything else is "?" - not a section we can name, so still ambiguous
inline juce::String knownSection (const juce::String& section)
{
    auto t = " " + section.toLowerCase().trim() + " ";
    t = t.replace (" de ess ", " deess ").replace (" de esser ", " deess ").replace (" deesser ", " deess ").replace (" de-ess ", " deess ");
    auto tokens = juce::StringArray::fromTokens (t, " ", ""); tokens.removeEmptyStrings();
    // the widget words a name may carry ("Gate Threshold slider", "Comp Knob") name no section of their own
    static const juce::StringArray kFiller { "slider", "knob", "fader", "dial", "control", "ctrl", "param", "parameter", "value", "amount", "amt" };
    for (int i = tokens.size(); --i >= 0;) if (kFiller.contains (tokens[i])) tokens.remove (i);
    if (tokens.isEmpty()) return "";
    static const std::map<juce::String, juce::String> canon {
        { "comp", "comp" }, { "compressor", "comp" }, { "cmp", "comp" }, { "gate", "gate" }, { "exp", "gate" }, { "expander", "gate" }, { "expand", "gate" },
        { "eq", "eq" }, { "equalizer", "eq" }, { "equaliser", "eq" }, { "lf", "lf" }, { "lmf", "lmf" }, { "mf", "mf" }, { "hmf", "hmf" }, { "hf", "hf" },
        { "low", "lf" }, { "lo", "lf" }, { "mid", "mf" }, { "high", "hf" }, { "hi", "hf" }, { "deess", "de-ess" }, { "ds", "de-ess" },
        { "lim", "limiter" }, { "limiter", "limiter" }, { "band", "band" } };
    const auto it = canon.find (tokens[0]); if (it == canon.end()) return "?";
    juce::String out = it->second;
    for (int i = 1; i < tokens.size(); ++i) { if (! tokens[i].containsOnly ("0123456789")) return "?"; out << " " << tokens[i]; }
    if (out == "band") return "?";   // "Band" with no number names no section
    return out;
}
// how a section reads on the review page: "gate" -> "Gate", "band 2" -> "Band 2", "de-ess" -> "De-ess"; none for "" and "?"
inline juce::String sectionLabel (const juce::String& known)
{
    if (known.isEmpty() || known == "?") return {};
    if (known == "comp") return "Comp"; if (known == "eq") return "EQ"; if (known == "de-ess") return "De-ess";
    if (known == "lf" || known == "lmf" || known == "mf" || known == "hmf" || known == "hf") return known.toUpperCase();
    return known.substring (0, 1).toUpperCase() + known.substring (1);
}
// the section a category's dial wants first: a compressor's own (or its Comp section), a gate's Gate / Exp, a de-esser's De-ess
inline juce::StringArray preferredSections (const juce::String& category)
{
    const auto c = category.toLowerCase();
    if (c == "compressor") return { "", "comp" };
    if (c == "channel_strip") return { "comp" };
    if (c == "gate") return { "", "gate" };
    if (c == "de-esser") return { "", "de-ess" };
    if (c == "limiter") return { "", "limiter" };
    return { "" };
}
inline juce::StringArray meetNeed (const juce::String& need, const Roles& R, const juce::String& category = {})
{
    auto secOf = [&R] (const juce::String& n) { const auto it = R.sectionFor.find (n); return it != R.sectionFor.end() ? it->second : knownSection (sectionOf (n)); };
    for (const auto& alt : juce::StringArray::fromTokens (need, "|", ""))
    {
        if (alt == "freq+gain")
        {
            const auto fi = R.identified.find ("freq"), gi = R.identified.find ("gain");
            if (fi == R.identified.end() || gi == R.identified.end()) continue;
            for (const auto& f : fi->second) for (const auto& g : gi->second)
                if (R.sectionFor.count (f) || R.sectionFor.count (g) ? secOf (f) == secOf (g) : sectionOf (f) == sectionOf (g)) return { f, g };
            continue;
        }
        // one control for the role; or, with several, each under its own KNOWN section and one of them the section this category dials
        // (Comp Threshold + Gate Threshold: the compressor dials Comp's); two in one section, or an unnamed section, stay ambiguous
        if (const auto it = R.identified.find (alt); it != R.identified.end() && ! it->second.isEmpty())
        {
            // one candidate: it is the dial unless it sits in a NAMED section this category does not dial (a lone Gate Threshold is
            // the gate's, never a compressor's - Kathy, 16 Oct, RChannel)
            if (it->second.size() == 1)
            {
                const auto k = secOf (it->second[0]);
                if (k.isEmpty() || k == "?" || preferredSections (category).contains (k) || category.isEmpty()) return { it->second[0] };
                continue;
            }
            std::map<juce::String, juce::String> bySec; bool clean = true;
            for (const auto& n : it->second) { const auto k = secOf (n); if (k == "?" || bySec.count (k)) { clean = false; break; } bySec[k] = n; }
            if (clean) for (const auto& want : preferredSections (category)) if (bySec.count (want)) return { bySec[want] };
        }
    }
    return {};
}

//==============================================================================
// 1b. THE CREDIT FROM EXISTING DATA: every {norm, display} reading in a record, by control name. A fixture's controls[].displayAt
// ({"0.500": "-12.0 dB"}); a Phase B record's lists of positions under the object that names their control ("control", or for an EQ
// band "gain_map" -> "gain_control", "freq_map" -> "freq_control", "q_map" -> "q_control"; saturation's "levels" {level: [...]})
struct Reading { double norm = 0.0; juce::String display; };
using Readings = std::map<juce::String, std::vector<Reading>>;   // control name -> readings
inline void readingsFromList (const juce::var& list, const juce::String& control, Readings& out)
{
    if (control.isEmpty() || control == "(none)") return;
    if (const auto* a = list.getArray())
        for (const auto& x : *a)
            if (x.isObject() && x.hasProperty ("norm") && x.hasProperty ("display"))
            {
                const auto d = x.getProperty ("display", "").toString();
                if (d.trim().isNotEmpty() && ! x.hasProperty ("why")) out[control].push_back ({ (double) x.getProperty ("norm", 0.0), d });
            }
}
inline void readingsWalk (const juce::var& v, Readings& out, int depth = 0)
{
    if (depth > 6) return;
    if (const auto* o = v.getDynamicObject())
    {
        const auto ctl = v.getProperty ("control", "").toString();
        for (const auto& p : o->getProperties())
        {
            const auto k = p.name.toString();
            if (p.value.isArray())
            {
                juce::String c = ctl;
                if (k.endsWith ("_map") && v.hasProperty (k.dropLastCharacters (4) + "_control")) c = v.getProperty (k.dropLastCharacters (4) + "_control", "").toString();
                readingsFromList (p.value, c, out);
            }
            if (k == "levels" && p.value.isObject()) if (const auto* lo = p.value.getDynamicObject()) for (const auto& l : lo->getProperties()) readingsFromList (l.value, ctl, out);
            if (k == "displayAt") continue;
            readingsWalk (p.value, out, depth + 1);
        }
    }
    else if (const auto* a = v.getArray()) for (const auto& x : *a) readingsWalk (x, out, depth + 1);
}
inline Readings readingsOfRecord (const juce::var& rec)
{
    Readings out;
    if (const auto* cs = rec.getProperty ("controls", {}).getArray())   // a certification fixture: every control at its norms, one load
        for (const auto& c : *cs)
            if (const auto* da = c.getProperty ("displayAt", {}).getDynamicObject())
                for (const auto& p : da->getProperties()) out[c.getProperty ("name", "").toString()].push_back ({ p.name.toString().getDoubleValue(), p.value.toString() });
    readingsWalk (rec, out);
    return out;
}
// WHEN AND ON WHAT VERSION a record read its displays (17 Oct, the recency rule): fixtures carry version + sampledAt, Phase B rows and
// profiles plugin.version + measured.date, the read-back sweep version + measuredAt. Time 0 = the record does not say
struct SourceMeta { juce::String version; juce::int64 atMs = 0; };
inline juce::int64 stampMs (const juce::String& s)
{
    const auto t = s.trim(); if (t.isEmpty()) return 0;
    if (t.length() >= 15 && t[8] == 'T' && t.substring (0, 8).containsOnly ("0123456789"))   // 20261016T232136 (nowStamp)
        return juce::Time (t.substring (0, 4).getIntValue(), t.substring (4, 6).getIntValue() - 1, t.substring (6, 8).getIntValue(),
                           t.substring (9, 11).getIntValue(), t.substring (11, 13).getIntValue(), t.substring (13, 15).getIntValue(), 0, true).toMilliseconds();
    if (t.length() == 10 && t[4] == '-' && t[7] == '-')                                       // 2026-10-04: the start of that day
        return juce::Time (t.substring (0, 4).getIntValue(), t.substring (5, 7).getIntValue() - 1, t.substring (8, 10).getIntValue(), 0, 0, 0, 0, true).toMilliseconds();
    return juce::Time::fromISO8601 (t).toMilliseconds();
}
inline SourceMeta metaOfRecord (const juce::var& rec)
{
    SourceMeta m;
    m.version = rec.getProperty ("version", rec.getProperty ("plugin", {}).getProperty ("version", "")).toString();
    m.atMs = stampMs (rec.getProperty ("measuredAt", rec.getProperty ("sampledAt", rec.getProperty ("measured", {}).getProperty ("date", ""))).toString());
    return m;
}
// THE MAP'S CONTROLS, both formats (17 Oct): the controls block, plus the older role-keyed params block (CLA-76, PuigChild 670) for any
// control the controls block does not carry - a params entry with an anchor table is read back as an anchored control
struct MapControl { juce::String name; juce::var c; };   // the map's own objects are never written (a var copy shares them)
inline std::vector<MapControl> mapControls (const juce::var& map)
{
    std::vector<MapControl> out; std::set<juce::String> names;
    if (const auto* co = map.getProperty ("controls", {}).getDynamicObject())
        for (const auto& p : co->getProperties())
        {
            const auto n = p.value.getProperty ("name", p.name.toString()).toString();
            names.insert (n); out.push_back ({ n, p.value });
        }
    if (const auto* po = map.getProperty ("params", {}).getDynamicObject())
        for (const auto& p : po->getProperties())
        {
            const auto& c = p.value; const auto n = c.getProperty ("name", "").toString();
            if (n.isEmpty() || names.count (n) || anchorsOf (c).size() < 2) continue;
            auto* o = new juce::DynamicObject(); o->setProperty ("name", n); o->setProperty ("index", c.getProperty ("index", -1)); o->setProperty ("kind", "anchored");
            o->setProperty ("anchors", c.getProperty ("anchors", {})); o->setProperty ("from", "params"); out.push_back ({ n, juce::var (o) }); names.insert (n);
        }
    return out;
}
// credit one map from the readings of its product's records (any version): per control, every reading judged against THIS map.
// THE RECENCY RULE (Kathy, 17 Oct): a control's failed reading is set aside when a NEWER source on the map's own version read the
// control back at 2+ settings with none failing; an old failure stands only if nothing newer contradicts it (an undated one stands)
inline std::map<juce::String, ControlReadback> creditMap (const juce::var& map, const std::vector<std::pair<juce::String, Readings>>& sources,
                                                          const std::vector<SourceMeta>& metas = {})
{
    std::map<juce::String, ControlReadback> out;
    const auto mapVersion = map.getProperty ("identity", {}).getProperty ("version", "").toString();
    for (const auto& [name, c] : mapControls (map))
    {
        const bool mode = c.getProperty ("kind", "").toString() == "mode";
        if (! mode && c.getProperty ("kind", "").toString() != "anchored") continue;
        const auto* lo = c.getProperty ("labels", {}).getDynamicObject(); if (mode && lo == nullptr) continue;
        const auto a = anchorsOf (c);
        std::vector<std::pair<size_t, Verdict>> vs;   // (source, verdict)
        for (size_t si = 0; si < sources.size(); ++si)
            if (const auto it = sources[si].second.find (name); it != sources[si].second.end())
                for (const auto& r : it->second)
                {
                    if (! mode) { vs.push_back ({ si, judgeObservation (a, r.norm, r.display) }); continue; }
                    // a stepped control: the reading at a norm must show the label the map has nearest that norm
                    juce::String best; double bd = 1e9;
                    for (const auto& l : lo->getProperties()) { const double d = std::abs ((double) l.value - r.norm); if (d < bd) { bd = d; best = l.name.toString(); } }
                    if (bd > 0.02 || r.display.trim().isEmpty()) continue;   // not at one of the map's own positions: nothing to compare
                    Verdict v; v.judged = true; v.pass = r.display.trim().equalsIgnoreCase (best.trim());
                    v.why = "norm " + juce::String (r.norm, 3) + ": the map's label '" + best + "', the display read '" + r.display.trim() + "'";
                    vs.push_back ({ si, v });
                }
        juce::int64 newestClean = 0;   // the newest source on this map's version that read this control back cleanly at 2+ settings
        if (metas.size() == sources.size())
            for (size_t si = 0; si < sources.size(); ++si)
            {
                if (metas[si].atMs <= 0 || metas[si].version != mapVersion) continue;
                int pass = 0, fail = 0; for (const auto& [s, v] : vs) if (s == si && v.judged) (v.pass ? pass : fail)++;
                if (pass >= 2 && fail == 0) newestClean = std::max (newestClean, metas[si].atMs);
            }
        auto& cr = out[name];
        for (const auto& [si, v] : vs)
        {
            if (v.judged && ! v.pass && newestClean > 0 && metas[si].atMs > 0 && metas[si].atMs < newestClean) { ++cr.superseded; continue; }
            addReading (cr, v, sources[si].first);
        }
        if (cr.judged == 0 && cr.superseded == 0) out.erase (name);
    }
    return out;
}

//==============================================================================
// 3. THE LEVEL
struct Level { juce::String level, why; juce::StringArray dialable; };
inline Level levelFor (const juce::var& map, bool certified, const juce::String& heldWhy, const std::map<juce::String, ControlReadback>& rb,
                       const Measured& measured = {})
{
    Level L;
    if (certified) { L.level = "certified"; L.why = "passed its category's v1 certification"; return L; }
    if (heldWhy.isNotEmpty()) { L.level = "unsure"; L.why = "held: " + heldWhy; return L; }
    const auto R = rolesOf (map, measured);
    juce::StringArray missing, unread;
    for (const auto& need : needsFor (map.getProperty ("category", "").toString()))
    {
        auto ctl = meetNeed (need, R, map.getProperty ("category", "").toString());
        if (ctl.isEmpty()) { missing.add (need); continue; }
        for (const auto& c : ctl) { const auto it = rb.find (c); if (it == rb.end() || ! it->second.proven()) unread.add (c + (it != rb.end() && it->second.failed > 0 ? " (read-back failed: " + it->second.worst + ")" : " (no read-back yet)")); else L.dialable.addIfNotAlreadyThere (c); }
    }
    if (! missing.isEmpty()) { L.level = "unsure"; L.why = "role not identified: " + missing.joinIntoString ("; ") + (R.ambiguous.isEmpty() ? juce::String() : " (ambiguous: " + R.ambiguous.joinIntoString ("; ") + ")"); L.dialable.clear(); return L; }
    if (! unread.isEmpty()) { L.level = "unsure"; L.why = "not read back: " + unread.joinIntoString ("; "); L.dialable.clear(); return L; }
    L.level = "mapped"; L.why = "roles identified and read back: " + L.dialable.joinIntoString (", "); return L;
}

} // namespace ejmap::trust
