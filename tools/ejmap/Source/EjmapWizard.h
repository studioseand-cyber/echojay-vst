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

namespace ejmap::wizard
{

// ---------------------------------------------------------------- the mock database
struct DbEntry { juce::var profile, tonecheck; juce::String file; juce::String uid() const; juce::String version() const; juce::String mapFp() const;
                 juce::String category() const { return profile.getProperty ("schema", "").toString().startsWith ("ej_eq_profile") ? "eq" : "compressor"; } };
inline juce::String uidOfPluginId (const juce::String& pid) { return pid.fromFirstOccurrenceOf ("|", false, false).upToFirstOccurrenceOf ("|", false, false).toLowerCase(); }
inline juce::String DbEntry::uid() const { return uidOfPluginId (profile.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString()); }
inline juce::String DbEntry::version() const { return profile.getProperty ("plugin", {}).getProperty ("version", "").toString(); }
inline juce::String DbEntry::mapFp() const { return profile.getProperty ("plugin", {}).getProperty ("map_fp", "").toString(); }
inline std::vector<DbEntry> loadDb (const juce::File& folder)
{
    std::vector<DbEntry> db;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (f.getFileName().endsWith (".tonecheck.json")) continue;
        DbEntry e; e.profile = juce::JSON::parse (f.loadFileAsString()); e.file = f.getFileName();
        const auto schema = e.profile.getProperty ("schema", "").toString();
        if (! schema.startsWith ("ej_comp_profile") && ! schema.startsWith ("ej_eq_profile")) continue;   // the two categories the wizard measures
        const auto tc = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tonecheck.json"); if (tc.existsAsFile()) e.tonecheck = juce::JSON::parse (tc.loadFileAsString());
        db.push_back (e);
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
        if (other == nullptr) other = &e;
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
struct Readback { bool pass = false; int checked = 0; juce::String why; };
inline juce::String normKey (double n) { return juce::String (std::round (n * 10000.0) / 10000.0, 4); }
inline juce::String textNorm (const juce::String& s) { return s.trim().removeCharacters (" ").toLowerCase(); }
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
        if (w.expect.isNotEmpty() && textNorm (t->second) != textNorm (w.expect)) { r.why = "'" + w.control + "' at " + normKey (w.norm) + " shows '" + t->second + "', the profile says '" + w.expect + "'"; return r; }
        ++r.checked;
    }
    r.pass = r.checked > 0; if (! r.pass) r.why = "the profile writes nothing to check"; else r.why = juce::String (r.checked) + " write(s) read back as the profile says";
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
    if (outcome == "ok" && certState == "exported") return "measured";
    return "not_measurable";
}
inline juce::String summaryLine (const std::map<juce::String, int>& n)
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
    if (get ("needs_hardware")) parts.add (plural (get ("needs_hardware"), "needs its hardware (UAD) connected", "need their hardware (UAD) connected"));
    if (get ("silent")) parts.add (plural (get ("silent"), "passes no sound", "pass no sound"));
    if (get ("crashed")) parts.add (plural (get ("crashed"), "crashed (we moved on)", "crashed (we moved on)"));
    if (get ("wont_load")) parts.add (plural (get ("wont_load"), "won't load on this Mac", "won't load on this Mac"));
    if (get ("not_in_scope")) parts.add (plural (get ("not_in_scope"), "is not measured by the wizard yet", "are not measured by the wizard yet"));
    if (get ("not_measurable")) parts.add (plural (get ("not_measurable"), "could not be measured", "could not be measured"));
    return parts.isEmpty() ? juce::String ("nothing to do") : parts.joinIntoString (", ");
}

} // namespace ejmap::wizard
