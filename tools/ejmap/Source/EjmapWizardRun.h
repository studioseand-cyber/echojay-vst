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
#include "EjmapWizardRoute.h"
#include <fstream>
#include <atomic>
#include <csignal>
#ifndef EJMAP_GIT_HASH
#define EJMAP_GIT_HASH "unknown"   // the suite includes this header; the ejmap target defines the real one
#endif
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

// THE STOP DURING THE SCAN (Kathy, 13 Oct, beta item 1): closing the window or Ctrl-C while Scanning / Checking stops as cleanly as a stop
// while measuring. Until today the engine had no handler there: SIGINT killed it and its probes outright, the crash markers stayed, and
// the next launch put every plugin in flight on the never-retry list. Now SIGINT / SIGTERM set this flag; the mapping pool discards what
// was in flight (never filed crashed / wont_load); a signature read cut short is not cached; the run finishes "Stopped" (markers cleared)
// and exits 130, the measuring stop's code. Maps and signature reads already done stay, so the next run carries on.
inline std::atomic<int>& wizardStop() { static std::atomic<int> f { 0 }; return f; }
inline void installWizardStop() { wizardStop() = 0; std::signal (SIGINT, [] (int) { wizardStop() = 1; }); std::signal (SIGTERM, [] (int) { wizardStop() = 1; }); }
inline juce::String scanStoppedWords (int checked, int total)
{
    return "Stopped - it carries on next time: " + juce::String (checked) + " of " + juce::String (total) + " plugin(s) checked; nothing was skipped for good, nothing packed or sent.";
}
// the mapping step (Checking): every plugin's parameter list, --jobs at a time; `runner` is the probe (the suite's fake); `finished` is called
// for each one that ran to its end - never for one in flight when the stop came (that one is discarded and runs again next time)
inline void mapForWizard (const std::vector<juce::PluginDescription>& ds, int jobs, const std::function<ChildResult (const juce::StringArray&)>& runner,
                          const std::function<jobs::Lane (const juce::PluginDescription&)>& laneOf, wizprog::Tracker& prog, const juce::File& probe,
                          const std::function<void (const juce::PluginDescription&, ChildResult&)>& finished)
{
    std::vector<size_t> idx; for (size_t i = 0; i < ds.size(); ++i) idx.push_back (i);
    jobs::Pool<size_t, juce::StringArray, ChildResult> pool;
    pool.laneOf = [&] (const size_t& i) { return laneOf ? laneOf (ds[i]) : jobs::Lane::parallel; };
    pool.stopped = [] { return wizardStop() != 0; };
    pool.prepare = [&] (const size_t& i) -> std::optional<juce::StringArray>
    { const auto& d = ds[i]; prog.started (d.name); return juce::StringArray { probe.getFullPathName(), d.name, d.fileOrIdentifier, juce::String::toHexString (d.uniqueId), "--list-params" }; };   // after a stop the pool starts nothing more (its own stopped())
    pool.run = [&] (const juce::StringArray& a) { return runner (a); };
    pool.discard = [] (const size_t&, juce::StringArray&) {};   // stopped: no state, no map; the marker goes with the stopped finish
    pool.finish = [&] (const size_t& i, juce::StringArray&, ChildResult& r) { finished (ds[i], r); };
    pool (idx, juce::jmax (1, jobs));
}

inline int runWizard (const WizardOptions& wo)
{
    installWizardStop();
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
    // NO PLUGIN LEFT BEHIND (12 Oct, EjmapWizardRoute.h): (a) the database first - nothing it settles is loaded; (b) the server's category;
    // (c) the name rules; (d) out of scope by type / words, not loaded; (e) only what is left gets EJ Map's signature reads
    const auto pending = wizroute::loadPending (wo.db);
    {   // THE REFERENCE STATES (custom defaults, 12 Oct): the server returns each plugin's agreed opening block with /lookup - STUB: the
        // database folder's references/ - and Phase B checks every plugin against it before measuring (EjmapCustomDefault.h)
        const auto refs = wo.db.getChildFile ("references"); const auto dst = out.getChildFile ("references");
        if (refs.isDirectory()) { dst.createDirectory(); for (const auto& f : refs.findChildFiles (juce::File::findFiles, false, "*.reference.json")) f.copyFileTo (dst.getChildFile (f.getFileName())); }
    }
    auto* state = new juce::DynamicObject(); juce::Array<juce::var> rows; std::map<juce::String, juce::StringArray> queues; std::map<juce::String, int> counts;   // queues: Phase B category -> products
    for (const auto& r : skippedRows) { rows.add (r); ++counts["crashed"]; }
    const auto problemState = [] (const juce::String& st) { return wizprog::isProblemState (st); };
    struct Pending { size_t i; wizroute::Decision d; juce::DynamicObject* row; };
    std::vector<Pending> toMap, toCategorise;
    auto typeOf = [] (const juce::PluginDescription& d) { return d.fileOrIdentifier.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf (",", false, false); };
    auto enqueue = [&] (const juce::String& name, const juce::String& cat) { for (const auto& pb : wizroute::phaseBFor (cat)) queues[pb].addIfNotAlreadyThere (name); };
    auto queueFile = [&] (const juce::String& pb) { return out.getChildFile (pb == "wizard" ? juce::String ("wizard_queue.txt") : pb == "wizard_eq" ? juce::String ("wizard_queue_eq.txt") : "wizard_queue_" + pb + ".txt"); };
    auto queuedAnywhere = [&] { for (const auto& [pb, l] : queues) if (! l.isEmpty()) return true; return false; };
    // the measuring items, in the measuring order (Kathy, 12 Oct): one per product and Phase B category (a limiter has two)
    auto measureItems = [&] { std::vector<wizprog::Item> q; int gi = 0; for (const auto& grp : wizroute::measureOrder()) { for (const auto& pb : grp) if (queues.count (pb)) for (const auto& n : queues[pb])
        for (const auto& ir : plugins) if (ir.desc.name == n) { wizprog::Item it; it.product = n; it.kind = wizroute::kindOf (pb); it.serial = serialOf (ir.desc); it.group = gi; q.push_back (it); break; }
        ++gi; } return q; };
    auto landedRow = [&] (const juce::String& pb, const juce::String& product) -> juce::var {
        for (const auto& f : out.getChildFile ("phaseb").getChildFile (pb).findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
        { auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("product", "").toString() == product) { r.getDynamicObject()->setProperty ("_stem", f.getFileName().replace (".phaseb.json", "")); return r; } }
        return {}; };
    // 1. THE DATABASE CHECK (Scanning): one answer per plugin; only rule B's quick read-back check loads anything here
    { std::vector<wizprog::Item> its; for (const auto& ir : plugins) { wizprog::Item it; it.product = ir.desc.name; it.kind = "check"; its.push_back (it); } prog.stage (wizprog::Stage::scanning, its); }
    for (size_t i = 0; i < plugins.size(); ++i)
    {
        const auto& ir = plugins[i]; const auto uid = juce::String::toHexString (ir.desc.uniqueId).toLowerCase(); const auto ver = ir.desc.version;
        const auto se = store.count (uid + "|" + ver) ? store.at (uid + "|" + ver) : StoreEntry();
        const auto ans = wizroute::ask (db, pending, catalogue, uid, ver, se.fp, ir.desc.name);
        const auto d = wizroute::route (ans, wizard::wizardCategory (ir.desc.name, juce::var()), categorise::outOfScope (typeOf (ir.desc), ir.desc.name, {}), wizroute::isKnownMultiband (uid));
        auto* row = new juce::DynamicObject(); row->setProperty ("product", ir.desc.name); row->setProperty ("plugin_id", wizroute::pluginId (uid, ver)); row->setProperty ("map_fp", se.fp);
        row->setProperty ("category", d.category); row->setProperty ("route", wizroute::routeName (d.route)); row->setProperty ("vendor", ir.desc.manufacturerName);
        juce::String st, why = d.why;
        switch (d.route)
        {
            case wizroute::Route::skipHave: st = "ready"; break;
            case wizroute::Route::skipPending: st = "pending_review"; break;
            case wizroute::Route::outOfScope: st = "out_of_scope"; break;
            case wizroute::Route::quickCheck:
            {
                const auto gate = licenceGate (opt, ir.desc.name, ir.desc.manufacturerName, out, false);
                if (gate.stop.isNotEmpty()) { st = gate.stop.contains (uad::kNotConnected) ? "needs_hardware" : "needs_licence"; why = gate.stop; break; }
                prog.started (ir.desc.name);
                bool window = false; const auto rb = quickCheck (opt, ir.desc, ans.entry->profile, window);
                row->setProperty ("from_version", ans.entry->version()); row->setProperty ("readback", rb.why);
                if (window) { st = "needs_licence"; why = rb.why; }
                else if (! wizapp::needsMeasuring (wizard::Match::sameControls, rb.pass)) { st = "ready_from_version"; why = "the version " + ans.entry->version() + " profile: " + rb.why; }
                else { st = "queued"; why = "the version " + ans.entry->version() + " profile does not fit (" + rb.why + "): measure this version"; toMap.push_back ({ i, d, row }); }
                break;
            }
            case wizroute::Route::measure: st = "queued"; toMap.push_back ({ i, d, row }); break;
            case wizroute::Route::sendOther: st = "other_category"; toMap.push_back ({ i, d, row }); break;
            case wizroute::Route::held: st = "held"; toMap.push_back ({ i, d, row }); break;   // multiband: mapped, listed, not measured
            case wizroute::Route::categorise: st = "identifying"; toCategorise.push_back ({ i, d, row }); break;
            case wizroute::Route::needsCategory: st = "needs_category"; break;
        }
        row->setProperty ("state", st); row->setProperty ("why", why); rows.add (juce::var (row));
        prog.finished (ir.desc.name, problemState (st));
        say ("  " + ir.desc.name.paddedRight (' ', 30) + " " + ver.paddedRight (' ', 9) + " -> " + wizroute::routeName (d.route) + " / " + st + ": " + why);
    }
    // 2. IDENTIFYING (Checking): the map (the parameter list, --jobs) for what will be measured, sent or categorised; then the signature
    // reads for what is left - cached in categorised.json, so a later run (the app's measure run) loads nothing again for them
    {
        std::vector<wizprog::Item> its; for (const auto* l : { &toMap, &toCategorise }) for (const auto& p : *l) { wizprog::Item it; it.product = plugins[p.i].desc.name; it.kind = "check"; its.push_back (it); }
        prog.stage (wizprog::Stage::checking, its);
    }
    std::vector<size_t> mapIdx; for (const auto* l : { &toMap, &toCategorise }) for (const auto& p : *l) { const auto& d = plugins[p.i].desc; if (! store.count (juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version)) mapIdx.push_back (p.i); }
    std::map<juce::String, juce::String> early;   // product -> a state settled while mapping (needs_licence / crashed / wont_load)
    say ("WIZARD: map store " + opt.ledger.getChildFile ("maps").getFullPathName().replace (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(), "~") + ": "
         + juce::String ((int) store.size()) + " map(s); " + juce::String ((int) mapIdx.size()) + " plugin(s) to map (the database settled the rest without loading them)");
    if (! mapIdx.empty())
    {
        opt.ledger.getChildFile ("maps").createDirectory();
        std::vector<juce::PluginDescription> mapDs; for (auto i : mapIdx) mapDs.push_back (plugins[i].desc);
        auto laneOfD = [&] (const juce::PluginDescription& d) { jobs::LaneInputs in; in.uad = uad::isUadProduct (d.name, d.manufacturerName); in.pace = paceHeld (d, bundlesAll, in.paceWhy); return jobs::laneFor (in); };
        mapForWizard (mapDs, opt.jobs, [&] (const juce::StringArray& a) { return runChild (a, opt.timeoutMs); }, laneOfD, prog, opt.probe,
                      [&] (const juce::PluginDescription& d, ChildResult& r)
        {
            const auto uid = juce::String::toHexString (d.uniqueId).toLowerCase();
            if (r.kind == ChildResult::Kind::uiShown) { early[d.name] = "needs_licence"; say ("  " + d.name + ": a window appeared (licence?) - closed, not clicked; not mapped"); prog.finished (d.name, true); return; }
            const auto ps = paramsOfList (r.out);
            if (! r.cleanExit() || ps.empty()) { early[d.name] = r.cleanExit() ? "wont_load" : "crashed"; say ("  " + d.name + ": not mapped (" + (ps.empty() && r.cleanExit() ? juce::String ("no parameters") : r.describe()) + ")"); prog.finished (d.name, true); return; }
            const auto fp = echojay::fingerprintForDescription (d, (int) ps.size());
            const auto cat = wizard::wizardCategory (d.name, catalogue);
            opt.ledger.getChildFile ("maps").getChildFile (fp + ".json").replaceWithText (juce::JSON::toString (wizard::paramMap (d.name, d.manufacturerName, uid, d.version, fp, ps, cat)) + "\n");
            store[uid + "|" + d.version] = { fp, cat };
            prog.finished (d.name, false);
            say ("  " + d.name.paddedRight (' ', 30) + " mapped: " + juce::String ((int) ps.size()) + " parameter(s), map " + fp.substring (0, 12));
        });
    }
    auto stoppedInScan = [&] (const juce::String& where)
    {   // 13 Oct: a stop while Scanning / Checking - the maps and signature reads that finished stay; nothing is filed for what was in flight
        const auto [done, total] = prog.stageDone();
        say ("WIZARD: stopped while " + where + " (" + juce::String (done) + " of " + juce::String (total) + " checked): nothing filed for the plugin(s) in flight; the next run carries on");
        prog.finish (scanStoppedWords (done, total), true);
        return 130;
    };
    if (wizardStop() != 0) return stoppedInScan ("mapping");
    auto fpOf = [&] (const juce::PluginDescription& d) { const auto k = juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version; return store.count (k) ? store.at (k).fp : juce::String(); };
    for (const auto& p : toMap)
    {
        const auto& d = plugins[p.i].desc; p.row->setProperty ("map_fp", fpOf (d));
        if (early.count (d.name)) { p.row->setProperty ("state", early[d.name]); p.row->setProperty ("why", "it could not be mapped (" + early[d.name] + ")"); continue; }
        if (p.d.route == wizroute::Route::measure || p.row->getProperty ("state").toString() == "queued") enqueue (d.name, p.d.category);
    }
    // the signature reads (EJ Map's categorise step, one plugin at a time), cached by plugin_id
    const auto cacheF = out.getChildFile ("categorised.json"); auto cache = juce::JSON::parse (cacheF.loadFileAsString()); if (! cache.isObject()) cache = juce::var (new juce::DynamicObject());
    auto catOpt = opt; catOpt.includePace = true;   // the user's own Mac: PACE units load (a licence window is still filed, quietly)
    const auto catRaw = out.getChildFile ("categorise_raw"); catRaw.createDirectory();
    for (const auto& p : toCategorise)
    {
        if (wizardStop() != 0) return stoppedInScan ("reading signatures");
        const auto& ir = plugins[p.i]; const auto pid = p.row->getProperty ("plugin_id").toString(); p.row->setProperty ("map_fp", fpOf (ir.desc));
        if (early.count (ir.desc.name)) { p.row->setProperty ("state", early[ir.desc.name]); p.row->setProperty ("why", "it could not be mapped (" + early[ir.desc.name] + ")"); continue; }
        juce::var sig;
        if (cache.hasProperty (pid)) { sig = cache[juce::Identifier (pid)]; say ("  " + ir.desc.name + ": signature reads from categorised.json (not loaded again)"); }
        else
        {
            prog.started (ir.desc.name);
            const auto r = categoriseOne (catOpt, ir, {}, bundlesAll, catRaw);
            if (wizardStop() != 0) return stoppedInScan ("reading signatures");   // a read cut short by the stop is never cached
            sig = categorise::rowVar (r); sig.getDynamicObject()->setProperty ("reads_ran", r.measured);
            cache.getDynamicObject()->setProperty (pid, sig); cacheF.replaceWithText (juce::JSON::toString (cache));
            prog.finished (ir.desc.name, false);
        }
        const auto d = wizroute::afterSignature ((bool) sig.getProperty ("reads_ran", false), (bool) sig.getProperty ("settled", false), sig.getProperty ("category", "").toString(),
                                                 sig.getProperty ("out_of_scope", "").toString(), sig.getProperty ("why", "").toString());
        p.row->setProperty ("route", wizroute::routeName (d.route)); p.row->setProperty ("category", d.category); p.row->setProperty ("why", d.why); p.row->setProperty ("signature", sig);
        const juce::String st = d.route == wizroute::Route::measure ? "queued" : d.route == wizroute::Route::sendOther ? "other_category" : d.route == wizroute::Route::outOfScope ? "out_of_scope"
                              : d.route == wizroute::Route::held ? "held" : "needs_category";
        p.row->setProperty ("state", st);
        if (d.route == wizroute::Route::measure) enqueue (ir.desc.name, d.category);
        say ("  " + ir.desc.name.paddedRight (' ', 30) + " -> " + wizroute::routeName (d.route) + (d.category.isNotEmpty() ? " (" + d.category + ")" : juce::String()) + ": " + d.why);
    }
    counts.clear(); for (const auto& r : rows) ++counts[r.getProperty ("state", "").toString()];
    for (const auto& grp : wizroute::measureOrder()) for (const auto& pb : grp) queueFile (pb).replaceWithText ((queues.count (pb) ? queues[pb].joinIntoString ("\n") : juce::String()) + "\n");
    for (auto& rv : rows) { juce::StringArray pbs; for (const auto& [pb, l] : queues) if (l.contains (rv.getProperty ("product", "").toString())) pbs.add (pb); if (! pbs.isEmpty()) rv.getDynamicObject()->setProperty ("phaseb", pbs.joinIntoString (",")); }
    say ("WIZARD: " + juce::String ((int) plugins.size()) + " plugin(s) found: " + wizard::summaryLine (counts, wo.bundle));
    {   // what the scan screen says: "N plugins we don't have yet - about X" (the measured per-category seconds once any exist, --jobs aware)
        const auto q = measureItems(); juce::StringArray distinct; for (const auto& it : q) distinct.addIfNotAlreadyThere (it.product);
        wizprog::Times tq; for (const auto& grp : wizroute::measureOrder()) for (const auto& pb : grp) for (const auto& f : out.getChildFile ("phaseb").getChildFile (pb).findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
        { const auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("outcome", "").toString() == "ok") tq.add (wizroute::kindOf (pb), (double) r.getProperty ("seconds", 0.0)); }
        state->setProperty ("to_measure", distinct.size()); state->setProperty ("to_measure_eta_s", std::round (wizprog::stageSecondsLeft (q, tq, opt.jobs)));
    }
    // 2. MEASURE THE QUEUES (Phase B: --jobs, resumable, every outcome; the tone check / the EQ acceptance)
    if (wo.measure && queuedAnywhere())
    {
        say ("WIZARD: measuring " + juce::String ((int) measureItems().size()) + " plugin measurement(s)" + (opt.jobs > 1 ? ", " + juce::String (opt.jobs) + " at a time" : juce::String()) + " - stop any time; it carries on next time");
        {   // MEASURING: the two queues; a plugin whose Phase B row is already on disk is done (resume) and its seconds are a measured time
            auto its = measureItems();   // RESUME: an item whose Phase B row already landed is done; its seconds are a measured time
            for (auto& it : its) for (const auto& grp : wizroute::measureOrder()) for (const auto& pb : grp) if (wizroute::kindOf (pb) == it.kind)
                if (const auto r = landedRow (pb, it.product); r.isObject()) { it.done = true; it.problem = r.getProperty ("outcome", "").toString() != "ok";
                    if (it.problem) prog.addProblems (1); else prog.seedTime (it.kind, (double) r.getProperty ("seconds", 0.0)); }
            prog.stage (wizprog::Stage::measuring, its);
            phaseb::rowHooks().started = [&] (const juce::String& cat, const juce::String& product) { prog.started (product, wizroute::kindOf (cat)); };
            phaseb::rowHooks().finished = [&] (const juce::String& cat, const juce::String& product, const juce::String& outcome, double seconds) { prog.finished (product, outcome != "ok", seconds, wizroute::kindOf (cat)); };
            prog.startTicker();
        }
        // THE MEASURING ORDER: one Phase B pass per group, most valuable first; a stop (130) ends the order there
        auto optW = opt; optW.wizardQueues = true; int pbCode = 0;
        for (const auto& grp : wizroute::measureOrder())
        {
            juce::StringArray run; for (const auto& pb : grp) if (queues.count (pb) && ! queues[pb].isEmpty()) run.add (pb);
            if (run.isEmpty()) continue;
            say ("WIZARD: measuring " + run.joinIntoString (" + ") + " (" + juce::String ((int) [&] { int n = 0; for (const auto& pb : run) n += queues[pb].size(); return n; }()) + ")");
            pbCode = runPhaseBAll (optW, run, {}, {});
            if (wizprog::measureStopped (pbCode)) break;
        }
        prog.stopTicker(); phaseb::rowHooks() = {};
        {   // the drafts for the categories beyond compressor / EQ (their cert derive step, the same rules)
            juce::StringArray cats; for (const auto& [pb, l] : queues) if (! l.isEmpty() && pb != "wizard" && pb != "wizard_eq") cats.add (pb);
            if (! cats.isEmpty() && ! wizprog::measureStopped (pbCode)) runPhaseBDrafts (optW, cats);
        }
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
            const auto pbs = juce::StringArray::fromTokens (row->getProperty ("phaseb").toString(), ",", "");
            if (! pbs.isEmpty() && pbs[0] != "wizard" && pbs[0] != "wizard_eq")
            {   // ALL CATEGORIES: measured when the category's own row is ok and its draft was derived; a limiter's compressor pass is said beside
                const auto r = landedRow (pbs[0], row->getProperty ("product").toString());
                juce::String st, why; juce::StringArray draftsL;
                if (! r.isObject()) st = "measuring";
                else
                {
                    const auto oc = r.getProperty ("outcome", "").toString(); why = r.getProperty ("reason", "").toString();
                    for (const auto& f : out.getChildFile ("phaseb").getChildFile (pbs[0]).getChildFile ("drafts").findChildFiles (juce::File::findFiles, false, r.getProperty ("_stem", "").toString() + "*.draft.json"))
                        draftsL.add (pbs[0] + "/drafts/" + f.getFileName());
                    bool usable = false, cabinet = false;
                    for (const auto& rel : draftsL) { const auto dv = juce::JSON::parse (out.getChildFile ("phaseb").getChildFile (rel).loadFileAsString()); if (wizroute::draftUsable (pbs[0], dv)) usable = true; if (dv.hasProperty ("cabinet_response")) cabinet = true; }
                    row->setProperty ("cabinet_read", cabinet);   // a saturation row that read an amp sim's cabinet is sent as amp_sim
                    st = oc == "ok" ? (draftsL.isEmpty() || ! usable ? juce::String ("not_measurable") : juce::String ("measured")) : wizard::plainOutcome (oc, {});
                    if (oc == "ok" && draftsL.isEmpty()) why = "measured, but no " + pbs[0] + " draft could be derived from the record";
                    else if (oc == "ok" && ! usable) why = "measured, but its " + pbs[0] + " draft carries nothing the server can use (e.g. no drive control)";
                }
                for (int k = 1; k < pbs.size(); ++k) { const auto r2 = landedRow (pbs[k], row->getProperty ("product").toString()); row->setProperty ("also_" + pbs[k], r2.isObject() ? r2.getProperty ("outcome", "") : juce::var ("not run")); }
                if (pbs.contains ("limitercomp") && st == "measured")
                {   // the limiter's compressor pass: its exported profile + tone check travel with the limiter draft (sent like any compressor)
                    const auto r2 = landedRow ("limitercomp", row->getProperty ("product").toString()); const auto stem2 = r2.getProperty ("_stem", "").toString();
                    juce::String cs; const auto ocV = juce::JSON::parse (out.getChildFile ("phaseb/limitercomp/outcome").getChildFile (stem2 + ".outcomes.json").loadFileAsString());   // held: getArray() on a temporary dangles
                    if (const auto* a = ocV.getArray()) for (const auto& o : *a) cs = o.getProperty ("state", "").toString();
                    for (const auto& f : out.getChildFile ("phaseb/limitercomp/profiles").findChildFiles (juce::File::findFiles, false, "*.json"))
                    {
                        if (f.getFileName().endsWith (".tonecheck.json")) continue;
                        if (juce::JSON::parse (f.loadFileAsString()).getProperty ("plugin", {}).getProperty ("plugin_id", "").toString() != row->getProperty ("plugin_id").toString()) continue;
                        const auto tc = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tonecheck.json");
                        const bool pass = (bool) juce::JSON::parse (tc.loadFileAsString()).getProperty ("pass_within_0_5_db", false);
                        row->setProperty ("compressor_pass", cs + (pass ? ", tone check passed" : ", tone check did not pass"));
                        if (wizroute::sendCompressorPass (cs, pass)) draftsL.add ("limitercomp/profiles/" + f.getFileName()), draftsL.add ("limitercomp/profiles/" + tc.getFileName());
                    }
                    row->setProperty ("drafts", draftsL.joinIntoString (","));
                }
                row->setProperty ("state", st); row->setProperty ("outcome", r.getProperty ("outcome", {})); row->setProperty ("drafts", draftsL.joinIntoString (",")); if (why.isNotEmpty()) row->setProperty ("why", why); ++counts[st];
                if (r.hasProperty ("reference_vote")) { row->setProperty ("reference_vote", r.getProperty ("reference_vote", {})); row->setProperty ("reference_stem", r.getProperty ("_stem", {})); }   // 12 Oct: this Mac's vote travels
                continue;
            }
            const bool isEq = row->getProperty ("category").toString() == "eq"; const auto catDir = out.getChildFile (isEq ? "phaseb/wizard_eq" : "phaseb/wizard");
            juce::String outcome, certState, rowWhy, stem;
            for (const auto& f : catDir.findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
            { const auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("product", "").toString() != row->getProperty ("product").toString()) continue;
              if (r.hasProperty ("reference_vote")) { row->setProperty ("reference_vote", r.getProperty ("reference_vote", {})); row->setProperty ("reference_stem", f.getFileName().replace (".phaseb.json", "")); }   // 12 Oct: this Mac's vote travels
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
        say ("WIZARD: " + wizard::summaryLine (counts, wo.bundle));
    }
    state->setProperty ("schema", "ej_wizard_state/0"); state->setProperty ("plugins", rows); state->setProperty ("summary", wizard::summaryLine (counts, wo.bundle));
    out.getChildFile ("wizard_state.json").replaceWithText (juce::JSON::toString (juce::var (state)) + "\n");
    // 3. THE BUNDLE (profiles + tone-check sidecars / EQ profiles + a manifest; nothing personal) and the STUBBED upload
    juce::String bundleWords;
    if (wo.bundle)
    {
        { wizprog::Item it; it.product = "the contribution bundle"; it.kind = "pack"; prog.stage (wizprog::Stage::packing, { it }); prog.started (it.product); }
        const auto B = out.getChildFile ("contribution"); B.deleteRecursively(); B.getChildFile ("profiles").createDirectory();
        juce::Array<juce::var> items, confirmations, skipped, crashedList, needsCategory, otherCategories, outOfScopeList, heldList, referenceVotes;
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
            if (const auto ve = wizard::referenceVoteEntry (rv); ve.isObject()) referenceVotes.add (ve);   // 12 Oct: every vote travels, whatever the row's state
            if (const auto hc = wizard::heldCustomDefault (rv); hc.isObject()) { heldList.add (hc); continue; }   // custom_default: HELD until a third source settles it
            if (st == "measured" && rv.getProperty ("drafts", "").toString().isNotEmpty())
            {   // ALL CATEGORIES: the category's draft(s), as the certification derives them
                juce::Array<juce::var> files;
                for (const auto& rel : juce::StringArray::fromTokens (rv.getProperty ("drafts", "").toString(), ",", ""))
                { const auto f = out.getChildFile ("phaseb").getChildFile (rel); f.copyFileTo (B.getChildFile ("profiles").getChildFile (f.getFileName())); files.add ("profiles/" + f.getFileName()); }
                auto* it = new juce::DynamicObject(); it->setProperty ("product", product); it->setProperty ("kind", wizserver::toServer (rv.getProperty ("category", "").toString(), (bool) rv.getProperty ("cabinet_read", false)));
                it->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); it->setProperty ("map_fp", rv.getProperty ("map_fp", {})); it->setProperty ("drafts", files); items.add (juce::var (it));   // measured_by (EJ Map's internal names) stays local
            }
            else if (st == "measured" && isEq)
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
            else if (st == "held")
            {   // multiband (12 Oct): no server category - listed with its map, NO category field
                const auto fp = rv.getProperty ("map_fp", "").toString(); const auto mapF = opt.ledger.getChildFile ("maps").getChildFile (fp + ".json");
                auto* h = new juce::DynamicObject(); h->setProperty ("product", product); h->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); h->setProperty ("map_fp", fp);
                h->setProperty ("map", fp.isNotEmpty() && mapF.existsAsFile() ? juce::JSON::parse (mapF.loadFileAsString()) : juce::var()); h->setProperty ("why", "held: no server category");
                heldList.add (juce::var (h));
            }
            else if (st == "needs_category" || st == "other_category")
            {   // NOTHING DROPPED (12 Oct): the map, and for needs_category the signature reads and the leaning, for Sean to label
                const auto fp = rv.getProperty ("map_fp", "").toString(); const auto mapF = opt.ledger.getChildFile ("maps").getChildFile (fp + ".json");
                auto* e = new juce::DynamicObject(); e->setProperty ("product", product); e->setProperty ("vendor", rv.getProperty ("vendor", {})); e->setProperty ("plugin_id", rv.getProperty ("plugin_id", {}));
                e->setProperty ("map_fp", fp); e->setProperty ("map", fp.isNotEmpty() && mapF.existsAsFile() ? juce::JSON::parse (mapF.loadFileAsString()) : juce::var());
                const bool review = st == "needs_category";
                const auto serverCat = wizserver::toServer (rv.getProperty ("category", "").toString());   // the boundary: Sean's names, or none
                if (review) { e->setProperty ("leaning", serverCat.isNotEmpty() ? juce::var (serverCat) : juce::var()); e->setProperty ("signature", rv.getProperty ("signature", {})); }
                else e->setProperty ("category", serverCat);
                e->setProperty ("why", rv.getProperty ("why", {}));
                const auto dir = review ? "needs_category" : "other_categories"; B.getChildFile (dir).createDirectory();
                const auto fn = juce::File::createLegalFileName (product) + ".json"; B.getChildFile (dir).getChildFile (fn).replaceWithText (juce::JSON::toString (juce::var (e)));
                auto* m = new juce::DynamicObject(); m->setProperty ("product", product); m->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); m->setProperty ("map_fp", fp);
                m->setProperty (review ? "leaning" : "category", serverCat.isNotEmpty() ? juce::var (serverCat) : juce::var()); m->setProperty ("file", juce::String (dir) + "/" + fn);
                (review ? needsCategory : otherCategories).add (juce::var (m));
            }
            else if (st == "out_of_scope")
            { auto* o = new juce::DynamicObject(); o->setProperty ("product", product); o->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); o->setProperty ("why", "not an EchoJay category: listed, not loaded"); outOfScopeList.add (juce::var (o)); }   // the internal label stays local
            else if (st != "ready" && st != "pending_review")   // A: nothing to send; pending_review: another user already sent it
            { auto* s = new juce::DynamicObject(); s->setProperty ("product", product); s->setProperty ("state", st); s->setProperty ("why", rv.getProperty ("why", {})); skipped.add (juce::var (s)); }
        }
        auto* M = new juce::DynamicObject(); M->setProperty ("schema", "ej_contribution/0"); M->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
        M->setProperty ("tool", juce::String ("EJ Map ") + EJMAP_GIT_HASH); M->setProperty ("platform", juce::SystemStats::getOperatingSystemName());
        M->setProperty ("profiles", items); M->setProperty ("confirmations", confirmations); M->setProperty ("skipped", skipped); M->setProperty ("crashed", crashedList);
        M->setProperty ("needs_category", needsCategory); M->setProperty ("other_categories", otherCategories); M->setProperty ("out_of_scope", outOfScopeList); M->setProperty ("held", heldList);
        M->setProperty ("reference_votes", referenceVotes);
        const juce::var Mv (M);   // ONE owner of the manifest (a second juce::var (M) after a temporary would use a deleted object)
        {   // THE BOUNDARY CHECK (12 Oct): every category the manifest carries is one of the server's 12, or nothing is packed
            const auto bad = wizserver::invalidOutgoing (Mv);
            if (! bad.isEmpty()) { say ("WIZARD: the bundle is NOT made - categories the server does not have: " + bad.joinIntoString ("; ")); prog.finish ("Stopped: the bundle would carry a category the server does not have (" + bad.joinIntoString ("; ") + "). Nothing was made or sent."); return 3; }
        }
        M->setProperty ("note", "results only: profiles, their tone checks and this manifest - no plugin files, no audio, no account token");
        B.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (Mv) + "\n");
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
    prog.finish ("Done in " + wizprog::durationWords ((juce::Time::getMillisecondCounterHiRes() - tStart) / 1000.0) + ": " + juce::String ((int) plugins.size()) + " plugin(s) - " + wizard::summaryLine (counts, wo.bundle) + "."
                 + (bundleWords.isNotEmpty() ? " " + bundleWords : juce::String()) + (wo.verbose ? juce::String() : " Details: wizard.log"));
    return 0;
}

} // namespace ejmap::cert
