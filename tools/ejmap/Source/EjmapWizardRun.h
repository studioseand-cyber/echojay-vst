/*
  EjmapWizardRun.h - the wizard's run (feat/ejmap-wizard): MAP (no map store needed: the wizard reads every plugin's parameter list
  itself, --jobs, and writes its own maps) -> look up (A / B / C) on those maps -> the quick live check for B -> measure the queues
  (compressors and EQs: Phase B's "wizard" / "wizard_eq": --jobs, resumable, every outcome, the tone check / the EQ acceptance) ->
  the contribution bundle -> the upload (STUBBED: nothing is sent). The pure rules are in EjmapWizard.h.

    ejmap --wizard --db <profiles folder> --out <work folder> [--ejmap-ledger <map store>] [--only NAME]... [--jobs N] [--measure] [--bundle]

  The map store (--ejmap-ledger, default ~/Library/ejmap) may be empty: the wizard maps what it finds there missing.
*/
#pragma once

#include "EjmapWizard.h"
#include "EjmapCertDriver.h"
#include "EjmapWizardProgress.h"
#include <fstream>
#include <unistd.h>

namespace ejmap::cert
{

struct WizardOptions { SweepOptions opt; juce::File db, skip; juce::StringArray only; bool measure = false, bundle = false, verbose = false; };   // skip: the app's never-retry list (skip.json)

// the map store (read): (uid|version) -> { fp, category }
struct StoreEntry { juce::String fp, category; };
inline std::map<juce::String, StoreEntry> mapStore (const juce::File& ledger)
{
    std::map<juce::String, StoreEntry> m;
    for (const auto& f : ledger.getChildFile ("maps").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto d = juce::JSON::parse (f.loadFileAsString()); const auto id = d.getProperty ("identity", {});
        if (id.getProperty ("format", "").toString() != "AudioUnit") continue;
        m[id.getProperty ("uid", "").toString().toLowerCase() + "|" + id.getProperty ("version", "").toString()] = { d.getProperty ("fp", "").toString(), d.getProperty ("category", "").toString() };
    }
    return m;
}
inline std::vector<std::pair<int, juce::String>> paramsOfList (const juce::String& listOut)
{
    std::vector<std::pair<int, juce::String>> ps; std::set<int> seen;
    for (const auto& line : juce::StringArray::fromLines (listOut))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() >= 2 && f[0].isNotEmpty() && f[0].containsOnly ("0123456789") && seen.insert (f[0].getIntValue()).second) ps.push_back ({ f[0].getIntValue(), f[1] });
    }
    return ps;
}

// THE QUICK LIVE CHECK: the plugin's control list, then each written control read back at the profile's norms (one probe call each)
inline wizard::Readback quickCheck (const SweepOptions& opt, const juce::PluginDescription& desc, const juce::var& profile, bool& window)
{
    window = false;
    const juce::StringArray base { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) };
    auto listArgs = base; listArgs.add ("--list-params");
    const auto lp = runChild (listArgs, opt.timeoutMs);
    if (lp.kind == ChildResult::Kind::uiShown) { window = true; return { false, 0, "a window appeared while reading its controls" }; }
    std::map<juce::String, int> names; for (const auto& [i, n] : paramsOfList (lp.out)) if (! names.count (n)) names[n] = i;
    const auto writes = wizard::writesOf (profile);
    std::map<int, juce::StringArray> norms;
    for (const auto& w : writes) { int idx = w.index; if (const auto it = names.find (w.control); it != names.end()) idx = it->second; if (idx >= 0) norms[idx].addIfNotAlreadyThere (wizard::normKey (w.norm)); }
    std::map<int, std::map<juce::String, juce::String>> observed;
    for (const auto& [idx, ns] : norms)
    {
        auto a = base; a.add ("--text-at-norms"); a.add (juce::String (idx)); a.add (ns.joinIntoString (","));
        const auto r = runChild (a, opt.timeoutMs);
        if (r.kind == ChildResult::Kind::uiShown) { window = true; return { false, 0, "a window appeared while reading its controls" }; }
        for (const auto& line : juce::StringArray::fromLines (r.out))
        {
            const auto f = juce::StringArray::fromTokens (line, "\t", "");
            if (f.size() < 4 || f[0] != "at") continue;
            const int t = f.indexOf ("text"); observed[idx][wizard::normKey (f[1].getDoubleValue())] = t >= 0 && t + 1 < f.size() ? f[t + 1] : juce::String();
        }
    }
    return wizard::readback (writes, names, observed);
}

inline int runWizard (const WizardOptions& wo)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto& opt = wo.opt; const auto out = opt.out; out.createDirectory();
    // THE PROGRESS (11 Oct): the terminal shows one live line + a final summary; every detail line goes to wizard.log (--verbose: as before,
    // details on the terminal and the bar as a line per change); progress.json is what a window app reads
    std::ostream term (std::cout.rdbuf()); std::ofstream logFile; std::streambuf* const coutWas = std::cout.rdbuf();
    struct Restore { std::streambuf* b; ~Restore() { std::cout.rdbuf (b); phaseb::rowHooks() = {}; } } restore { coutWas };
    if (! wo.verbose)
    {
        logFile.open (out.getChildFile ("wizard.log").getFullPathName().toStdString(), std::ios::app);
        term << "EJ Map wizard - every detail goes to " << out.getChildFile ("wizard.log").getFullPathName().replace (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(), "~") << std::endl;
        std::cout.rdbuf (logFile.rdbuf());
    }
    const bool live = ! wo.verbose && isatty (STDOUT_FILENO);
    wizprog::Tracker prog (out.getChildFile ("progress.json"), term, live, opt.jobs);
    prog.setMarkerDir (out.getChildFile ("inflight"));
    const double tStart = juce::Time::getMillisecondCounterHiRes();
    const auto db = wizard::loadDb (wo.db);
    const auto catalogue = juce::JSON::parse (wo.db.getChildFile ("categories.json").loadFileAsString());
    say ("WIZARD: " + juce::String ((int) db.size()) + " profile(s) in the database (" + wo.db.getFileName() + ", a local folder: nothing is fetched)");
    // THE NEVER-RETRY LIST (the app's crash recovery): a plugin the wizard went down with is not loaded again, by any stage
    const auto skipped = wo.skip != juce::File() ? wizapp::skipList (wo.skip.getParentDirectory()) : juce::StringArray();
    std::vector<InstalledRecord> plugins; juce::Array<juce::var> skippedRows;
    for (const auto& r : installedAudioUnits())
    {
        if (! (wo.only.isEmpty() || wo.only.contains (r.desc.name))) continue;
        if (skipped.contains (r.desc.name))
        { auto* row = new juce::DynamicObject(); row->setProperty ("product", r.desc.name); row->setProperty ("plugin_id", "AudioUnit|" + juce::String::toHexString (r.desc.uniqueId).toLowerCase() + "|" + r.desc.version);
          row->setProperty ("state", "crashed"); row->setProperty ("why", "the wizard closed while checking it: skipped for good, never retried"); skippedRows.add (juce::var (row));
          say ("  " + r.desc.name + ": on the never-retry list (the wizard closed while checking it) - not loaded"); continue; }
        plugins.push_back (r);
    }
    auto store = mapStore (opt.ledger);
    prog.setPlugins ((int) plugins.size());
    const auto bundlesAll = componentBundles();
    auto serialOf = [&] (const juce::PluginDescription& d) { jobs::LaneInputs in; in.uad = uad::isUadProduct (d.name, d.manufacturerName); in.pace = paceHeld (d, bundlesAll, in.paceWhy); return jobs::laneFor (in) == jobs::Lane::serial; };
    {   // SCANNING: every plugin; one the store already maps is done (a resumed run starts there)
        std::vector<wizprog::Item> its; for (const auto& ir : plugins) { wizprog::Item it; it.product = ir.desc.name; it.kind = "map"; it.serial = serialOf (ir.desc);
            it.done = store.count (juce::String::toHexString (ir.desc.uniqueId).toLowerCase() + "|" + ir.desc.version) > 0; its.push_back (it); }
        prog.stage (wizprog::Stage::scanning, its);
    }
    std::map<juce::String, juce::String> early;   // product -> a state settled while mapping (needs_licence / crashed / wont_load)
    // 0. MAP what the store does not have: the parameter list (one probe call each, --jobs), the join key, the category
    {
        std::vector<size_t> toMap; for (size_t i = 0; i < plugins.size(); ++i) { const auto& d = plugins[i].desc; if (! store.count (juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version)) toMap.push_back (i); }
        say ("WIZARD: map store " + opt.ledger.getChildFile ("maps").getFullPathName().replace (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(), "~") + ": "
             + juce::String ((int) store.size()) + " map(s); " + juce::String ((int) toMap.size()) + " of the " + juce::String ((int) plugins.size()) + " plugin(s) need mapping");
        if (! toMap.empty())
        {
            opt.ledger.getChildFile ("maps").createDirectory();
            const auto bundles = componentBundles();
            jobs::Pool<size_t, juce::StringArray, ChildResult> pool;
            pool.laneOf = [&] (const size_t& i) { jobs::LaneInputs in; in.uad = uad::isUadProduct (plugins[i].desc.name, plugins[i].desc.manufacturerName); in.pace = paceHeld (plugins[i].desc, bundles, in.paceWhy); return jobs::laneFor (in); };
            pool.prepare = [&] (const size_t& i) -> std::optional<juce::StringArray>
            { const auto& d = plugins[i].desc; prog.started (d.name); return juce::StringArray { opt.probe.getFullPathName(), d.name, d.fileOrIdentifier, juce::String::toHexString (d.uniqueId), "--list-params" }; };
            pool.run = [&] (const juce::StringArray& a) { return runChild (a, opt.timeoutMs); };
            pool.discard = [] (const size_t&, juce::StringArray&) {};
            pool.finish = [&] (const size_t& i, juce::StringArray&, ChildResult& r)
            {
                const auto& d = plugins[i].desc; const auto uid = juce::String::toHexString (d.uniqueId).toLowerCase();
                if (r.kind == ChildResult::Kind::uiShown) { early[d.name] = "needs_licence"; say ("  " + d.name + ": a window appeared (licence?) - closed, not clicked; not mapped"); prog.finished (d.name, true); return; }
                const auto ps = paramsOfList (r.out);
                if (! r.cleanExit() || ps.empty()) { early[d.name] = r.cleanExit() ? "wont_load" : "crashed"; say ("  " + d.name + ": not mapped (" + (ps.empty() && r.cleanExit() ? juce::String ("no parameters") : r.describe()) + ")"); prog.finished (d.name, true); return; }
                const auto fp = echojay::fingerprintForDescription (d, (int) ps.size());
                const auto cat = wizard::wizardCategory (d.name, catalogue);
                opt.ledger.getChildFile ("maps").getChildFile (fp + ".json").replaceWithText (juce::JSON::toString (wizard::paramMap (d.name, d.manufacturerName, uid, d.version, fp, ps, cat)) + "\n");
                store[uid + "|" + d.version] = { fp, cat };
                prog.finished (d.name, false);
                say ("  " + d.name.paddedRight (' ', 30) + " mapped: " + juce::String ((int) ps.size()) + " parameter(s), map " + fp.substring (0, 12) + (cat == "eq" ? juce::String (", an EQ") : cat.isNotEmpty() ? ", a " + cat : juce::String (", kind unknown")));
            };
            pool (toMap, juce::jmax (1, opt.jobs));
        }
    }
    // 1. LOOK UP on the maps (A / B / C), the quick check for B
    auto* state = new juce::DynamicObject(); juce::Array<juce::var> rows; juce::StringArray queue, queueEq; std::map<juce::String, int> counts;
    for (const auto& r : skippedRows) { rows.add (r); ++counts["crashed"]; }
    { std::vector<wizprog::Item> its; for (const auto& ir : plugins) { wizprog::Item it; it.product = ir.desc.name; it.kind = "check"; its.push_back (it); } prog.stage (wizprog::Stage::checking, its); }
    const auto problemState = [] (const juce::String& st) { return wizprog::isProblemState (st); };
    for (const auto& ir : plugins)
    {
        prog.started (ir.desc.name);
        const auto uid = juce::String::toHexString (ir.desc.uniqueId).toLowerCase(); const auto ver = ir.desc.version;
        const auto se = store.count (uid + "|" + ver) ? store.at (uid + "|" + ver) : StoreEntry();
        auto cat = wizard::wizardCategory (ir.desc.name, catalogue); if (cat.isEmpty() && (se.category == "compressor" || se.category == "eq")) cat = se.category;
        auto* row = new juce::DynamicObject(); row->setProperty ("product", ir.desc.name); row->setProperty ("plugin_id", "AudioUnit|" + uid + "|" + ver); row->setProperty ("map_fp", se.fp); row->setProperty ("category", cat);
        juce::String st, why;
        if (early.count (ir.desc.name)) { st = early[ir.desc.name]; why = st == "needs_licence" ? "a licence window appeared while mapping" : "it could not be mapped"; }
        else if (cat.isEmpty()) { st = "not_in_scope"; why = "the wizard measures compressors and EQs; this plugin's kind is not one of them (or not known)"; }
        else
        {
            const auto L = wizard::lookup (db, uid, ver, se.fp); why = L.why;
            if (L.match == wizard::Match::exact && ! wizapp::needsMeasuring (L.match, false)) st = "ready";   // rule A: skipped silently
            else if (L.match == wizard::Match::sameControls)
            {
                const auto gate = licenceGate (opt, ir.desc.name, ir.desc.manufacturerName, out, false);
                if (gate.stop.isNotEmpty()) { st = gate.stop.contains (uad::kNotConnected) ? "needs_hardware" : "needs_licence"; why = gate.stop; }
                else
                {
                    bool window = false; const auto rb = quickCheck (opt, ir.desc, L.entry->profile, window);
                    row->setProperty ("from_version", L.entry->version()); row->setProperty ("readback", rb.why);
                    if (window) { st = "needs_licence"; why = rb.why; }
                    else if (! wizapp::needsMeasuring (L.match, rb.pass)) { st = "ready_from_version"; why = "the version " + L.entry->version() + " profile: " + rb.why; }
                    else { st = "queued"; why = "the version " + L.entry->version() + " profile does not fit (" + rb.why + "): measure this version"; (cat == "eq" ? queueEq : queue).add (ir.desc.name); }
                }
            }
            else { st = "queued"; (cat == "eq" ? queueEq : queue).add (ir.desc.name); }
        }
        row->setProperty ("state", st); row->setProperty ("why", why); rows.add (juce::var (row)); ++counts[st];
        prog.finished (ir.desc.name, problemState (st) && ! early.count (ir.desc.name));   // a mapping problem was counted when it happened
        say ("  " + ir.desc.name.paddedRight (' ', 30) + " " + ver.paddedRight (' ', 9) + (cat.isNotEmpty() ? " (" + cat + ")" : juce::String()) + " -> " + st + ": " + why);
    }
    out.getChildFile ("wizard_queue.txt").replaceWithText (queue.joinIntoString ("\n") + "\n");
    out.getChildFile ("wizard_queue_eq.txt").replaceWithText (queueEq.joinIntoString ("\n") + "\n");
    say ("WIZARD: " + juce::String ((int) plugins.size()) + " plugin(s) found: " + wizard::summaryLine (counts));
    {   // what the scan screen says: "N plugins we don't have yet - about X" (the measured per-category seconds once any exist, --jobs aware)
        std::vector<wizprog::Item> q; for (const auto& ir : plugins) { const bool eq = queueEq.contains (ir.desc.name); if (! eq && ! queue.contains (ir.desc.name)) continue;
            wizprog::Item it; it.product = ir.desc.name; it.kind = eq ? "eq" : "compressor"; it.serial = serialOf (ir.desc); q.push_back (it); }
        wizprog::Times tq; for (const auto* c : { "wizard", "wizard_eq" }) for (const auto& f : out.getChildFile ("phaseb").getChildFile (c).findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
        { const auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("outcome", "").toString() == "ok") tq.add (juce::String (c) == "wizard_eq" ? "eq" : "compressor", (double) r.getProperty ("seconds", 0.0)); }
        state->setProperty ("to_measure", (int) q.size()); state->setProperty ("to_measure_eta_s", std::round (wizprog::stageSecondsLeft (q, tq, opt.jobs)));
    }
    // 2. MEASURE THE QUEUES (Phase B: --jobs, resumable, every outcome; the tone check / the EQ acceptance)
    if (wo.measure && (! queue.isEmpty() || ! queueEq.isEmpty()))
    {
        say ("WIZARD: measuring " + juce::String (queue.size() + queueEq.size()) + " plugin(s)" + (opt.jobs > 1 ? ", " + juce::String (opt.jobs) + " at a time" : juce::String()) + " - stop any time; it carries on next time");
        {   // MEASURING: the two queues; a plugin whose Phase B row is already on disk is done (resume) and its seconds are a measured time
            std::map<juce::String, juce::String> landed; std::map<juce::String, double> landedSeconds;
            for (const auto* c : { "wizard", "wizard_eq" }) for (const auto& f : out.getChildFile ("phaseb").getChildFile (c).findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
            { const auto r = juce::JSON::parse (f.loadFileAsString()); const auto p = r.getProperty ("product", "").toString(); landed[p] = r.getProperty ("outcome", "").toString(); landedSeconds[p] = (double) r.getProperty ("seconds", 0.0); }
            std::vector<wizprog::Item> q;
            for (const auto& ir : plugins)
            {
                const bool eq = queueEq.contains (ir.desc.name); if (! eq && ! queue.contains (ir.desc.name)) continue;
                wizprog::Item it; it.product = ir.desc.name; it.kind = eq ? "eq" : "compressor"; it.serial = serialOf (ir.desc); q.push_back (it);
            }
            const auto its = wizprog::resumeItems (q, landed);
            for (const auto& it : its) if (it.done) { if (it.problem) prog.addProblems (1); else prog.seedTime (it.kind, landedSeconds[it.product]); }
            prog.stage (wizprog::Stage::measuring, its);
            phaseb::rowHooks().started = [&] (const juce::String&, const juce::String& product) { prog.started (product); };
            phaseb::rowHooks().finished = [&] (const juce::String&, const juce::String& product, const juce::String& outcome, double seconds) { prog.finished (product, outcome != "ok", seconds); };
            prog.startTicker();
        }
        const int pbCode = runPhaseBAll (opt, { "wizard", "wizard_eq" }, {}, {});
        prog.stopTicker(); phaseb::rowHooks() = {};
        if (wizprog::measureStopped (pbCode))
        {   // the user stopped it: no bundle, no "Done" - the rows that landed stay, the same command carries on
            const auto [measured, toMeasure] = prog.stageDone();
            say ("WIZARD: stopped while measuring (" + juce::String (measured) + " of " + juce::String (toMeasure) + " landed): no bundle; the same command carries on");
            prog.finish (wizprog::stoppedWords (prog.stoppedAt(), measured, toMeasure, prog.running()), true);
            return 130;
        }
        counts.clear();
        for (auto& rv : rows)
        {
            auto* row = rv.getDynamicObject(); if (row->getProperty ("state").toString() != "queued") { ++counts[row->getProperty ("state").toString()]; continue; }
            const bool isEq = row->getProperty ("category").toString() == "eq"; const auto catDir = out.getChildFile (isEq ? "phaseb/wizard_eq" : "phaseb/wizard");
            juce::String outcome, certState, rowWhy, stem;
            for (const auto& f : catDir.findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
            { const auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("product", "").toString() != row->getProperty ("product").toString()) continue;
              outcome = r.getProperty ("outcome", "").toString(); rowWhy = r.getProperty ("reason", "").toString(); stem = f.getFileName().replace (".phaseb.json", "");
              if (! isEq) { const auto oc = juce::JSON::parse (catDir.getChildFile ("outcome").getChildFile (stem + ".outcomes.json").loadFileAsString());
                            if (const auto* a = oc.getArray()) for (const auto& o : *a) { certState = o.getProperty ("state", "").toString(); if (rowWhy.isEmpty()) rowWhy = o.getProperty ("reason", "").toString(); } } }
            juce::String st = outcome.isEmpty() ? juce::String ("measuring") : wizard::plainOutcome (outcome, isEq ? juce::String ("exported") : certState);
            if (isEq && st == "measured")
            {   // an EQ is ready to send when its acceptance passed
                juce::String why; const auto draft = juce::JSON::parse (catDir.getChildFile ("drafts").getChildFile (stem + ".eq_profile.draft.json").loadFileAsString());
                if (! wizard::eqAccepted (draft, why)) { st = "not_measurable"; rowWhy = "measured, but " + why; } else { rowWhy = why; row->setProperty ("eq_draft", "drafts/" + stem + ".eq_profile.draft.json"); }
            }
            row->setProperty ("state", st); row->setProperty ("outcome", outcome); row->setProperty ("cert_state", certState); if (rowWhy.isNotEmpty()) row->setProperty ("why", rowWhy); ++counts[st];
        }
        say ("WIZARD: " + wizard::summaryLine (counts));
    }
    state->setProperty ("schema", "ej_wizard_state/0"); state->setProperty ("plugins", rows); state->setProperty ("summary", wizard::summaryLine (counts));
    out.getChildFile ("wizard_state.json").replaceWithText (juce::JSON::toString (juce::var (state)) + "\n");
    // 3. THE BUNDLE (profiles + tone-check sidecars / EQ profiles + a manifest; nothing personal) and the STUBBED upload
    juce::String bundleWords;
    if (wo.bundle)
    {
        { wizprog::Item it; it.product = "the contribution bundle"; it.kind = "pack"; prog.stage (wizprog::Stage::packing, { it }); prog.started (it.product); }
        const auto B = out.getChildFile ("contribution"); B.deleteRecursively(); B.getChildFile ("profiles").createDirectory();
        juce::Array<juce::var> items, confirmations, skipped, crashedList;
        if (wo.skip != juce::File())
        {   // the crash reports travel with the upload (already redacted by the app; one that still trips the check is withheld, said so)
            const auto work = wo.skip.getParentDirectory(); B.getChildFile ("crash_reports").createDirectory();
            char h[256] = {}; gethostname (h, sizeof h - 1);
            const auto skipV = wizapp::readSkip (work);
            if (const auto* a = skipV.getArray())
                for (const auto& e : *a)
                {
                    const auto rel = e.getProperty ("crash_report", "").toString(); const auto src = work.getChildFile (rel);
                    auto txt = src.loadFileAsString();
                    const auto pp = wizard::personalProblems (txt, juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(), juce::SystemStats::getLogonName(), juce::String (h).upToFirstOccurrenceOf (".", false, false));
                    if (! pp.isEmpty()) txt = "This crash report was withheld: it would carry " + pp.joinIntoString (", ") + ".\n";
                    if (rel.isNotEmpty()) B.getChildFile (rel).replaceWithText (txt);
                    auto* c = new juce::DynamicObject(); c->setProperty ("product", e.getProperty ("product", {})); c->setProperty ("stage", e.getProperty ("stage", {}));
                    c->setProperty ("crash_report", rel); c->setProperty ("report_found", e.getProperty ("report_found", false)); crashedList.add (juce::var (c));
                }
        }
        for (const auto& rv : rows)
        {
            const auto st = rv.getProperty ("state", "").toString(); const auto product = rv.getProperty ("product", "").toString();
            const bool isEq = rv.getProperty ("category", "").toString() == "eq";
            if (st == "measured" && isEq)
            {
                const auto f = out.getChildFile ("phaseb/wizard_eq").getChildFile (rv.getProperty ("eq_draft", "").toString());
                f.copyFileTo (B.getChildFile ("profiles").getChildFile (f.getFileName()));
                auto* it = new juce::DynamicObject(); it->setProperty ("product", product); it->setProperty ("kind", "eq"); it->setProperty ("plugin_id", rv.getProperty ("plugin_id", {}));
                it->setProperty ("map_fp", rv.getProperty ("map_fp", {})); it->setProperty ("profile", "profiles/" + f.getFileName()); it->setProperty ("acceptance", rv.getProperty ("why", {})); items.add (juce::var (it));
            }
            else if (st == "measured")
                for (const auto& f : out.getChildFile ("phaseb/wizard/profiles").findChildFiles (juce::File::findFiles, false, "*.json"))
                {
                    if (f.getFileName().endsWith (".tonecheck.json")) continue;
                    const auto p = juce::JSON::parse (f.loadFileAsString()); if (p.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString() != rv.getProperty ("plugin_id", "").toString()) continue;
                    const auto tc = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tonecheck.json");
                    const auto tcv = juce::JSON::parse (tc.loadFileAsString());
                    if (! (bool) tcv.getProperty ("pass_within_0_5_db", false)) { auto* s = new juce::DynamicObject(); s->setProperty ("product", product); s->setProperty ("why", "measured, but its tone check did not pass: not sent"); skipped.add (juce::var (s)); continue; }
                    f.copyFileTo (B.getChildFile ("profiles").getChildFile (f.getFileName())); tc.copyFileTo (B.getChildFile ("profiles").getChildFile (tc.getFileName()));
                    auto* it = new juce::DynamicObject(); it->setProperty ("product", product); it->setProperty ("kind", "compressor"); it->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); it->setProperty ("map_fp", p.getProperty ("plugin", {}).getProperty ("map_fp", {}));
                    it->setProperty ("profile", "profiles/" + f.getFileName()); it->setProperty ("tone_check", "profiles/" + tc.getFileName()); it->setProperty ("tone_check_g2_pass", true); items.add (juce::var (it));
                }
            else if (st == "ready_from_version")
            { auto* c = new juce::DynamicObject(); c->setProperty ("product", product); c->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); c->setProperty ("profile_from_version", rv.getProperty ("from_version", {})); c->setProperty ("readback", rv.getProperty ("readback", {})); confirmations.add (juce::var (c)); }
            else if (st != "ready")
            { auto* s = new juce::DynamicObject(); s->setProperty ("product", product); s->setProperty ("state", st); s->setProperty ("why", rv.getProperty ("why", {})); skipped.add (juce::var (s)); }
        }
        auto* M = new juce::DynamicObject(); M->setProperty ("schema", "ej_contribution/0"); M->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
        M->setProperty ("tool", juce::String ("EJ Map ") + EJMAP_GIT_HASH); M->setProperty ("platform", juce::SystemStats::getOperatingSystemName());
        M->setProperty ("profiles", items); M->setProperty ("confirmations", confirmations); M->setProperty ("skipped", skipped); M->setProperty ("crashed", crashedList);
        M->setProperty ("note", "results only: profiles, their tone checks and this manifest - no plugin files, no audio, no account token");
        B.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (juce::var (M)) + "\n");
        char host[256] = {}; gethostname (host, sizeof host - 1);
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(); const auto user = juce::SystemStats::getLogonName();
        juce::StringArray problems;
        for (const auto& f : B.findChildFiles (juce::File::findFiles, true))
            for (const auto& p : wizard::personalProblems (f.loadFileAsString(), home, user, juce::String (host).upToFirstOccurrenceOf (".", false, false)))
                problems.add (f.getRelativePathFrom (B) + ": " + p);
        if (! problems.isEmpty()) { say ("WIZARD: the bundle is NOT made - it would carry: " + problems.joinIntoString ("; ")); prog.finish ("Stopped: the bundle would carry something personal (" + problems.joinIntoString ("; ") + "). Nothing was made or sent."); return 3; }
        const auto zip = out.getChildFile ("contribution.zip"); zip.deleteFile();
        juce::ChildProcess z; z.start (juce::StringArray { "/bin/sh", "-c", "cd \"" + B.getFullPathName() + "\" && /usr/bin/zip -qr \"" + zip.getFullPathName() + "\" ." }); z.waitForProcessToFinish (60000);
        say ("WIZARD: contribution bundle -> " + zip.getFileName() + " (" + juce::String (items.size()) + " new profile(s), " + juce::String (confirmations.size()) + " confirmation(s) of another version's profile, "
             + juce::String (skipped.size()) + " skipped with the reason; " + juce::String (zip.getSize() / 1024.0, 1) + " KB; checked: no paths, names or tokens)");
        say ("WIZARD: UPLOAD (stubbed): nothing is sent. A real app shows this list and sends only when the user presses Send.");
        prog.finished ("the contribution bundle", false);
        bundleWords = "Ready to send: " + zip.getFileName() + " (" + juce::String (items.size()) + " new, " + juce::String (confirmations.size()) + " confirmation(s), " + juce::String (zip.getSize() / 1024.0, 1) + " KB) - nothing was sent.";
    }
    prog.setProblems (wizprog::problemCount (counts));   // the final states decide (an EQ can land ok and fail its acceptance)
    prog.finish ("Done in " + wizprog::durationWords ((juce::Time::getMillisecondCounterHiRes() - tStart) / 1000.0) + ": " + juce::String ((int) plugins.size()) + " plugin(s) - " + wizard::summaryLine (counts) + "."
                 + (bundleWords.isNotEmpty() ? " " + bundleWords : juce::String()) + (wo.verbose ? juce::String() : " Details: wizard.log"));
    return 0;
}

} // namespace ejmap::cert
