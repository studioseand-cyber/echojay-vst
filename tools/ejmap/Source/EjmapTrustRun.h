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
            certv1::Result r; r.category = "multiband"; r.product = m.product; r.pluginId = m.pluginId; r.pass = m.pass;
            for (const auto& b : m.bands) for (const auto& p : b.parts) r.parts.push_back ({ "band " + juce::String (b.band) + ": " + p.name, p.state, p.why });
            for (const auto& b : m.bands) r.parts.push_back ({ "band " + juce::String (b.band), b.pass ? "pass" : (mbv1::passesSoFar (b) ? "not_tested" : "fail"), b.pass ? juce::String() : juce::String (mbv1::passesSoFar (b) ? "passes so far; the v1 measuring decides" : "a part failed") });
            out.push_back (r);
        }
    }
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

struct ProductData { std::vector<std::pair<juce::String, trust::Readings>> readings; std::vector<trust::SourceMeta> metas; trust::Measured measured; };   // metas: one per reading source (the recency rule)
inline void loadRecords (const juce::File& root, const juce::String& tag, std::map<juce::String, ProductData>& by)
{
    auto take = [&] (const juce::File& f, const juce::String& category)
    {
        const auto rec = readJson (f); if (! rec.isObject()) return;
        const auto product = rec.getProperty ("product", rec.getProperty ("plugin", {}).getProperty ("name", "")).toString(); if (product.isEmpty()) return;
        auto& pd = by[product];
        auto rs = trust::readingsOfRecord (rec); if (! rs.empty()) { pd.readings.push_back ({ tag, std::move (rs) }); pd.metas.push_back (trust::metaOfRecord (rec)); }
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
    juce::StringArray needRoles; for (const auto& need : trust::needsFor (cat)) for (const auto& alt : juce::StringArray::fromTokens (need, "|", "")) for (const auto& r : juce::StringArray::fromTokens (alt, "+", "")) needRoles.addIfNotAlreadyThere (r);
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
    return items;
}

inline int runTrustLevels (const juce::File& mapsDir, const juce::StringArray& certDirs, const juce::StringArray& wizardDirs, const juce::File& publishList,
                           const juce::StringArray& heldFiles, const juce::File& quarantine, const juce::File& out, const juce::File& humanFile = {}, const juce::String& firstCategory = {})
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
    const auto human = loadHuman (humanFile); juce::Array<juce::var> review;
    std::map<juce::String, std::map<juce::String, int>> counts; juce::Array<juce::var> rows; int creditedControls = 0, failedControls = 0, supersededReadings = 0, supersededControls = 0;
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        auto m = readJson (f); if (! m.isObject()) continue;
        const auto name = m.getProperty ("identity", {}).getProperty ("name", "").toString(), cat = m.getProperty ("category", "").toString();
        static const ProductData none;
        const auto it = by.find (name); const auto& pd = it != by.end() ? it->second : none;
        const auto rb = trust::creditMap (m, pd.readings, pd.metas);
        for (const auto& [ctl, r] : rb) if (r.superseded > 0) { supersededReadings += r.superseded; ++supersededControls; }
        const auto fpKey = m.getProperty ("fp", "").toString();
        const auto measured = human.count (fpKey) ? withHuman (pd.measured, human.at (fpKey)) : pd.measured;
        const bool certified = (cat == "compressor" && compCert.count (name)) || (v1Pass.count (cat) && v1Pass[cat].count (name)) || (cat == "amp_sim" && v1Pass["saturation"].count (name))
                               || (cat == "compressor" && v1Pass["multiband"].count (name));   // 16 Oct: a multiband passes per band (mbv1)
        const auto heldWhy = heldWhyFor (held, name);
        const auto L = trust::levelFor (m, certified, heldWhy, rb, measured);
        if (L.level == "unsure" && L.why.startsWith ("role not identified")) review.addArray (reviewItems (m, measured, rb));
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
                            o->setProperty ("source", r.source); if (r.superseded > 0) o->setProperty ("superseded", r.superseded); cobj->setProperty ("readback", juce::var (o));
                            ++creditedControls; if (r.failed > 0) ++failedControls;
                        }
        out.getChildFile ("maps").getChildFile (f.getFileName()).replaceWithText (juce::JSON::toString (m));
        auto* o = new juce::DynamicObject(); o->setProperty ("fp", m.getProperty ("fp", "")); o->setProperty ("product", name); o->setProperty ("category", cat);
        o->setProperty ("version", m.getProperty ("identity", {}).getProperty ("version", "")); o->setProperty ("level", L.level); o->setProperty ("why", L.why);
        juce::Array<juce::var> d; for (const auto& x : L.dialable) d.add (x); o->setProperty ("dialable", juce::var (d)); rows.add (juce::var (o));
    }
    out.getChildFile ("trust_levels.json").replaceWithText (juce::JSON::toString (juce::var (rows)));
    {   // the review list (schema ejmap_role_review/1), the same file the local page and the admin page read
        auto* rv = new juce::DynamicObject(); rv->setProperty ("schema", "ejmap_role_review/1"); rv->setProperty ("generated_at", juce::Time::getCurrentTime().toISO8601 (true));
        rv->setProperty ("source", "ejmap --trust-levels");
        // --review-first <category> (Kathy, 17 Oct: EQ): the rows where a yes counts now, that category's first - in the file and on the page
        if (firstCategory.isNotEmpty())
        {
            rv->setProperty ("first_category", firstCategory);
            auto tier = [&] (const juce::var& x) { const bool now = (bool) x.getProperty ("yes_makes_mapped", false); return now ? (x.getProperty ("category", "").toString() == firstCategory ? 0 : 1) : 2; };
            std::vector<juce::var> v; for (const auto& x : review) v.push_back (x);
            std::stable_sort (v.begin(), v.end(), [&] (const juce::var& a, const juce::var& b) { return tier (a) < tier (b); });
            review.clearQuick(); for (const auto& x : v) review.add (x);
        }
        rv->setProperty ("items", juce::var (review));
        out.getChildFile ("review_items.json").replaceWithText (juce::JSON::toString (juce::var (rv)));
        int wouldMap = 0; for (const auto& x : review) if ((bool) x.getProperty ("yes_makes_mapped", false)) ++wouldMap;
        std::cout << "TRUST-LEVELS: " << review.size() << " control(s) for the human check (" << wouldMap << " where a yes alone makes the map mapped); "
                  << human.size() << " map(s) with answers applied\n";
    }
    std::cout << "TRUST-LEVELS: " << rows.size() << " map(s); read-back credited on " << creditedControls << " control(s) from the measuring runs ("
              << failedControls << " of them with a failed reading); " << supersededReadings << " older failed reading(s) on " << supersededControls << " control(s) set aside by a newer clean read-back (the recency rule); nothing loaded, nothing sent\n" << table (counts, { "maps", "certified", "mapped", "unsure" });
    return 0;
}

} // namespace ejmap::trustrun
