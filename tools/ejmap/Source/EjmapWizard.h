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

namespace ejmap::wizard
{

// ---------------------------------------------------------------- the mock database
struct DbEntry { juce::var profile, tonecheck; juce::String file; juce::String uid() const; juce::String version() const; juce::String mapFp() const; };
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
        if (! e.profile.getProperty ("schema", "").toString().startsWith ("ej_comp_profile")) continue;
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
    for (const char* list : { "neutral", "engage" })
        if (const auto* a = profile.getProperty (list, {}).getArray()) for (const auto& x : *a)
            w.push_back ({ x.getProperty ("control", "").toString(), (int) x.getProperty ("index", -1), (double) x.getProperty ("norm", 0.0), x.getProperty ("set", "").toString() });
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
    if (get ("not_measurable")) parts.add (plural (get ("not_measurable"), "could not be measured", "could not be measured"));
    return parts.isEmpty() ? juce::String ("nothing to do") : parts.joinIntoString (", ");
}

} // namespace ejmap::wizard
