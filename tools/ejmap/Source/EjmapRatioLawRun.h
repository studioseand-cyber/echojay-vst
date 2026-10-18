/*
  EjmapRatioLawRun.h - THE RATIO LAW CHECK (Kathy, 19 Oct 2026; EjmapRatioLaw.h holds the rules). On the Mac that holds the licence, per
  certified compressor profile in <cert>/profiles/ (--only <product>, one or more):
    the settings: the record's ratio control at the lowest, 2:1, 4:1 and the highest ratio (the measured ratio and duplicates skipped);
    the positions: the tone check's g 2 and g 4 picks (tone_check.pick, tone_check.deep_levels[g 4].pick);
    the levels: T + 6, 9 and 12 dB (T from the existing sweep at that position), above the quiet reference ladder;
    the call: the tone check's own (--sweep at the position, the profile's engage / neutral writes) with the ratio write swapped - one
    process per (ratio, position), GR read after settling; the landed ratio read back as the plugin prints it.
  Licence gate first (a product the session knows needs a licence is not loaded); a window stops that product. Writes
  <cert>/ratio_law/<stem>.ratio_law.json (ej_ratio_law_run/1: every reading, the raw traces under raw/) with the verdict and the proposed
  ratio_law block. --dry-run prints the plan and loads nothing.
    ejmap --ratio-law --cert <cert folder> --only <product>... [--dry-run] [--probe <path>] [--ejmap-ledger <dir>] [--timeout-s N]
    ejmap --ratio-law-score <results.json>     (our side: the verdict again from the readings - loads nothing)
*/
#pragma once
#include "EjmapRatioLaw.h"

namespace ejmap::cert
{
inline juce::File ratioLawProfileFor (const juce::File& cert, const juce::String& product)
{
    for (const auto& f : cert.getChildFile ("profiles").findChildFiles (juce::File::findFiles, false, "*.json"))
        if (! f.getFileName().endsWith (".tonecheck.json") && juce::JSON::parse (f.loadFileAsString()).getProperty ("plugin", {}).getProperty ("name", "").toString() == product) return f;
    return {};
}
// the tone check's own g 2 and g 4 picks: where the server dials 2 and 4 dB
inline std::vector<std::pair<double, double>> ratioLawPositions (const juce::var& profile)
{
    std::vector<std::pair<double, double>> out;   // (g, norm)
    const auto tc = profile.getProperty ("tone_check", {});
    if (const auto n = tc.getProperty ("pick", {}).getProperty ("norm", {}); n.isDouble() || n.isInt()) out.push_back ({ (double) tc.getProperty ("g_db", 2.0), (double) n });
    if (const auto* d = tc.getProperty ("deep_levels", {}).getArray())
        for (const auto& x : *d) if (std::abs ((double) x.getProperty ("g_db", 0.0) - 4.0) < 1e-6) if (const auto n = x.getProperty ("pick", {}).getProperty ("norm", {}); n.isDouble() || n.isInt()) out.push_back ({ 4.0, (double) n });
    return out;
}

inline int ratioLawScoreFile (const juce::File& results)
{
    auto r = juce::JSON::parse (results.loadFileAsString()); if (! r.isObject()) { std::cout << "RATIO-LAW: cannot read " << results.getFullPathName() << std::endl; return 2; }
    std::vector<ratiolaw::Point> pts;
    if (const auto* runs = r.getProperty ("runs", {}).getArray())
        for (const auto& run : *runs)
            if (const auto* ls = run.getProperty ("levels", {}).getArray())
                for (const auto& l : *ls)
                {
                    ratiolaw::Point p; p.ratio = (double) run.getProperty ("labelled_ratio", run.getProperty ("ratio", 0.0)); p.position = run.getProperty ("position", 0.0); p.T = run.getProperty ("T_dbfs", 0.0);
                    p.input = l.getProperty ("input_dbfs", 0.0); if (const auto g = l.getProperty ("gr_db", {}); g.isDouble() || g.isInt()) p.gr = (double) g; pts.push_back (p);
                }
    const auto v = ratiolaw::score (pts, (double) r.getProperty ("knee_db", 0.0), (double) r.getProperty ("measured_ratio", 0.0));
    if (auto* o = r.getDynamicObject()) { o->setProperty ("ratio_law", ratiolaw::block (v)); o->setProperty ("verdict", v.words); }
    results.replaceWithText (juce::JSON::toString (r) + "\n");
    std::cout << "RATIO-LAW: " << r.getProperty ("product", "").toString() << " - " << v.words << std::endl;
    for (const auto& rr : v.ratios) std::cout << "  " << juce::String (rr.ratio, 2) << ":1  " << (rr.tested ? (rr.pass ? "PASS  " : "FAIL  ") : "--    ") << rr.why << std::endl;
    if (const auto m = r.getProperty ("measured_ratio_law", {}); m.isObject()) std::cout << "  at the measured " << m.getProperty ("ratio", "").toString() << ":1 (the existing sweep, no new load): " << m.getProperty ("words", "").toString() << std::endl;
    return 0;
}

inline int runRatioLawOne (const SweepOptions& opt, const juce::File& cert, const juce::String& product, bool dryRun)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto pf = ratioLawProfileFor (cert, product); if (! pf.existsAsFile()) { say ("RATIO-LAW: " + product + " - no profile in " + cert.getChildFile ("profiles").getFullPathName()); return 2; }
    const auto profile = juce::JSON::parse (pf.loadFileAsString());
    const auto pid = profile.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString();
    const auto recordFile = cert.getChildFile ("fixtures").getChildFile (pid.replace ("|", "_") + ".json");
    auto record = juce::JSON::parse (recordFile.loadFileAsString()); if (! record.isObject()) { say ("RATIO-LAW: " + product + " - no record " + recordFile.getFileName()); return 2; }
    const auto rt = profile.getProperty ("ratio", {}); const auto ratioName = rt.getProperty ("control", "").toString();
    const double measured = rt.getProperty ("curve", {}).size() > 0 ? (double) rt.getProperty ("curve", {})[0].getProperty ("value", 0.0) : 0.0;
    if (ratioName.isEmpty() || measured <= 0.0) { say ("RATIO-LAW: " + product + " - the profile has no ratio control / measured ratio: nothing to check (the maths stays at its fixed ratio)"); return 3; }
    juce::var ratioCtl; int ratioIdx = -1;
    if (const auto* cs = record.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) if (c.getProperty ("name", "").toString() == ratioName) { ratioCtl = c; ratioIdx = (int) c.getProperty ("index", -1); }
    juce::String refused; const auto settings = ratiolaw::settingsFor (ratioCtl, measured, refused);
    if (ratioIdx < 0 || settings.empty()) { say ("RATIO-LAW: " + product + " - " + (refused.isNotEmpty() ? refused : juce::String ("no ratio setting to test besides the measured one"))); return 3; }
    const auto positions = ratioLawPositions (profile);
    if (positions.empty()) { say ("RATIO-LAW: " + product + " - the profile's tone check has no g 2 / g 4 pick to test at"); return 3; }
    const auto curve = profile.getProperty ("amount", {}).getProperty ("curve", {});
    const double kneeDb = rt.getProperty ("knee_db", {}).isDouble() ? (double) rt.getProperty ("knee_db", 0.0) : ratiolaw::kDefaultKneeDb;
    juce::StringArray sets; juce::Array<juce::var> writes; juce::String ratioNote;
    if (const auto why = profile::toneWrites (profile, record, sets, writes, ratioNote); why.isNotEmpty()) { say ("RATIO-LAW: " + product + " - " + why); return 2; }
    auto plan = sweep::planFromFixture (record); if (! plan.ok) { say ("RATIO-LAW: " + product + " - the record has no plan: " + plan.why); return 4; }
    if (record.getProperty ("pickedCandidate", {}).isObject())
    { const int want = (int) record.getProperty ("pickedCandidate", {}).getProperty ("index", -1); for (const auto& c : plan.candidates) if (c.index == want) { plan = plan.forCandidate (c); break; } }
    if (plan.thr < 0) { say ("RATIO-LAW: " + product + " - several threshold candidates and none picked"); return 4; }
    juce::String quiet; { std::vector<double> q; for (const auto& [lo, hi] : sweep::kQuietLadder) { q.push_back (lo); q.push_back (hi); } std::sort (q.begin(), q.end()); for (double L : q) quiet << juce::String (L, 2) << ","; }
    // THE PLAN (printed in full; --dry-run stops here)
    struct Job { ratiolaw::Setting s; double g = 0.0, norm = 0.0; ratiolaw::Line T; std::vector<double> levels; };
    std::vector<Job> jobs;
    for (const auto& [g, norm] : positions)
    {
        const auto T = ratiolaw::thresholdAt (curve, norm);
        if (! T.ok) { say ("RATIO-LAW: " + product + " - no threshold from the sweep at norm " + juce::String (norm, 4) + " (g " + juce::String (g, 0) + "): position skipped"); continue; }
        std::vector<double> lv; for (double up : ratiolaw::levelsAboveThreshold()) if (T.T + up <= -0.5) lv.push_back (std::round ((T.T + up) * 100.0) / 100.0);
        for (const auto& s : settings) jobs.push_back ({ s, g, norm, T, lv });
    }
    say ("RATIO-LAW: " + product + " " + record.getProperty ("version", "").toString() + " - measured at " + juce::String (measured, 1) + ":1 ([" + juce::String (ratioIdx) + "] " + ratioName
         + "), knee " + juce::String (kneeDb, 1) + " dB; " + juce::String ((int) jobs.size()) + " probe call(s):");
    for (const auto& j : jobs)
    {   juce::StringArray l; for (double x : j.levels) l.add (juce::String (x, 2));
        say ("  " + juce::String (j.s.ratio, 1) + ":1 (" + j.s.why + ", norm " + juce::String (j.s.norm, 4) + ") at the g " + juce::String (j.g, 0) + " position (norm " + juce::String (j.norm, 4)
             + ", T " + juce::String (j.T.T, 2) + " dBFS from the sweep, its slope there " + juce::String (j.T.slope, 3) + "): inputs " + l.joinIntoString (" / ") + " dBFS"); }
    // the law at the MEASURED ratio, from the existing sweep alone
    const auto atMeasured = ratiolaw::lawAtMeasuredRatio (curve, measured, kneeDb);
    say ("  at the measured " + juce::String (measured, 1) + ":1, from the existing sweep (no load): " + atMeasured.words + " (worst " + juce::String (atMeasured.worst, 2) + " dB)");
    if (dryRun) { say ("RATIO-LAW: --dry-run - nothing loaded"); return 0; }
    // THE RUN (licence gate first)
    std::vector<InstalledRecord> hits; for (const auto& r : installedPlugins()) if (r.desc.name == product) hits.push_back (r);
    if (hits.size() != 1) { say ("RATIO-LAW: '" + product + "' resolves to " + juce::String ((int) hits.size()) + " component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto vm = profile::versionMismatch (record.getProperty ("version", "").toString(), desc.version); vm.isNotEmpty()) { say ("RATIO-LAW: " + product + " - " + vm); return 5; }
    if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("RATIO-LAW: " + product + " - " + gate.stop); return 6; }
    const auto outDir = cert.getChildFile ("ratio_law"); const auto raw = outDir.getChildFile ("raw"); raw.createDirectory();
    const auto stem = pid.replace ("|", "_");
    juce::Array<juce::var> runs;
    for (const auto& j : jobs)
    {
        if (phasebStop() != 0) { say ("RATIO-LAW: stopped - nothing written for the calls not made"); break; }
        juce::StringArray all; for (const auto& x : sets) if (! x.startsWith (juce::String (ratioIdx) + ":")) all.add (x);
        all.add (juce::String (ratioIdx) + ":" + juce::String (j.s.norm, 6));
        juce::String lv = quiet; for (size_t k = 0; k < j.levels.size(); ++k) lv << juce::String (j.levels[k], 2) << (k + 1 < j.levels.size() ? "," : "");
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId),
                                 "--sweep", "thr=" + juce::String (plan.thr), "norms=" + juce::String (j.norm, 6), "levels=" + lv,
                                 "hz=997", "hold=2.5", "discard=2.2", "win=0.3", "ref=0", "moving_db=0.1", "reset=0", "set=" + all.joinIntoString (",") };
        const auto r = runProbeCall (args, opt.timeoutMs);
        const auto tag = stem + ".r" + juce::String (j.s.ratio, 1) + ".g" + juce::String (j.g, 0); raw.getChildFile (tag + ".1.txt").replaceWithText (r.out);
        auto* o = new juce::DynamicObject(); o->setProperty ("ratio", j.s.ratio); o->setProperty ("ratio_why", j.s.why); o->setProperty ("ratio_norm", j.s.norm);
        o->setProperty ("g", j.g); o->setProperty ("position", j.norm); o->setProperty ("T_dbfs", std::round (j.T.T * 100.0) / 100.0); o->setProperty ("sweep_slope", std::round (j.T.slope * 1000.0) / 1000.0);
        o->setProperty ("probe", r.describe()); o->setProperty ("raw", "raw/" + tag + ".1.txt");
        if (r.kind == ChildResult::Kind::uiShown) { o->setProperty ("stopped", "a window appeared - closed, not clicked; this product stops here"); runs.add (juce::var (o)); say ("RATIO-LAW: " + product + " - a window: stopped"); break; }
        juce::Array<juce::var> lvls;
        if (r.cleanExit())
        {
            sweep::ProcessOut po { r.out, true, r.describe(), (float) j.norm };
            const auto d = sweep::derive (sweep::mergeProcesses ({ juce::String(), true, "none", -1.0f }, { po }), j.levels, plan.ratioIndex, true);
            if (d.setTexts.count (ratioIdx)) { o->setProperty ("landed_text", d.setTexts.at (ratioIdx)); if (const auto lr = ratiolaw::ratioOfText (d.setTexts.at (ratioIdx))) o->setProperty ("labelled_ratio", *lr); }
            for (double L : j.levels)
            {   auto* l = new juce::DynamicObject(); l->setProperty ("input_dbfs", L); const auto key = sweep::levelKey (L);
                const auto gr = d.reduction.count (key) && ! d.reduction.at (key).empty() ? d.reduction.at (key)[0] : std::nullopt;
                l->setProperty ("gr_db", gr ? juce::var (std::round (*gr * 100.0) / 100.0) : juce::var()); lvls.add (juce::var (l)); }
            o->setProperty ("quiet_check_db", ! d.quietCheckDb.empty() && d.quietCheckDb[0] ? juce::var (*d.quietCheckDb[0]) : juce::var());
        }
        o->setProperty ("levels", lvls); runs.add (juce::var (o));
        say ("  " + juce::String (j.s.ratio, 1) + ":1 at g " + juce::String (j.g, 0) + ": " + r.describe());
    }
    auto* res = new juce::DynamicObject();
    res->setProperty ("schema", "ej_ratio_law_run/1"); res->setProperty ("product", product); res->setProperty ("plugin_id", pid); res->setProperty ("version", record.getProperty ("version", ""));
    res->setProperty ("map_fp", profile.getProperty ("plugin", {}).getProperty ("map_fp", "")); res->setProperty ("measuredAt", juce::Time::getCurrentTime().toISO8601 (true)); res->setProperty ("host", opt.hostVersion);
    res->setProperty ("measured_ratio", measured); res->setProperty ("knee_db", kneeDb); res->setProperty ("ratio_control", ratioName); res->setProperty ("ratio_index", ratioIdx);
    { auto* m = new juce::DynamicObject(); m->setProperty ("ratio", measured); m->setProperty ("worst_db", std::round (atMeasured.worst * 100.0) / 100.0); m->setProperty ("status", atMeasured.status); m->setProperty ("words", atMeasured.words);
      res->setProperty ("measured_ratio_law", juce::var (m)); }
    res->setProperty ("runs", runs);
    const auto file = outDir.getChildFile (stem + ".ratio_law.json"); file.replaceWithText (juce::JSON::toString (juce::var (res)) + "\n");
    return ratioLawScoreFile (file);
}
inline int runRatioLawAll (const SweepOptions& opt, const juce::File& cert, const juce::StringArray& only, bool dryRun)
{
    if (only.isEmpty()) { std::cout << "RATIO-LAW: name the products with --only (the list of single-ratio profiles)" << std::endl; return 2; }
    int worst = 0; for (const auto& p : only) worst = juce::jmax (worst, runRatioLawOne (opt, cert, p, dryRun));
    return worst;
}
} // namespace ejmap::cert
