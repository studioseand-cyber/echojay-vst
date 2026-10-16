/*
  EjmapTrustRun.h - the two commands over data already on disk (Kathy, 16 Oct, "SORT ALL THREE"). Nothing is loaded, nothing is sent,
  nothing is written outside --out.

    ejmap --cert-v1 --cert <cert folder> [--out <file.json>]
        every draft in <cert>/phaseb/<eq|limiter|delay|reverb|saturation|gate>/drafts judged by EjmapCertV1.h; a table per category.

    ejmap --trust-levels --maps <maps folder> --cert <folder>... [--wizard <engine folder>]... [--certified <PUBLISH_LIST.txt>]
                         [--held <outcomes.json | manifest.json>]... [--quarantine <quarantine.json>] --out <folder>
        THE CREDIT: every {norm, display} reading in the measuring runs' records (fixtures' displayAt, Phase B positions), judged
        against each map's own anchors (EjmapTrust.h creditMap); the measured roles (roles_by_measurement: confirmed / found on another
        control; dropped = refuted) and each fixture's picked threshold. THE LEVEL per map (levelFor). Writes <out>/trust_levels.json,
        <out>/maps/<fp>.json (each map with its credited per-control "readback"), and prints the table per category.
*/

#pragma once

#include "EjmapCertV1.h"
#include <iostream>

namespace ejmap::trustrun
{

inline juce::var readJson (const juce::File& f) { return juce::JSON::parse (f.loadFileAsString()); }

inline std::vector<certv1::Result> judgeFolder (const juce::File& cert)
{
    std::vector<certv1::Result> out;
    for (const auto& cat : certv1::v1Categories())
        for (const auto& f : cert.getChildFile ("phaseb").getChildFile (cat).getChildFile ("drafts").findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            if (cat == "limiter" && ! f.getFileName().contains ("limiter_ceiling")) continue;
            const auto d = readJson (f); if (! d.isObject()) continue;
            out.push_back (certv1::judge (cat, d));
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

struct ProductData { std::vector<std::pair<juce::String, trust::Readings>> readings; trust::Measured measured; };
inline void loadRecords (const juce::File& root, const juce::String& tag, std::map<juce::String, ProductData>& by)
{
    auto take = [&] (const juce::File& f, const juce::String& category)
    {
        const auto rec = readJson (f); if (! rec.isObject()) return;
        const auto product = rec.getProperty ("product", rec.getProperty ("plugin", {}).getProperty ("name", "")).toString(); if (product.isEmpty()) return;
        auto& pd = by[product];
        auto rs = trust::readingsOfRecord (rec); if (! rs.empty()) pd.readings.push_back ({ tag, std::move (rs) });
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
            if (schema == "ej_eq_profile/1")
                if (const auto* bs = rec.getProperty ("bands", {}).getArray())
                    for (const auto& b : *bs)
                        if (b.getProperty ("verdict", "").toString() == "measured")
                        {
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
    static const juce::StringArray kHeld { "needs_licence", "needs_hardware", "refused", "needs_review", "held", "quarantined_at_scan", "surround", "crashed", "wont_load", "demo", "silent", "custom_default" };
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
    if (const auto* q = readJson (quarantine).getArray())
        for (const auto& x : *q) { const auto id = x.getProperty ("plugin_id", "").toString(); held["@quarantine:" + id.toLowerCase()] = "quarantined (" + x.getProperty ("reason", "").toString() + ")"; }
    return held;
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

inline int runTrustLevels (const juce::File& mapsDir, const juce::StringArray& certDirs, const juce::StringArray& wizardDirs, const juce::File& publishList,
                           const juce::StringArray& heldFiles, const juce::File& quarantine, const juce::File& out)
{
    if (! mapsDir.isDirectory() || out == juce::File()) { std::cerr << "usage: ejmap --trust-levels --maps <folder> --cert <folder>... --out <folder>" << std::endl; return 2; }
    std::map<juce::String, ProductData> by;
    for (const auto& c : certDirs) loadRecords (juce::File (c), "cert:" + juce::File (c).getFileName(), by);
    for (const auto& w : wizardDirs) loadRecords (juce::File (w), "wizard", by);
    const auto held = loadHeld (heldFiles, quarantine);
    const auto compCert = certifiedFromList (publishList);
    std::map<juce::String, std::set<juce::String>> v1Pass;   // category -> products
    for (const auto& c : certDirs) for (const auto& r : judgeFolder (juce::File (c))) if (r.pass) v1Pass[r.category].insert (r.product);
    out.getChildFile ("maps").createDirectory();
    std::map<juce::String, std::map<juce::String, int>> counts; juce::Array<juce::var> rows; int creditedControls = 0, failedControls = 0;
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        auto m = readJson (f); if (! m.isObject()) continue;
        const auto name = m.getProperty ("identity", {}).getProperty ("name", "").toString(), cat = m.getProperty ("category", "").toString();
        static const ProductData none;
        const auto it = by.find (name); const auto& pd = it != by.end() ? it->second : none;
        const auto rb = trust::creditMap (m, pd.readings);
        const bool certified = (cat == "compressor" && compCert.count (name)) || (v1Pass.count (cat) && v1Pass[cat].count (name)) || (cat == "amp_sim" && v1Pass["saturation"].count (name));
        const auto L = trust::levelFor (m, certified, heldWhyFor (held, name), rb, pd.measured);
        ++counts[cat][L.level]; ++counts[cat]["maps"];
        // the credited copy: each judged control carries its read-back, as the sweep writes it
        if (auto* co = m.getProperty ("controls", {}).getDynamicObject())
            for (const auto& [ctl, r] : rb)
                for (const auto& p : co->getProperties())
                    if (p.value.getProperty ("name", p.name.toString()).toString() == ctl && r.judged > 0)
                        if (auto* cobj = p.value.getDynamicObject())
                        {
                            auto* o = new juce::DynamicObject(); o->setProperty ("checked", r.judged); o->setProperty ("failed", r.failed); o->setProperty ("pass", r.proven());
                            if (r.worst.isNotEmpty()) o->setProperty ("worst", r.worst);
                            o->setProperty ("source", r.source); cobj->setProperty ("readback", juce::var (o));
                            ++creditedControls; if (r.failed > 0) ++failedControls;
                        }
        out.getChildFile ("maps").getChildFile (f.getFileName()).replaceWithText (juce::JSON::toString (m));
        auto* o = new juce::DynamicObject(); o->setProperty ("fp", m.getProperty ("fp", "")); o->setProperty ("product", name); o->setProperty ("category", cat);
        o->setProperty ("version", m.getProperty ("identity", {}).getProperty ("version", "")); o->setProperty ("level", L.level); o->setProperty ("why", L.why);
        juce::Array<juce::var> d; for (const auto& x : L.dialable) d.add (x); o->setProperty ("dialable", juce::var (d)); rows.add (juce::var (o));
    }
    out.getChildFile ("trust_levels.json").replaceWithText (juce::JSON::toString (juce::var (rows)));
    std::cout << "TRUST-LEVELS: " << rows.size() << " map(s); read-back credited on " << creditedControls << " control(s) from the measuring runs ("
              << failedControls << " of them with a failed reading); nothing loaded, nothing sent\n" << table (counts, { "maps", "certified", "mapped", "unsure" });
    return 0;
}

} // namespace ejmap::trustrun
