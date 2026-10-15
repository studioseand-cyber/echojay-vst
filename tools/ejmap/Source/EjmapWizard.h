/*
  EjmapWizard.h - the user-side wizard (EJMAP_COMMUNITY_MAPPING_PLAN.md v0.1, Kathy 10 Oct; branch feat/ejmap-wizard). The PURE parts:

  1. THE LOOKUP (plan section 3, rule A / B / C) against a database of profiles (here a LOCAL MOCK: a folder of ej_comp_profile/1
     files + their tone-check sidecars; no network):
       A exact          the same plugin (uid) and version, and the same map_fp when both are known      -> use as is
       B same controls  the same plugin, another version (or another map_fp)                             -> the quick live check
       C                no profile for the plugin, or B's check failed                                    -> queue for measuring
  2. THE QUICK LIVE CHECK (plan section 3, "the read-back check"): every control the profile WRITES (the amount curve, the ratio
     curve, the neutral and engage writes) is written at the profile's norms and read back; every display must equal the profile's.
     Only the written controls count - a new feature knob does not break a profile. No audio.
  3. THE BUNDLE: profiles + tone-check sidecars + a manifest; nothing personal - every file is checked for the user's home path,
     user name, host name and anything token-like before it is packed; a hit refuses the bundle.
  4. THE SUMMARY in plain words ("23 plugins ready, 4 measuring, 2 need a licence").
*/
#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <optional>
#include <vector>
#include <algorithm>
#include <cmath>

namespace ejmap::wizard
{

// ---------------------------------------------------------------- the mock database
// indexOnly (13 Oct, the beta snapshot): an "already mapped" entry from <db>/mapped.json - another category's measured Phase B draft, by
// plugin id + version + map fp (when known) and its category, without the draft itself: it can answer rule A, never rule B (no writes)
// pass (13 Oct, limiters): the Phase B pass an entry covers - "limiter" (the limiter draft) and "limitercomp" (a limiter's compressor pass,
// <db>/limitercomp/) are looked up separately; "" for a compressor / EQ profile and the other categories' drafts
struct DbEntry { juce::var profile, tonecheck; juce::String file, cat, pass; bool indexOnly = false; juce::String uid() const; juce::String version() const; juce::String mapFp() const;
                 juce::String category() const { if (cat.isNotEmpty()) return cat; return profile.getProperty ("schema", "").toString().startsWith ("ej_eq_profile") ? "eq" : "compressor"; } };
inline juce::String uidOfPluginId (const juce::String& pid) { return pid.fromFirstOccurrenceOf ("|", false, false).upToFirstOccurrenceOf ("|", false, false).toLowerCase(); }
inline juce::String DbEntry::uid() const { return uidOfPluginId (profile.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString()); }
inline juce::String DbEntry::version() const { return profile.getProperty ("plugin", {}).getProperty ("version", "").toString(); }
inline juce::String DbEntry::mapFp() const { return profile.getProperty ("plugin", {}).getProperty ("map_fp", "").toString(); }
inline std::vector<DbEntry> loadDb (const juce::File& folder)
{
    std::vector<DbEntry> db;
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.json"); files.addArray (folder.getChildFile ("limitercomp").findChildFiles (juce::File::findFiles, false, "*.json"));
    for (const auto& f : files)
    {
        if (f.getFileName().endsWith (".tonecheck.json")) continue;
        DbEntry e; e.profile = juce::JSON::parse (f.loadFileAsString()); e.file = f.getFileName();
        const auto schema = e.profile.getProperty ("schema", "").toString();
        if (! schema.startsWith ("ej_comp_profile") && ! schema.startsWith ("ej_eq_profile")) continue;   // the two categories the wizard measures
        const auto tc = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tonecheck.json"); if (tc.existsAsFile()) e.tonecheck = juce::JSON::parse (tc.loadFileAsString());
        if (f.getParentDirectory().getFileName() == "limitercomp") { e.pass = "limitercomp"; e.cat = "limiter"; }   // a limiter's compressor pass
        db.push_back (e);
    }
    // <db>/mapped.json (ej_wizard_mapped/1): {"entries": [{"plugin_id", "version", "map_fp" (or null), "category", ...}]}
    const auto m = juce::JSON::parse (folder.getChildFile ("mapped.json").loadFileAsString());
    if (const auto* a = m.getProperty ("entries", {}).getArray())
        for (const auto& x : *a)
        {
            DbEntry e; e.indexOnly = true; e.file = "mapped.json"; e.cat = x.getProperty ("category", "").toString();
            if (x.getProperty ("measured", "").toString() == "limiter") e.pass = "limiter";
            auto* pl = new juce::DynamicObject(); pl->setProperty ("plugin_id", x.getProperty ("plugin_id", "")); pl->setProperty ("version", x.getProperty ("version", ""));
            pl->setProperty ("map_fp", x.getProperty ("map_fp", {}).isString() ? x.getProperty ("map_fp", "") : juce::var ("")); pl->setProperty ("name", x.getProperty ("product", ""));
            auto* pr = new juce::DynamicObject(); pr->setProperty ("plugin", juce::var (pl)); e.profile = juce::var (pr);
            if (e.uid().isNotEmpty() && e.version().isNotEmpty()) db.push_back (e);
        }
    return db;
}

// ---------------------------------------------------------------- 1. the lookup
enum class Match { exact, sameControls, none };
struct Lookup { Match match = Match::none; const DbEntry* entry = nullptr; juce::String why; };
inline Lookup lookup (const std::vector<DbEntry>& db, const juce::String& uid, const juce::String& version, const juce::String& mapFp)
{
    Lookup L; const DbEntry* other = nullptr;
    for (const auto& e : db)
    {
        if (e.uid() != uid.toLowerCase()) continue;
        const bool sameVersion = e.version() == version;
        const bool fpKnown = mapFp.isNotEmpty() && e.mapFp().isNotEmpty();
        if (sameVersion && (! fpKnown || e.mapFp() == mapFp)) { L.match = Match::exact; L.entry = &e; L.why = "the same plugin and version" + juce::String (fpKnown ? " and map" : ""); return L; }
        if (other == nullptr && ! e.indexOnly) other = &e;   // rule B checks a profile's writes: an index entry has none
    }
    if (other != nullptr) { L.match = Match::sameControls; L.entry = other; L.why = "a profile from version " + other->version() + (other->version() == version ? " (another map)" : "") + ": check its controls"; return L; }
    L.why = "no profile for this plugin yet"; return L;
}

// ---------------------------------------------------------------- 2. the quick live check
struct Write { juce::String control; int index = -1; double norm = 0.0; juce::String expect; };
inline std::vector<Write> writesOf (const juce::var& profile)
{
    std::vector<Write> w;
    auto curve = [&] (const char* block, const char* textKey)
    {
        const auto b = profile.getProperty (block, {}); const auto name = b.getProperty ("control", "").toString();
        if (const auto* c = b.getProperty ("curve", {}).getArray()) for (const auto& p : *c) w.push_back ({ name, -1, (double) p.getProperty ("norm", 0.0), p.getProperty (textKey, "").toString() });
    };
    curve ("amount", "display"); curve ("ratio", "set");
    // an EQ profile (ej_eq_profile/1): each band's gain / frequency / Q maps, by the band's control names
    if (const auto* bands = profile.getProperty ("bands", {}).getArray())
        for (const auto& b : *bands)
            for (const auto& [mapKey, ctlKey] : { std::pair<const char*, const char*> { "gain_map", "gain_control" }, { "freq_map", "freq_control" }, { "q_map", "q_control" } })
                if (const auto* m = b.getProperty (mapKey, {}).getArray())
                    for (const auto& p : *m) w.push_back ({ b.getProperty (ctlKey, "").toString(), -1, (double) p.getProperty ("norm", 0.0), p.getProperty ("display", "").toString() });
    for (const char* list : { "neutral", "engage" })
        if (const auto* a = profile.getProperty (list, {}).getArray()) for (const auto& x : *a)
            w.push_back ({ x.getProperty ("control", "").toString(), (int) x.getProperty ("index", -1), (double) x.getProperty ("norm", 0.0), x.getProperty ("set", "").toString() });
    // a placeholder control ("-", or none) is not a write
    w.erase (std::remove_if (w.begin(), w.end(), [] (const Write& x) { return x.control.trim().isEmpty() || x.control.trim() == "-"; }), w.end());
    return w;
}
// observed: control index -> norm (rounded to 1e-4) -> the display read back; names: control name -> index (the live plugin's list)
// NEAR-MATCH TOLERANCE (Kathy, 14 Oct; the WIZARD's read-back only - the certification's accuracy rules are untouched): two readings
// match when both parse as a number in the SAME unit and differ by no more than 0.5% (of the larger) or one step of the last shown digit
// (the coarser of the two), whichever is larger - and never by more than 1% of the larger (the ceiling, 14 Oct). Text / enum values (and ratios like 4:1) still match exactly; units must match.
struct NumReading { bool ok = false; double value = 0.0; juce::String unit; int decimals = 0; };
inline NumReading parseNumeric (const juce::String& raw)
{
    NumReading n; const auto s = raw.trim(); int i = 0, len = s.length();
    if (i < len && (s[i] == '-' || s[i] == '+')) ++i;
    const int numStart = i; bool dot = false; int dec = 0, digits = 0;
    for (; i < len; ++i)
    {
        const auto c = s[i];
        if (c >= '0' && c <= '9') { ++digits; if (dot) ++dec; }
        else if (c == '.' && ! dot) dot = true;
        else break;
    }
    if (digits == 0) return n;
    const auto unit = s.substring (i).trim();
    if (unit.containsAnyOf ("0123456789:,/")) return n;   // "1:1", "1,000", "2/3": not a plain number - compared as text
    n.ok = true; n.value = s.substring (0, i).getDoubleValue(); n.unit = unit.removeCharacters (" ").toLowerCase(); n.decimals = dec;
    juce::ignoreUnused (numStart);
    return n;
}
inline bool nearMatch (const juce::String& ours, const juce::String& theirs)
{
    const auto a = parseNumeric (ours), b = parseNumeric (theirs);
    if (! a.ok || ! b.ok || a.unit != b.unit) return false;
    const double step = std::pow (10.0, -(double) juce::jmin (a.decimals, b.decimals));
    const double larger = juce::jmax (std::abs (a.value), std::abs (b.value));
    // the hard ceiling (Kathy, 14 Oct): never more than 1% of the larger, whatever the last-digit step says (42 vs 41 is 2.4%: no match)
    const double allowed = juce::jmin (juce::jmax (0.005 * larger, step), 0.01 * larger);
    return std::abs (a.value - b.value) <= allowed + 1e-12;
}
struct NearMatchRec { juce::String control, norm, ours, theirs; };
struct Readback { bool pass = false; int checked = 0; juce::String why; juce::String mode; int loads = 0; std::vector<NearMatchRec> near; };   // mode / loads (14 Oct): one_load or per_control, and how many loads
// THE ONE-LOAD READ-BACK (Kathy, 14 Oct): every control the check reads, in one probe call - "<i>:<n0>,<n1>;<j>:<n0>"
inline juce::String multiNormsSpec (const std::map<int, juce::StringArray>& norms)
{
    juce::StringArray parts; for (const auto& [idx, ns] : norms) if (! ns.isEmpty()) parts.add (juce::String (idx) + ":" + ns.joinIntoString (","));
    return parts.joinIntoString (";");
}
// the probe's text-at lines, one control's (--text-at-norms) or many (--text-at-norms-multi: each block opens with a "textat ... index <i>"
// line); singleIndex >= 0: lines before any header belong to it
inline std::map<int, std::map<juce::String, juce::String>> parseTextAt (const juce::String& out, int singleIndex = -1);
inline juce::String normKey (double n) { return juce::String (std::round (n * 10000.0) / 10000.0, 4); }
inline juce::String textNorm (const juce::String& s) { return s.trim().removeCharacters (" ").toLowerCase(); }
inline std::map<int, std::map<juce::String, juce::String>> parseTextAt (const juce::String& out, int singleIndex)
{
    std::map<int, std::map<juce::String, juce::String>> observed; int cur = singleIndex;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() >= 5 && f[0] == "textat") { const int k = f.indexOf ("index"); if (k >= 0 && k + 1 < f.size()) cur = f[k + 1].getIntValue(); continue; }
        if (f.size() < 4 || f[0] != "at" || cur < 0) continue;
        const int t = f.indexOf ("text"); observed[cur][normKey (f[1].getDoubleValue())] = t >= 0 && t + 1 < f.size() ? f[t + 1] : juce::String();
    }
    return observed;
}
// the audit's list: control, norm, ours (this Mac's display), theirs (the profile's)
inline juce::var nearMatchesVar (const std::vector<NearMatchRec>& near)
{
    juce::Array<juce::var> a;
    for (const auto& n : near) { auto* o = new juce::DynamicObject(); o->setProperty ("control", n.control); o->setProperty ("norm", n.norm); o->setProperty ("ours", n.ours); o->setProperty ("theirs", n.theirs); a.add (juce::var (o)); }
    return a;
}
inline Readback readback (const std::vector<Write>& writes, const std::map<juce::String, int>& names, const std::map<int, std::map<juce::String, juce::String>>& observed)
{
    Readback r;
    for (const auto& w : writes)
    {
        int idx = w.index;
        if (const auto it = names.find (w.control); it != names.end()) { if (idx >= 0 && idx != it->second) { r.why = "'" + w.control + "' moved from index " + juce::String (idx) + " to " + juce::String (it->second); return r; } idx = it->second; }
        else { r.why = "the plugin has no control named '" + w.control + "'"; return r; }
        const auto o = observed.find (idx); if (o == observed.end()) { r.why = "'" + w.control + "' was not read back"; return r; }
        const auto t = o->second.find (normKey (w.norm)); if (t == o->second.end()) { r.why = "'" + w.control + "' not read at " + normKey (w.norm); return r; }
        if (w.expect.isNotEmpty() && textNorm (t->second) != textNorm (w.expect))
        {
            if (! nearMatch (t->second, w.expect)) { r.why = "'" + w.control + "' at " + normKey (w.norm) + " shows '" + t->second + "', the profile says '" + w.expect + "'"; r.near.clear(); return r; }
            r.near.push_back ({ w.control, normKey (w.norm), t->second, w.expect });   // within tolerance: recorded for the audit
        }
        ++r.checked;
    }
    r.pass = r.checked > 0; if (! r.pass) r.why = "the profile writes nothing to check";
    else r.why = juce::String (r.checked) + " write(s) read back as the profile says" + (r.near.empty() ? juce::String() : " (" + juce::String ((int) r.near.size()) + " within tolerance)");
    return r;
}

// ---------------------------------------------------------------- 0. no map store: the wizard maps and categorises itself
// the category the wizard measures a plugin as: the server's catalogue first (the mock: <db>/categories.json, product -> category), then
// the name - only words that say compressor (not "dynamics": a gate / limiter / de-esser is not measured as a compressor) or EQ;
// anything else is not measured by the wizard yet
inline juce::String wizardCategory (const juce::String& name, const juce::var& catalogue)
{
    const auto c = catalogue.getProperty (juce::Identifier (name), {}).toString();
    if (c == "compressor" || c == "eq") return c;
    if (c.isNotEmpty()) return {};   // the catalogue says something the wizard does not measure
    auto t = juce::StringArray::fromTokens (name.toLowerCase(), " -_/()[]:.,", "\"'"); t.removeEmptyStrings();
    for (const auto& x : t) if (x == "comp" || x == "compressor" || x == "compression" || x == "opto" || x == "vca" || x == "leveler" || x == "leveller") return "compressor";
    for (const auto& x : t) if (x == "eq" || x == "equalizer" || x == "equaliser") return "eq";
    return {};
}
// the wizard's own parameter map (no human or model mapping: identity, the parameter list and the join key), written to <store>/maps/<fp>.json
inline juce::var paramMap (const juce::String& name, const juce::String& vendor, const juce::String& uid, const juce::String& version, const juce::String& fp,
                           const std::vector<std::pair<int, juce::String>>& params, const juce::String& category)
{
    auto* id = new juce::DynamicObject(); id->setProperty ("format", "AudioUnit"); id->setProperty ("uid", uid.toLowerCase()); id->setProperty ("name", name); id->setProperty ("vendor", vendor);
    id->setProperty ("version", version); id->setProperty ("param_count", (int) params.size());
    juce::Array<juce::var> ps; for (const auto& [i, n] : params) { auto* p = new juce::DynamicObject(); p->setProperty ("index", i); p->setProperty ("name", n); ps.add (juce::var (p)); }
    auto* m = new juce::DynamicObject(); m->setProperty ("fp", fp); m->setProperty ("schema", "ej_param_map/wizard-0"); m->setProperty ("identity", juce::var (id));
    m->setProperty ("category", category); m->setProperty ("params", ps); m->setProperty ("source", "the wizard: the parameter list read by the probe (no roles mapped)");
    return juce::var (m);
}
// an EQ profile is ready to send when at least one band is measured and every acceptance write that ran passed
inline bool eqAccepted (const juce::var& eqProfile, juce::String& why)
{
    int measured = 0, ran = 0, failed = 0, notRun = 0; juce::String notRunWhy;
    if (const auto* bands = eqProfile.getProperty ("bands", {}).getArray())
        for (const auto& b : *bands)
        {
            if (b.getProperty ("verdict", "").toString() == "measured") ++measured;
            if (const auto* acc = b.getProperty ("acceptance", {}).getArray()) for (const auto& a : *acc)
            { if ((bool) a.getProperty ("ran", false)) { ++ran; if (! (bool) a.getProperty ("pass", false)) ++failed; } else { ++notRun; if (notRunWhy.isEmpty()) notRunWhy = a.getProperty ("why", "").toString(); } }
        }
    why = juce::String (measured) + " band(s) measured, " + juce::String (ran - failed) + " of " + juce::String (ran) + " acceptance write(s) passed"
        + (notRun > 0 ? "; " + juce::String (notRun) + " could not run (" + notRunWhy + ")" : juce::String());
    return measured > 0 && ran > 0 && failed == 0;
}

// ---------------------------------------------------------------- 3. the bundle
// what must never leave the Mac: the home path, the user's name, the host name, and anything that looks like a credential
inline juce::StringArray personalProblems (const juce::String& text, const juce::String& homeDir, const juce::String& userName, const juce::String& hostName)
{
    juce::StringArray p;
    if (homeDir.isNotEmpty() && text.contains (homeDir)) p.add ("the home folder path");
    if (text.contains ("/Users/")) p.add ("a /Users/ path");
    if (userName.length() >= 3 && text.containsIgnoreCase (userName)) p.add ("the user name");
    if (hostName.length() >= 3 && text.containsIgnoreCase (hostName)) p.add ("the computer's name");
    for (const char* k : { "\"token\"", "mapper_token", "api_key", "apikey", "secret", "Bearer ", "Authorization", "password" }) if (text.containsIgnoreCase (k)) p.add (juce::String ("a credential-like field (") + k + ")");
    return p;
}

// ---------------------------------------------------------------- 3b. A DEMO VERSION (Kathy, 13 Oct, beta item 2: a stranger's Mac)
// A stranger's Mac has no licence file, so a demo is found by what it does: before a plugin is measured, one steady 997 Hz tone at
// -18 dBFS runs through it at its opening state for kDemoHoldS (offline: ~1 s of rendering), read in 50 ms windows. A DROPOUT is the
// output falling more than kDemoDipDb below its own median while the input holds, and then coming back - the demo's periodic mute.
// Measured here 13 Oct: Melda's MTurboComp, MLimiterX and MFreeformEqualizer (demos) dip 23 dB from 19-25 s in; 11 licensed plugins of
// every measured category (compressor, de-esser, limiter, amp sim, gate, transient, multiband, tuner, delay, reverb, strip) never do.
// A demo is not measured and not sent. Output silent throughout is not judged here (silent_output's business); the first 3 s settle.
inline constexpr double kDemoHoldS = 60.0, kDemoDipDb = 20.0, kDemoSettleMs = 3000.0;
inline juce::StringArray demoProbeArgs() { return { "--burst", "quiet=-60", "loud=-18", "pre=1.0", "hold=" + juce::String (juce::roundToInt (kDemoHoldS)), "post=0.5", "hz=997", "win_ms=50" }; }
struct DemoVerdict { bool judged = false, demo = false, window = false, silent = false, inert = false; int dropouts = 0; double firstS = 0.0, depthDb = 0.0; juce::String why; };   // window (14 Oct): a licence window at the load
inline DemoVerdict demoDropouts (const juce::String& trace)
{
    DemoVerdict v; std::vector<std::pair<double, std::pair<double, double>>> w;   // t_ms -> (in, out), the loud segment after the settle
    for (const auto& l : juce::StringArray::fromLines (trace))
    {
        if (! l.startsWith ("bwin\t")) continue;
        const auto f = juce::StringArray::fromTokens (l, "\t", "");
        auto val = [&] (const char* k) { const int i = f.indexOf (k); return i >= 0 && i + 1 < f.size() ? f[i + 1] : juce::String(); };
        if (val ("seg") != "loud") continue;
        const double t = val ("t_ms").getDoubleValue(); if (t < kDemoSettleMs) continue;
        w.push_back ({ t, { val ("in_db").getDoubleValue(), val ("out_db").getDoubleValue() } });
    }
    if (w.size() < 20) { v.why = "no steady-tone reading to judge"; return v; }
    auto median = [] (std::vector<double> x) { std::sort (x.begin(), x.end()); return x[x.size() / 2]; };
    std::vector<double> ins, outs; for (const auto& [t, io] : w) { ins.push_back (io.first); outs.push_back (io.second); }
    const double mi = median (ins), mo = median (outs);
    if (mo < -100.0) { v.why = "silent output: not judged here"; return v; }
    v.judged = true;
    bool inDip = false; double deepest = 0.0;
    for (const auto& [t, io] : w)
    {
        const bool steadyIn = std::abs (io.first - mi) <= 1.0, dip = steadyIn && io.second < mo - kDemoDipDb;
        if (dip && ! inDip) { inDip = true; if (v.dropouts == 0) v.firstS = t / 1000.0; }
        if (dip) deepest = juce::jmin (deepest, io.second - mo);
        if (! dip && inDip && io.second >= mo - 3.0) { inDip = false; ++v.dropouts; }   // it came back: a dropout, not a stop
    }
    v.depthDb = -deepest; v.demo = v.dropouts >= 1;
    v.why = v.demo ? "a demo version: its output drops out " + juce::String (v.dropouts) + " time(s) on a steady tone (" + juce::String (juce::roundToInt (v.depthDb)) + " dB, first at " + juce::String (juce::roundToInt (v.firstS)) + " s) - not measured, not sent"
                   : "no dropout on a steady tone over " + juce::String (juce::roundToInt (kDemoHoldS)) + " s";
    return v;
}

// ---------------------------------------------------------------- 4. the plain-words summary
// state per plugin: ready (A), ready_from_version (B passed), queued, measuring, measured (ready to send), needs_licence, needs_hardware,
// silent, crashed, wont_load, not_measurable (failed / nothing to measure / needs review)
inline juce::String plainOutcome (const juce::String& outcome, const juce::String& certState)
{
    if (outcome == "needs_licence" || outcome == "window") return "needs_licence";
    if (outcome == "needs_device") return "needs_hardware";
    if (outcome == "silent_output") return "silent";
    if (outcome == "probe_crashed") return "crashed";
    if (outcome == "unhostable" || outcome == "empty_param_list") return "wont_load";
    if (outcome == "custom_default") return "custom_default";
    if (outcome == "demo") return "demo";   // 13 Oct: a demo version, found by its dropouts - skipped quietly   // 12 Oct: opens differently from its reference and could not be restored
    if (outcome == "ok" && certState == "exported") return "measured";
    return "not_measurable";
}
// THE REFERENCE VOTE (Kathy, 12 Oct): every plugin checked against a reference casts one (EjmapCustomDefault.h voteOf); the upload carries
// it so the server can count the Macs that agree. The entry names the reference by its file (<stem>.reference.json), the plugin, the
// vote, and - when it disagrees - this Mac's opening block, so a third source can settle it. A row with no vote -> void.
inline juce::var referenceVoteEntry (const juce::var& row)
{
    const auto v = row.getProperty ("reference_vote", {}); const auto stem = row.getProperty ("reference_stem", "").toString();
    if (! v.isObject() || stem.isEmpty()) return {};
    auto* o = new juce::DynamicObject(); const juce::var e (o);
    o->setProperty ("product", row.getProperty ("product", {})); o->setProperty ("plugin_id", row.getProperty ("plugin_id", {}));
    o->setProperty ("reference", stem + ".reference.json");
    o->setProperty ("agrees", v.getProperty ("agrees", false)); o->setProperty ("agreeing_before", v.getProperty ("agreeing_before", 0)); o->setProperty ("makes_trusted", v.getProperty ("makes_trusted", false));
    if (v.hasProperty ("opening_block")) o->setProperty ("opening_block", v.getProperty ("opening_block", {}));
    o->setProperty ("measured_here", row.getProperty ("state", "").toString() == "measured");
    return e;
}
// a custom_default plugin is HELD (Kathy, 12 Oct): not measured, listed with why, until a third source settles its reference
inline juce::var heldCustomDefault (const juce::var& row)
{
    if (row.getProperty ("state", "").toString() != "custom_default") return {};
    auto* h = new juce::DynamicObject(); const juce::var e (h);
    h->setProperty ("product", row.getProperty ("product", {})); h->setProperty ("plugin_id", row.getProperty ("plugin_id", {})); h->setProperty ("map_fp", row.getProperty ("map_fp", {}));
    h->setProperty ("held_reason", "custom_default");
    h->setProperty ("why", "held: opens differently from its reference (" + row.getProperty ("why", "").toString() + "); its opening block is in reference_votes");
    return e;
}
// FINISH NOW (Kathy, 13 Oct): a plugin still waiting when the user finished - listed, not measured
inline constexpr const char* kNotReachedWhy = "not measured: the wizard was finished before it got to it";
inline juce::String summaryLine (const std::map<juce::String, int>& n, bool sent = true)   // sent = false: a run that makes no bundle says 'will be sent'
{
    auto get = [&] (const char* k) { const auto it = n.find (k); return it == n.end() ? 0 : it->second; };
    auto plural = [] (int k, const char* one, const char* many) { return juce::String (k) + " " + (k == 1 ? one : many); };
    juce::StringArray parts;
    const int ready = get ("ready") + get ("ready_from_version");
    if (ready) parts.add (plural (ready, "plugin ready", "plugins ready"));
    if (get ("measured")) parts.add (plural (get ("measured"), "newly measured, ready to send", "newly measured, ready to send"));
    if (get ("queued")) parts.add (juce::String (get ("queued")) + " to measure");
    if (get ("measuring")) parts.add (juce::String (get ("measuring")) + " measuring");
    if (get ("needs_licence")) parts.add (plural (get ("needs_licence"), "needs a licence", "need a licence"));
    if (get ("needs_hardware")) parts.add (plural (get ("needs_hardware"), "needs its hardware (UAD / APB) connected", "need their hardware (UAD / APB) connected"));
    if (get ("silent")) parts.add (plural (get ("silent"), "passes no sound", "pass no sound"));
    if (get ("crashed")) parts.add (plural (get ("crashed"), "crashed (we moved on)", "crashed (we moved on)"));
    if (get ("wont_load")) parts.add (plural (get ("wont_load"), "won't load on this Mac", "won't load on this Mac"));
    if (get ("not_in_scope")) parts.add (plural (get ("not_in_scope"), "is not measured by the wizard yet", "are not measured by the wizard yet"));
    if (get ("not_measurable")) parts.add (plural (get ("not_measurable"), "could not be measured", "could not be measured"));
    // NOTHING DROPPED (12 Oct): what was sent instead of measured, and what another user already sent
    if (get ("needs_category")) parts.add (plural (get ("needs_category"), sent ? "plugin we couldn't identify was sent for review" : "plugin we couldn't identify will be sent for review", sent ? "plugins we couldn't identify were sent for review" : "plugins we couldn't identify will be sent for review"));
    if (get ("other_category")) parts.add (plural (get ("other_category"), sent ? "plugin of a kind we don't measure yet was sent so EchoJay knows it" : "plugin of a kind we don't measure yet will be sent so EchoJay knows it", sent ? "plugins of kinds we don't measure yet were sent so EchoJay knows them" : "plugins of kinds we don't measure yet will be sent so EchoJay knows them"));
    if (get ("out_of_scope")) parts.add (plural (get ("out_of_scope"), sent ? "instrument / meter / utility was listed, not loaded" : "instrument / meter / utility will be listed, not loaded", sent ? "instruments / meters / utilities were listed, not loaded" : "instruments / meters / utilities will be listed, not loaded"));
    if (get ("demo")) parts.add (plural (get ("demo"), "plugin is a demo version and was skipped", "plugins are demo versions and were skipped"));
    if (get ("custom_default")) parts.add (plural (get ("custom_default"), "plugin opens with a custom default and was not measured", "plugins open with a custom default and were not measured"));
    if (get ("pending_review")) parts.add (plural (get ("pending_review"), "was already sent for review by someone else", "were already sent for review by someone else"));
    if (get ("waves_skipped")) parts.add (plural (get ("waves_skipped"), "Waves plugin skipped - check Waves Central", "Waves plugins skipped - check Waves Central"));
    if (get ("not_reached")) parts.add (plural (get ("not_reached"), "plugin was not reached before you finished", "plugins were not reached before you finished"));
    if (get ("identifying")) parts.add (juce::String (get ("identifying")) + " being identified");
    return parts.isEmpty() ? juce::String ("nothing to do") : parts.joinIntoString (", ");
}

} // namespace ejmap::wizard
