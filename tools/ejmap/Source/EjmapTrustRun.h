/*
  EjmapTrustRun.h - the two commands over data already on disk (Kathy, 16 Oct, "SORT ALL THREE"). Nothing is loaded, nothing is sent,
  nothing is written outside --out.

    ejmap --cert-v1 --cert <cert folder> [--out <file.json>]
        every draft in <cert>/phaseb/<eq|limiter|delay|reverb|saturation|gate>/drafts judged by EjmapCertV1.h; a table per category.

    ejmap --trust-levels --maps <maps folder> --cert <folder>... [--wizard <engine folder>]... [--certified <PUBLISH_LIST.txt>]
                         [--held <outcomes.json | manifest.json>]... [--quarantine <quarantine.json>] [--human <answers.json>] [--review-first <category>] --out <folder>
        THE CREDIT: every {norm, display} reading in the measuring runs' records (fixtures' displayAt, Phase B positions), judged
        against each map's own anchors (EjmapTrust.h creditMap); the measured roles (roles_by_measurement: confirmed / found on another
        control; dropped = refuted) and each fixture's picked threshold. THE LEVEL per map (levelFor). Writes <out>/trust_levels.json,
        <out>/maps/<fp>.json (each map with its credited per-control "readback"), and prints the table per category.
*/

#pragma once

#include "EjmapCertV1.h"
#include "EjmapMultibandV1.h"
#include <iostream>

namespace ejmap::trustrun
{

inline juce::var readJson (const juce::File& f) { return juce::JSON::parse (f.loadFileAsString()); }

inline std::vector<certv1::Result> judgeFolder (const juce::File& cert)
{
    std::vector<certv1::Result> out;
    {   // MULTIBAND v1 (16 Oct): the draft + its v1 measuring record, by plugin_id; certified per band (mbv1::judge)
        std::map<juce::String, juce::var> v1; for (const auto& f : cert.getChildFile ("phaseb").getChildFile ("multiband").getChildFile ("v1").findChildFiles (juce::File::findFiles, false, "*.multiband_v1.json"))
        { const auto r = readJson (f); v1[r.getProperty ("plugin_id", "").toString()] = r; }
        for (const auto& f : cert.getChildFile ("phaseb").getChildFile ("multiband").getChildFile ("drafts").findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            const auto d = readJson (f); if (! d.isObject()) continue;
            const auto pid = d.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString();
            const auto m = mbv1::judge (d, v1.count (pid) ? v1[pid] : juce::var());
            certv1::Result r; r.category = "multiband"; r.product = m.product; r.pluginId = m.pluginId; r.pass = m.pass; r.format = trust::formatOfRecord (d, f.getFileName());
            for (const auto& b : m.bands) for (const auto& p : b.parts) r.parts.push_back ({ "band " + juce::String (b.band) + ": " + p.name, p.state, p.why });
            for (const auto& b : m.bands) r.parts.push_back ({ "band " + juce::String (b.band), b.pass ? "pass" : (mbv1::passesSoFar (b) ? "not_tested" : "fail"), b.pass ? juce::String() : juce::String (mbv1::passesSoFar (b) ? "passes so far; the v1 measuring decides" : "a part failed") });
            out.push_back (r);
        }
    }
    std::map<juce::String, juce::var> dsRecords;   // 27 Oct: an older de-esser draft is judged with its measuring record (by identity)
    for (const auto& f : cert.getChildFile ("phaseb").getChildFile ("deesser").getChildFile ("deesser").findChildFiles (juce::File::findFiles, false, "*.deesser.json"))
    { const auto r = readJson (f); if (r.isObject()) dsRecords[r.getProperty ("identity", "").toString()] = r; }
    for (const auto& cat : certv1::v1Categories())
        for (const auto& f : cert.getChildFile ("phaseb").getChildFile (cat).getChildFile ("drafts").findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            if (cat == "limiter" && ! f.getFileName().contains ("limiter_ceiling")) continue;
            const auto d = readJson (f); if (! d.isObject()) continue;
            const auto pid = d.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString();
            auto r = certv1::judge (cat, d, cat == "deesser" && dsRecords.count (pid) ? dsRecords[pid] : juce::var()); r.format = trust::formatOfRecord (d, f.getFileName()); out.push_back (r);
        }
    return out;
}
inline juce::String table (const std::map<juce::String, std::map<juce::String, int>>& counts, const juce::StringArray& cols)
{
    juce::String t = "category | " + cols.joinIntoString (" | ") + "\n";
    for (const auto& [cat, c] : counts) { t << cat; for (const auto& k : cols) t << " | " << (c.count (k) ? c.at (k) : 0); t << "\n"; }
    return t;
}
inline int runCertV1 (const juce::File& cert, const juce::File& outFile)
{
    const auto rs = judgeFolder (cert);
    std::map<juce::String, std::map<juce::String, int>> counts; juce::Array<juce::var> rows;
    for (const auto& r : rs)
    {
        ++counts[r.category]["drafts"]; ++counts[r.category][r.pass ? "pass" : "fail"];
        auto* o = new juce::DynamicObject(); o->setProperty ("category", r.category); o->setProperty ("product", r.product); o->setProperty ("plugin_id", r.pluginId);
        o->setProperty ("pass", r.pass); o->setProperty ("why", r.why());
        juce::Array<juce::var> cp; for (const auto& x : r.certifiedParts()) cp.add (x); o->setProperty ("certified_parts", juce::var (cp)); rows.add (juce::var (o));
    }
    std::cout << "CERT-V1: " << cert.getFullPathName() << " (drafts judged, nothing loaded)\n" << table (counts, { "drafts", "pass", "fail" });
    for (const auto& cat : certv1::v1Categories()) std::cout << "  " << cat << " - waits for the listening session: " << certv1::feelWaits (cat) << "\n";
    if (outFile != juce::File()) { outFile.getParentDirectory().createDirectory(); outFile.replaceWithText (juce::JSON::toString (juce::var (rows))); }
    return 0;
}

// metas: one per reading source (the recency rule); 23 Oct: per format, when a filter measurement last found a cutoff and when an EQ
// measurement last measured a band (the newer decides filter or EQ), and the formats a filter / pitch-shift draft certified in
struct ProductData { std::vector<std::pair<juce::String, trust::Readings>> readings; std::vector<trust::SourceMeta> metas; trust::Measured measured;
                     std::map<juce::String, juce::int64> filterAt, eqBandAt; std::set<juce::String> filterCert, shiftCert; };
inline void loadRecords (const juce::File& root, const juce::String& tag, std::map<juce::String, ProductData>& by)
{
    auto take = [&] (const juce::File& f, const juce::String& category)
    {
        const auto rec = readJson (f); if (! rec.isObject()) return;
        const auto product = rec.getProperty ("product", rec.getProperty ("plugin", {}).getProperty ("name", "")).toString(); if (product.isEmpty()) return;
        auto& pd = by[product];
        auto rs = trust::readingsOfRecord (rec); if (! rs.empty()) { pd.readings.push_back ({ tag, std::move (rs) }); pd.metas.push_back (trust::metaOfRecord (rec, f.getFileName())); }
        if (const auto* rm = rec.getProperty ("roles_by_measurement", {}).getArray())
            for (const auto& x : *rm)
            {
                const auto verdict = x.getProperty ("verdict", "").toString(), ctl = x.getProperty ("control", "").toString();
                const auto role = trust::ruleRoleOfMeasured (x.getProperty ("role", "").toString(), category);
                // only "confirmed": a measured_unnamed control is reported by the record's own rule, never swept as the role
                if (verdict == "confirmed") pd.measured.roles[role].addIfNotAlreadyThere (ctl);
                else if (verdict == "dropped") pd.measured.dropped.insert (ctl);
            }
        // A DRAFT's chosen controls are measured roles where that part measured cleanly (16 Oct): the control the space / saturation /
        // gate / EQ / limiter draft adopted for the job, by name or by measurement
        {
            const auto schema = rec.getProperty ("schema", "").toString();
            auto adopt = [&] (const char* part, const char* role, bool okPart) { const auto b = rec.getProperty (part, {}); const auto c = b.getProperty ("control", "").toString(); if (okPart && b.isObject() && c.isNotEmpty()) pd.measured.roles[role].addIfNotAlreadyThere (c); };
            auto measuredVerdict = [&] (const char* part) { return rec.getProperty (part, {}).getProperty ("verdict", "").toString() == "measured"; };
            if (schema == "ej_space_profile/1")
            {
                adopt ("decay", "decay", measuredVerdict ("decay")); adopt ("predelay", "predelay", measuredVerdict ("predelay"));
                adopt ("time", "delay_time", measuredVerdict ("time")); adopt ("feedback", "feedback", measuredVerdict ("feedback"));
                const auto mx = rec.getProperty ("mix", {}); adopt ("mix", "mix", measuredVerdict ("mix") && (double) mx.getProperty ("worst_db", 99.0) <= 1.0);
            }
            if (schema == "ej_saturation_profile/1") adopt ("drive", "drive", rec.getProperty ("drive", {}).getProperty ("verdict", "").toString() == "drive");
            if (schema == "ej_gate_profile/1") adopt ("threshold", "threshold", rec.getProperty ("verdict", "").toString() == "measured");
            if (rec.getProperty ("ceiling", {}).isObject()) adopt ("ceiling", "ceiling", (bool) rec.getProperty ("ceiling", {}).getProperty ("holds_true_peak", false));
            // 23 Oct (Kathy): THE FILTER AND PITCH-SHIFT DRAFTS (EjmapTrust.h 2b) - their identified cutoff / shift controls are measured roles,
            // and the format each certified in is kept (certified only in the format it was measured in, as every category)
            if (schema == "ej_filter_profile/1")
            {
                const auto j = trust::judgeFilterDraft (rec); const auto fmt = trust::formatOfRecord (rec, f.getFileName());
                for (const auto& c : j.identified) pd.measured.roles["cutoff"].addIfNotAlreadyThere (c);
                if (! j.identified.isEmpty()) pd.filterAt[fmt] = std::max (pd.filterAt[fmt], trust::metaOfRecord (rec, f.getFileName()).atMs);
                if (j.certified) pd.filterCert.insert (fmt);
            }
            if (schema == "ej_pitch_profile/1" && (rec.getProperty ("pitch_measurement", "").toString() == "shift" || rec.getProperty ("measurement", "").toString() == "shift"))   // 27 Oct: the renamed key, and drafts on disk from before
            {
                const auto j = trust::judgeShiftDraft (rec); const auto fmt = trust::formatOfRecord (rec, f.getFileName());
                pd.measured.pitchShift = true;
                for (const auto& c : j.identified) pd.measured.roles["shift"].addIfNotAlreadyThere (c);
                if (j.certified) pd.shiftCert.insert (fmt);
            }
            if (schema == "ej_eq_profile/1")
                if (const auto* bs = rec.getProperty ("bands", {}).getArray())
                    for (const auto& b : *bs)
                        if (b.getProperty ("verdict", "").toString() == "measured")
                        {
                            { const auto fm = trust::formatOfRecord (rec, f.getFileName()); pd.eqBandAt[fm] = std::max (pd.eqBandAt.count (fm) ? pd.eqBandAt[fm] : (juce::int64) -1, trust::metaOfRecord (rec, f.getFileName()).atMs); }
                            const auto g = b.getProperty ("gain_control", "").toString(), f = b.getProperty ("freq_control", "").toString();
                            if (g.isNotEmpty() && g != "(none)") pd.measured.roles["gain"].addIfNotAlreadyThere (g);
                            if (f.isNotEmpty() && f != "(none)") pd.measured.roles["freq"].addIfNotAlreadyThere (f);
                        }
        }
        if (rec.getProperty ("schema", "").toString() == "ej_comp_profile/1")   // a certification profile: its measured amount control
            if (const auto amt = rec.getProperty ("amount", {}).getProperty ("control", "").toString(); amt.isNotEmpty()) pd.measured.roles["threshold"].addIfNotAlreadyThere (amt);
        const auto pick = rec.getProperty ("pickedCandidate", {});   // a compressor fixture: the threshold its sweep certified
        if (pick.isObject()) { const auto n = pick.getProperty ("name", pick.getProperty ("control", "")).toString(); if (n.isNotEmpty()) pd.measured.roles["threshold"].addIfNotAlreadyThere (n); }
    };
    for (const auto& f : root.getChildFile ("fixtures").findChildFiles (juce::File::findFiles, false, "*.json")) if (! f.getFileName().endsWith (".defaults.json")) take (f, "compressor");
    for (const auto& f : root.getChildFile ("profiles").findChildFiles (juce::File::findFiles, false, "*.json")) if (! f.getFileName().endsWith (".tonecheck.json")) take (f, "compressor");
    for (const auto& catDir : root.getChildFile ("phaseb").findChildFiles (juce::File::findDirectories, false))
        for (const auto& sub : catDir.findChildFiles (juce::File::findDirectories, false))
        {
            if (sub.getFileName() == "logs" || sub.getFileName() == "raw") continue;
            for (const auto& f : sub.findChildFiles (juce::File::findFiles, false, "*.json")) take (f, catDir.getFileName());
        }
}
inline std::map<juce::String, juce::String> loadHeld (const juce::StringArray& heldFiles, const juce::File& quarantine)
{
    std::map<juce::String, juce::String> held;   // product (lower case) -> why
    static const juce::StringArray kHeld { "needs_licence", "needs_hardware", "refused", "needs_review", "held", "quarantined_at_scan", "surround", "crashed", "wont_load", "demo", "silent", "custom_default",
                                         "multiband" };   // 16 Oct: multiband profiling is not built - a band threshold is not a compressor's dial
    for (const auto& path : heldFiles)
    {
        const auto v = readJson (juce::File (path));
        auto rows = v.isArray() ? v : v.getProperty ("skipped", {});
        if (const auto* a = rows.getArray())
            for (const auto& x : *a)
            {
                const auto st = x.getProperty ("state", "").toString(), why = x.getProperty ("why", x.getProperty ("reason", "")).toString();
                if (! kHeld.contains (st) || why.contains ("never processes")) continue;   // 16 Oct: the old sound check's false "never processes" is not a hold
                held[x.getProperty ("product", "").toString().toLowerCase()] = st;
            }
    }
    const auto qroot = readJson (quarantine);
    if (const auto* q = qroot.getArray())
        for (const auto& x : *q) { const auto id = x.getProperty ("plugin_id", "").toString(); held["@quarantine:" + id.toLowerCase()] = "quarantined (" + x.getProperty ("reason", "").toString() + ")"; }
    return held;
}
// A LATER PASSING MEASUREMENT CLEARS AN EARLIER HOLD (Kathy, 26 Oct, SPL IRON; like the read-back recency rule): the same plugin version and
// format (the profile's plugin_id is the hold's identity), a tone check dated after the hold that passes (within 0.5 dB, the quiet check
// ok), under the SAME conditions as the failed one (the tone check's spec, target g, reference level within 0.1 dB, map fp). The failed
// check must be on record (its file beside the hold's outcomes), and the hold stays on the row as earlier_hold
struct HoldClear { bool cleared = false; juce::String why; };
inline juce::int64 stampMs (const juce::String& s)
{
    auto t = s.trim(); if (t.isEmpty()) return 0;
    if (t.length() >= 15 && t[8] == 'T' && t.substring (0, 8).containsOnly ("0123456789"))   // 20261012T202902[.278][+0100|Z]
        t = t.substring (0, 4) + "-" + t.substring (4, 6) + "-" + t.substring (6, 8) + "T" + t.substring (9, 11) + ":" + t.substring (11, 13) + ":" + t.substring (13);
    if (t.length() >= 5 && (t.endsWithChar ('+') == false) && (t.fromLastOccurrenceOf ("+", false, false).length() == 4)) t = t.dropLastCharacters (2) + ":" + t.getLastCharacters (2);
    return juce::Time::fromISO8601 (t).toMilliseconds();
}
inline HoldClear holdCleared (const juce::var& holdRow, const juce::var& oldTone, const juce::var& newProfile, const juce::var& newTone)
{
    HoldClear h;
    const auto id = holdRow.getProperty ("identity", "").toString(), newId = newProfile.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString();
    if (id.isEmpty() || newId != id) { h.why = "another plugin version or format (" + newId + " vs " + id + ")"; return h; }
    if (! oldTone.isObject()) { h.why = "the failed tone check is not on record"; return h; }
    const auto heldAt = stampMs (holdRow.getProperty ("at", "").toString()), newAt = stampMs (newTone.getProperty ("measuredAt", "").toString());
    if (heldAt <= 0 || newAt <= heldAt) { h.why = "the tone check is not later than the hold"; return h; }
    if (! (bool) newTone.getProperty ("pass_within_0_5_db", false) || ! (bool) newTone.getProperty ("quiet_check_ok", false)) { h.why = "the later tone check does not pass"; return h; }
    if (newTone.getProperty ("spec", "").toString() != oldTone.getProperty ("spec", "x").toString()) { h.why = "another tone-check spec"; return h; }
    if (std::abs ((double) newTone.getProperty ("g_db", -1.0) - (double) oldTone.getProperty ("g_db", -2.0)) > 1e-6) { h.why = "another target g"; return h; }
    if (std::abs ((double) newTone.getProperty ("L_ref_dbfs", 0.0) - (double) oldTone.getProperty ("L_ref_dbfs", 99.0)) > 0.1) { h.why = "another reference level"; return h; }
    const auto fa = oldTone.getProperty ("map_fp", "").toString(), fb = newTone.getProperty ("map_fp", "").toString();
    if (fa.isNotEmpty() && fb.isNotEmpty() && fa != fb) { h.why = "another map"; return h; }
    h.cleared = true;
    h.why = "a later tone check passed under the same conditions (" + newTone.getProperty ("spec", "").toString() + ", g " + juce::String ((double) newTone.getProperty ("g_db", 0.0), 1)
            + " dB, L " + juce::String ((double) newTone.getProperty ("L_ref_dbfs", 0.0), 2) + " dBFS, measured " + juce::String ((double) newTone.getProperty ("gr_measured_db", 0.0), 2) + " dB) on "
            + newTone.getProperty ("measuredAt", "").toString();
    return h;
}
// every hold row with a tone check (the held outcomes files) that a cert folder's later profile clears: product (lower case) -> the old row
// + cleared_by (the identity and the why)
inline std::map<juce::String, juce::var> clearedHolds (const juce::StringArray& heldFiles, const juce::StringArray& certDirs)
{
    std::map<juce::String, juce::var> out;
    for (const auto& path : heldFiles)
    {
        const juce::File hf (path); const auto v = readJson (hf);
        const auto rows = v.isArray() ? v : v.getProperty ("skipped", {});
        if (const auto* a = rows.getArray())
            for (const auto& x : *a)
            {
                const auto tc = x.getProperty ("tonecheck", "").toString(); if (tc.isEmpty()) continue;
                const auto oldTone = readJson (hf.getSiblingFile ("profiles").getChildFile (juce::File (tc).getFileName()));
                for (const auto& c : certDirs)
                    for (const auto& tf : juce::File (c).getChildFile ("profiles").findChildFiles (juce::File::findFiles, false, "*.tonecheck.json"))
                    {
                        const auto newTone = readJson (tf); if (newTone.getProperty ("product", "").toString() != x.getProperty ("product", "").toString()) continue;
                        const auto prof = readJson (tf.getSiblingFile (tf.getFileName().replace (".tonecheck.json", ".json")));
                        const auto h = holdCleared (x, oldTone, prof, newTone); if (! h.cleared) continue;
                        auto* o = new juce::DynamicObject(); for (const char* k : { "state", "reason", "at", "identity" }) o->setProperty (k, x.getProperty (k, {}));
                        o->setProperty ("cleared_by", juce::File (c).getFileName() + ": " + h.why);
                        out[x.getProperty ("product", "").toString().toLowerCase()] = juce::var (o);
                    }
            }
    }
    return out;
}
inline juce::String heldWhyFor (const std::map<juce::String, juce::String>& held, const juce::String& name)
{
    const auto lower = name.toLowerCase();
    if (const auto it = held.find (lower); it != held.end()) return it->second;
    for (const auto& [k, why] : held) if (k.startsWith ("@quarantine:") && k.containsIgnoreCase ("/" + lower + ".")) return why;
    return {};
}
inline std::set<juce::String> certifiedFromList (const juce::File& publishList)
{
    std::set<juce::String> s;
    for (const auto& l : juce::StringArray::fromLines (publishList.loadFileAsString()))
        if (l.contains (" | ") && ! l.startsWith ("product")) s.insert (l.upToFirstOccurrenceOf (" | ", false, false).trim());
    return s;
}

// THE HUMAN CHECK (Kathy, 16 Oct): answers from the review page - {"answers": [{"id": "<fp>|<control>", "fp", "control", "role",
// "verdict": "yes" | "no", "at", "by"}]}. A yes identifies the control for that role on that map (human-checked); a no refutes it (the
// name rule never claims it back). The read-back is still required for "mapped"
// verdicts (16 Oct, the role dropdown): "yes" = the suggested role in the row's section; "role" = the role and section the person picked;
// "no" = not the suggestion (no role given); "other" = not a main control; "unsure" = counts neither way (measuring / ask Sean)
struct HumanAnswer { juce::String control, role, section, verdict; bool yes = false; };
inline std::map<juce::String, std::vector<HumanAnswer>> loadHuman (const juce::File& f)
{
    std::map<juce::String, std::vector<HumanAnswer>> out;
    const auto root = readJson (f);   // held by name: a list read out of a temporary would be gone before it is read
    if (const auto* a = root.getProperty ("answers", {}).getArray())
        for (const auto& x : *a)
        {
            const auto v = x.getProperty ("verdict", "").toString();
            if (v != "yes" && v != "no" && v != "role" && v != "other") continue;   // unsure counts neither way
            out[x.getProperty ("fp", "").toString()].push_back ({ x.getProperty ("control", "").toString(), x.getProperty ("role", "").toString(),
                                                               x.getProperty ("section", "").toString(), v, v == "yes" || v == "role" });
        }
    return out;
}
inline trust::Measured withHuman (trust::Measured m, const std::vector<HumanAnswer>& answers)
{
    for (const auto& h : answers)
    {
        if (h.yes && h.role.isNotEmpty() && h.role != "other")
        {   // the role (and the section the person put it in: "Gate" -> "gate", "Band 2" -> "band 2", "" = the plugin's own)
            for (auto& [r, n] : m.roles) if (r != h.role) n.removeString (h.control);
            m.roles[h.role].addIfNotAlreadyThere (h.control); m.dropped.erase (h.control);
            m.section[h.control] = trust::knownSection (h.section.toLowerCase());
        }
        else { m.dropped.insert (h.control); for (auto& [r, n] : m.roles) n.removeString (h.control); }
    }
    return m;
}
// THE REVIEW LIST: for a map left unsure by "role not identified", every control a human could settle - a name that matches a needed
// role with the wrong or no unit, or one of two candidates for it. Each item says whether a yes alone would make the map mapped
inline juce::Array<juce::var> reviewItems (const juce::var& m, const trust::Measured& measured, const std::map<juce::String, trust::ControlReadback>& rb)
{
    juce::Array<juce::var> items;
    const auto cat = m.getProperty ("category", "").toString(); const auto fp = m.getProperty ("fp", "").toString();
    const auto* co = m.getProperty ("controls", {}).getDynamicObject(); if (co == nullptr) return items;
    juce::StringArray needRoles; for (const auto& need : trust::needsFor (cat, measured.pitchShift)) for (const auto& alt : juce::StringArray::fromTokens (need, "|", "")) for (const auto& r : juce::StringArray::fromTokens (alt, "+", "")) needRoles.addIfNotAlreadyThere (r);
    const auto R = trust::rolesOf (m, measured);
    juce::Array<juce::var> pluginSections;   // every named section this plugin's controls carry, for the role dropdown
    { juce::StringArray seen; for (const auto& p : co->getProperties()) { const auto l = trust::sectionLabel (trust::knownSection (trust::sectionOf (p.value.getProperty ("name", p.name.toString()).toString())));
          if (l.isNotEmpty() && seen.addIfNotAlreadyThere (l)) pluginSections.add (l); } }
    for (const auto& p : co->getProperties())
    {
        const auto& c = p.value; const auto name = c.getProperty ("name", p.name.toString()).toString();
        if (measured.dropped.count (name)) continue;
        const auto hit = trust::roleOf (c); if (hit.role.isEmpty() || ! needRoles.contains (hit.role)) continue;
        juce::String why;
        if (! hit.unitOk) why = "the name says " + hit.role + "; the display shows " + (trust::unitOf (c).isEmpty() ? juce::String ("no unit") : "'" + trust::unitOf (c) + "'");
        else { bool amb = false; for (const auto& a : R.ambiguous) if (a.startsWith (hit.role + ":") && a.contains (name)) amb = true;
               const auto it = R.identified.find (hit.role); if (it != R.identified.end() && it->second.size() > 1) amb = true;
               if (! amb) continue; why = "one of several " + hit.role + " candidates"; }
        auto yes = measured; yes.roles[hit.role].clear(); yes.roles[hit.role].add (name);
        const auto L = trust::levelFor (m, false, "", rb, yes);
        const auto rit = rb.find (name);
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", fp + "|" + name); o->setProperty ("fp", fp); o->setProperty ("plugin", m.getProperty ("identity", {}).getProperty ("name", ""));
        o->setProperty ("vendor", m.getProperty ("identity", {}).getProperty ("vendor", "")); o->setProperty ("version", m.getProperty ("identity", {}).getProperty ("version", ""));
        o->setProperty ("category", cat); o->setProperty ("control", name); o->setProperty ("unit", trust::unitOf (c).isEmpty() ? juce::String ("-") : trust::unitOf (c));
        o->setProperty ("suggested_role", hit.role); o->setProperty ("why", why);
        // the section the control's name puts the role in (Kathy, 16 Oct): the question is "Gate -> Threshold", never a bare "Threshold"
        o->setProperty ("section", trust::sectionLabel (trust::knownSection (trust::sectionOf (name))));
        o->setProperty ("plugin_sections", juce::var (pluginSections));
        o->setProperty ("read_back", rit != rb.end() && rit->second.proven());
        o->setProperty ("yes_makes_mapped", L.level == "mapped");
        items.add (juce::var (o));
    }
    // A PAIR THAT COUNTS (17 Oct: an EQ needs a band's freq AND gain, so no single yes maps it): two rows of different roles whose yeses
    // together make the map "mapped" name each other in counts_with; the page counts the map once both are yes
    for (int i = 0; i < items.size(); ++i)
        for (int j = i + 1; j < items.size(); ++j)
        {
            auto& a = items.getReference (i); auto& b = items.getReference (j);
            if ((bool) a.getProperty ("yes_makes_mapped", false) || (bool) b.getProperty ("yes_makes_mapped", false) || a.hasProperty ("counts_with") || b.hasProperty ("counts_with")) continue;
            const auto ra = a.getProperty ("suggested_role", "").toString(), rb2 = b.getProperty ("suggested_role", "").toString(); if (ra == rb2) continue;
            auto both = measured; both.roles[ra].clear(); both.roles[ra].add (a.getProperty ("control", "").toString()); both.roles[rb2].clear(); both.roles[rb2].add (b.getProperty ("control", "").toString());
            if (trust::levelFor (m, false, "", rb, both).level != "mapped") continue;
            a.getDynamicObject()->setProperty ("counts_with", b.getProperty ("id", "")); b.getDynamicObject()->setProperty ("counts_with", a.getProperty ("id", ""));
        }
    return items;
}

// THE BRIDGE'S CONTROLS AND SETTINGS (17 Oct, "ALL DAWs"): the AU map's role controls are the ones that meet its category's needs (the
// same rule the level uses); the bridge reads each at five settings across its range (plus the read-back's own 25/50/75 %)
inline juce::StringArray roleControlsOf (const juce::var& map, const trust::Measured& measured)
{
    const auto cat = map.getProperty ("category", "").toString(); const auto R = trust::rolesOf (map, measured);
    juce::StringArray out;
    for (const auto& need : trust::needsFor (cat, measured.pitchShift)) { const auto c = trust::meetNeed (need, R, cat); if (c.isEmpty()) return {}; for (const auto& n : c) out.addIfNotAlreadyThere (n); }
    return out;
}
inline std::vector<double> bridgeNorms() { return { 0.1, 0.3, 0.5, 0.7, 0.9 }; }

// CERTIFICATION BY FORMAT (Kathy, 17 Oct, rulings 2): certification counts only for the format it was measured in. The publish list
// names products; the published profiles (--certified-profiles: the folder the publish zip unpacks to) say each one's format. A listed
// product with no profile here, or one whose profile does not state its format, certifies nothing (said)
using FormatSet = std::set<std::pair<juce::String, juce::String>>;   // (product, format)
inline FormatSet certifiedByFormat (const std::set<juce::String>& listed, const juce::File& profilesDir, juce::StringArray& unstated)
{
    FormatSet out; std::set<juce::String> seen;
    for (const auto& f : profilesDir.findChildFiles (juce::File::findFiles, true, "*.json"))
    {
        if (f.getFileName().endsWith (".tonecheck.json")) continue;
        const auto rec = readJson (f); const auto name = rec.getProperty ("plugin", {}).getProperty ("name", rec.getProperty ("product", "")).toString();
        if (! listed.count (name)) continue;
        const auto fmt = trust::formatOfRecord (rec, f.getFileName()); if (fmt.isEmpty()) continue;
        out.insert ({ name, fmt }); seen.insert (name);
    }
    for (const auto& n : listed) if (! seen.count (n)) unstated.add (n);
    return out;
}

inline int runTrustLevels (const juce::File& mapsDir, const juce::StringArray& certDirs, const juce::StringArray& wizardDirs, const juce::File& publishList,
                           const juce::StringArray& heldFiles, const juce::File& quarantine, const juce::File& out, const juce::File& humanFile = {}, const juce::String& firstCategory = {},
                           const juce::File& certifiedProfiles = {})
{
    if (! mapsDir.isDirectory() || out == juce::File()) { std::cerr << "usage: ejmap --trust-levels --maps <folder> --cert <folder>... --out <folder>" << std::endl; return 2; }
    std::map<juce::String, ProductData> by;
    for (const auto& c : certDirs) loadRecords (juce::File (c), "cert:" + juce::File (c).getFileName(), by);
    for (const auto& w : wizardDirs) loadRecords (juce::File (w), "wizard", by);
    const auto held = loadHeld (heldFiles, quarantine);
    const auto cleared = clearedHolds (heldFiles, certDirs);   // 26 Oct: a later passing measurement clears an earlier hold (holdCleared)
    juce::StringArray listedUnstated;
    const auto compCert = certifiedByFormat (certifiedFromList (publishList), certifiedProfiles, listedUnstated);
    std::map<juce::String, FormatSet> v1Pass;   // category -> (product, format): the format each draft was measured in
    std::map<std::pair<juce::String, juce::String>, std::vector<certv1::Part>> v1Parts;   // 27 Oct: (product, format) -> its parts, for the part-by-part categories
    for (const auto& c : certDirs) for (const auto& r : judgeFolder (juce::File (c))) if (r.pass)
    {   v1Pass[certv1::mapCategoryOf (r.category)].insert ({ r.product, r.format });   // 27 Oct: "transient" certifies transient_shaper maps
        if (certv1::partByPart (certv1::mapCategoryOf (r.category))) v1Parts[{ r.product, r.format }] = certv1::mergeParts (v1Parts[{ r.product, r.format }], r.parts); }
    if (! listedUnstated.isEmpty()) std::cout << "TRUST-LEVELS: " << listedUnstated.size() << " published product(s) with no profile stating its format here - certified for no format: " << listedUnstated.joinIntoString (", ") << "\n";
    out.getChildFile ("maps").createDirectory();
    const auto human = loadHuman (humanFile); juce::Array<juce::var> review;
    std::map<juce::String, std::map<juce::String, int>> counts; juce::Array<juce::var> rows; int creditedControls = 0, failedControls = 0, supersededReadings = 0, supersededControls = 0, otherFormat = 0, snappedReadings = 0;
    struct Entry { juce::var m; juce::String file, name, cat, fmt, version, heldWhy, bridgedFrom, bridgeWhy; trust::Level L; trust::Measured measured; const ProductData* pd = nullptr; trust::Bridge bridge; std::vector<certv1::Part> parts; };
    std::vector<Entry> entries;
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        auto m = readJson (f); if (! m.isObject()) continue;
        const auto name = m.getProperty ("identity", {}).getProperty ("name", "").toString(); auto cat = m.getProperty ("category", "").toString();
        static const ProductData none;
        const auto it = by.find (name); const auto& pd = it != by.end() ? it->second : none;
        // 23 Oct (Kathy): FILTER IS A CATEGORY - a map filed as an EQ (or with none) whose plugin a filter measurement found a cutoff in, in
        // this format, more recently than any EQ measurement measured a band there (the wizard's newer routing decides), is counted as a
        // filter (the map store is never written: this is the trust run's copy)
        const auto mfmt = m.getProperty ("identity", {}).getProperty ("format", "").toString();
        if ((cat == "eq" || cat.isEmpty() || cat == "filter") && pd.filterAt.count (mfmt) && (! pd.eqBandAt.count (mfmt) || pd.filterAt.at (mfmt) > pd.eqBandAt.at (mfmt)))
        {
            if (cat != "filter") m.getDynamicObject()->setProperty ("category_from", "a filter measurement (filed as '" + cat + "')");
            cat = "filter"; m.getDynamicObject()->setProperty ("category", cat);
        }
        const auto rb = trust::creditMap (m, pd.readings, pd.metas, &otherFormat);
        for (const auto& [ctl, r] : rb) { if (r.superseded > 0) { supersededReadings += r.superseded; ++supersededControls; } snappedReadings += r.snapped; }
        const auto fpKey = m.getProperty ("fp", "").toString();
        const auto measured = trust::deesserDepths (human.count (fpKey) ? withHuman (pd.measured, human.at (fpKey)) : pd.measured, cat);   // 26 Oct
        const auto fmt = m.getProperty ("identity", {}).getProperty ("format", "").toString();
        const std::pair<juce::String, juce::String> key { name, fmt };   // 17 Oct (rulings 2): certified only in the format it was measured in
        const bool certified = (cat == "compressor" && compCert.count (key)) || (v1Pass.count (cat) && v1Pass[cat].count (key)) || (cat == "amp_sim" && v1Pass["saturation"].count (key))
                               || (cat == "compressor" && v1Pass["multiband"].count (key))    // 16 Oct: a multiband passes per band (mbv1)
                               || (cat == "filter" && pd.filterCert.count (fmt)) || (cat == "pitch" && pd.shiftCert.count (fmt));   // 23 Oct: the two drafts' own certification
        auto heldWhy = heldWhyFor (held, name);
        juce::var earlierHold;   // 26 Oct: the cleared hold, kept on the row - only for the map of the hold's own format and version
        if (heldWhy.isNotEmpty())
            if (const auto ch = cleared.find (name.toLowerCase()); ch != cleared.end())
            {   const auto hid = juce::StringArray::fromTokens (ch->second.getProperty ("identity", "").toString(), "|", "");
                if (hid.size() == 3 && hid[0] == fmt && trust::versionKey (hid[2]) == trust::versionKey (m.getProperty ("identity", {}).getProperty ("version", "").toString()))
                { heldWhy = {}; earlierHold = ch->second; m.getDynamicObject()->setProperty ("earlier_hold", earlierHold); } }
        auto L = trust::levelFor (m, certified, heldWhy, rb, measured);
        if (L.level == "certified" && (cat == "filter" || cat == "pitch") && ! compCert.count (key) && ! (v1Pass.count (cat) && v1Pass[cat].count (key)))   // 23 Oct: say which
            L.why = cat == "filter" ? "passed the filter measurement's certification: every cutoff matches its labels under one convention, 3+ judged positions"
                                    : "passed the pitch-shift measurement's certification: every judged position within 5 cents, 3+ positions";
        if (L.level == "unsure" && L.why.startsWith ("role not identified")) review.addArray (reviewItems (m, measured, rb));
        // the credited copy: each judged control carries its read-back, as the sweep writes it. 18 Oct (B): a params-format map (CLA-76,
        // PuigChild 670) keeps its role controls in params - they were credited and levelled but their read-back was never written there
        for (const char* section : { "controls", "params" })
        if (auto* co = m.getProperty (section, {}).getDynamicObject())
            for (const auto& [ctl, r] : rb)
                for (const auto& p : co->getProperties())
                    if (p.value.getProperty ("name", p.name.toString()).toString() == ctl && r.judged > 0)
                        if (auto* cobj = p.value.getDynamicObject())
                        {
                            auto* o = new juce::DynamicObject(); o->setProperty ("checked", r.judged); o->setProperty ("failed", r.failed); o->setProperty ("pass", r.proven());
                            if (r.worst.isNotEmpty()) o->setProperty ("worst", r.worst);
                            o->setProperty ("source", r.source); if (r.superseded > 0) o->setProperty ("superseded", r.superseded); if (r.snapped > 0) o->setProperty ("snapped", r.snapped);
                            if (! r.failedAt.empty()) { juce::Array<juce::var> fa; for (double n : r.failedAt) fa.add (juce::String (n, 6)); o->setProperty ("failed_at", juce::var (fa)); }
                            cobj->setProperty ("readback", juce::var (o));
                            if (juce::String (section) == "controls") { ++creditedControls; if (r.failed > 0) ++failedControls; }   // the printed tally stays per control
                        }
        out.getChildFile ("maps").getChildFile (f.getFileName()).replaceWithText (juce::JSON::toString (m));
        std::vector<certv1::Part> parts;   // 27 Oct (PART BY PART): a certified de-esser / transient shaper lists each part, verified or not
        if (L.level == "certified" && certv1::partByPart (cat) && v1Parts.count (key))
        {   parts = v1Parts.at (key);
            for (auto& p : parts) { const auto it = rb.find (p.control); p.readBack = p.control.isNotEmpty() && it != rb.end() && it->second.proven(); }
            L.why << certv1::partsWhy (parts); }
        entries.push_back ({ m, f.getFileName(), name, cat, fmt, m.getProperty ("identity", {}).getProperty ("version", "").toString(), heldWhy, {}, {}, L, measured, &pd, {}, parts });
    }
    // THE FORMAT BRIDGE (Kathy, 17 Oct, "ALL DAWs"): a VST3 below its AU twin (same product, same version) inherits the twin's certified /
    // mapped level only when trust::bridgeFor finds every role control displaying the same values in both formats at 3+ shared settings.
    // The plan for the guarded run that takes those readings is written beside the levels (bridge_plan.json, bridge_names.txt)
    int bridged = 0, bridgeRefused = 0; auto* plan = new juce::DynamicObject(); juce::StringArray planNames;
    auto rank = [] (const juce::String& l) { return l == "certified" ? 2 : l == "mapped" ? 1 : 0; };
    for (auto& e : entries)
    {
        if (e.fmt != "VST3" || e.heldWhy.isNotEmpty()) continue;
        const Entry* twin = nullptr;
        for (const auto& a : entries)
            if (a.fmt == "AudioUnit" && a.name == e.name && trust::versionKey (a.version) == trust::versionKey (e.version) && (twin == nullptr || rank (a.L.level) > rank (twin->L.level))) twin = &a;
        if (twin == nullptr || rank (twin->L.level) <= rank (e.L.level)) continue;
        const auto roles = roleControlsOf (twin->m, twin->measured);
        e.bridge = trust::bridgeFor (twin->m, e.m, roles, e.pd->readings, e.pd->metas);
        if (e.bridge.ok)
        {
            e.L.level = twin->L.level; e.L.why = "bridged from the AU map " + twin->m.getProperty ("fp", "").toString().substring (0, 12) + ": " + e.bridge.why;
            e.L.dialable = roles; e.L.roles = twin->L.roles; e.bridgedFrom = twin->m.getProperty ("fp", "").toString(); e.parts = twin->parts; e.L.why << certv1::partsWhy (e.parts); ++bridged; continue;
        }
        e.bridgeWhy = e.bridge.why; ++bridgeRefused;
        auto* pc = new juce::DynamicObject();
        for (const auto& [n, c] : trust::mapControls (twin->m))
        {
            if (! roles.contains (n)) continue;
            juce::Array<juce::var> ns;
            if (c.getProperty ("kind", "").toString() == "mode") { if (const auto* lo = c.getProperty ("labels", {}).getDynamicObject()) for (const auto& l : lo->getProperties()) if (ns.size() < 5) ns.add (juce::String ((double) l.value, 6)); }
            else for (double x : bridgeNorms()) ns.add (juce::String (x, 6));
            pc->setProperty (n, juce::var (ns));
        }
        plan->setProperty (e.name, juce::var (pc)); planNames.addIfNotAlreadyThere (e.name);
    }
    for (const auto& e : entries)
    {
        ++counts[e.cat][e.L.level]; ++counts[e.cat]["maps"];
        auto* o = new juce::DynamicObject(); o->setProperty ("fp", e.m.getProperty ("fp", "")); o->setProperty ("product", e.name); o->setProperty ("category", e.cat);
        o->setProperty ("format", e.fmt);   // 17 Oct (NEXT 1): the counts split by format
        o->setProperty ("version", e.version); o->setProperty ("level", e.L.level); o->setProperty ("why", e.L.why);
        if (e.bridgedFrom.isNotEmpty()) o->setProperty ("bridged_from", e.bridgedFrom);
        if (e.m.hasProperty ("earlier_hold")) o->setProperty ("earlier_hold", e.m.getProperty ("earlier_hold", {}));   // 26 Oct: the old failure stays on record
        if (e.bridgeWhy.isNotEmpty()) o->setProperty ("bridge_refused", e.bridgeWhy);
        juce::Array<juce::var> d; for (const auto& x : e.L.dialable) d.add (x); o->setProperty ("dialable", juce::var (d));
        juce::Array<juce::var> rl; for (const auto& [need, c] : e.L.roles) { auto* r = new juce::DynamicObject(); r->setProperty ("semantic", need); r->setProperty ("control", c); rl.add (juce::var (r)); }
        o->setProperty ("roles", juce::var (rl));   // 18 Oct (B): each dialable control with the role it was found for
        if (! e.parts.empty())
        {   juce::Array<juce::var> ps;   // 27 Oct (Kathy, PART BY PART): verified parts dial as certified; the rest as mapped, "not verified yet"
            for (const auto& p : e.parts) { auto* x = new juce::DynamicObject(); x->setProperty ("part", p.name); x->setProperty ("control", p.control); x->setProperty ("verified", p.state == "pass"); x->setProperty ("read_back", p.readBack); x->setProperty ("why", p.why); ps.add (juce::var (x)); }
            o->setProperty ("parts", juce::var (ps)); }
        rows.add (juce::var (o));
    }
    out.getChildFile ("trust_levels.json").replaceWithText (juce::JSON::toString (juce::var (rows)));
    out.getChildFile ("bridge_plan.json").replaceWithText (juce::JSON::toString (juce::var (plan)));
    out.getChildFile ("bridge_names.txt").replaceWithText (planNames.joinIntoString ("\n") + (planNames.isEmpty() ? "" : "\n"));
    std::cout << "TRUST-LEVELS: format bridge - " << bridged << " VST3 map(s) inherit their AU twin's level; " << bridgeRefused
              << " below their twin without a bridge yet (bridge_plan.json lists their role controls for the guarded read-back)\n";
    {   // the review list (schema ejmap_role_review/1), the same file the local page and the admin page read
        auto* rv = new juce::DynamicObject(); rv->setProperty ("schema", "ejmap_role_review/1"); rv->setProperty ("generated_at", juce::Time::getCurrentTime().toISO8601 (true));
        rv->setProperty ("source", "ejmap --trust-levels");
        // --review-first <category> (Kathy, 17 Oct: EQ): the rows where a yes counts now, that category's first - in the file and on the page
        if (firstCategory.isNotEmpty())
        {
            rv->setProperty ("first_category", firstCategory);
            auto tier = [&] (const juce::var& x) { const bool now = (bool) x.getProperty ("yes_makes_mapped", false) || x.hasProperty ("counts_with"); return now ? (x.getProperty ("category", "").toString() == firstCategory ? 0 : 1) : 2; };
            std::vector<juce::var> v; for (const auto& x : review) v.push_back (x);
            std::stable_sort (v.begin(), v.end(), [&] (const juce::var& a, const juce::var& b) { return tier (a) < tier (b); });
            review.clearQuick(); for (const auto& x : v) review.add (x);
        }
        rv->setProperty ("items", juce::var (review));
        out.getChildFile ("review_items.json").replaceWithText (juce::JSON::toString (juce::var (rv)));
        int wouldMap = 0, inPairs = 0; for (const auto& x : review) { if ((bool) x.getProperty ("yes_makes_mapped", false)) ++wouldMap; if (x.hasProperty ("counts_with")) ++inPairs; }
        std::cout << "TRUST-LEVELS: " << review.size() << " control(s) for the human check (" << wouldMap << " where a yes alone makes the map mapped, " << inPairs << " in pairs where two yeses do); "
                  << human.size() << " map(s) with answers applied\n";
    }
    std::cout << "TRUST-LEVELS: " << rows.size() << " map(s); read-back credited on " << creditedControls << " control(s) from the measuring runs ("
              << failedControls << " of them with a failed reading); " << supersededReadings << " older failed reading(s) on " << supersededControls << " control(s) set aside by a newer clean read-back within " << trust::RECENCY_REACH << " of their setting (the recency rule); "
              << snappedReadings << " reading(s) judged at the step the plugin took; " << otherFormat << " reading(s) not used on a map of another (or an unstated) format; nothing loaded, nothing sent\n" << table (counts, { "maps", "certified", "mapped", "unsure" });
    return 0;
}

} // namespace ejmap::trustrun
