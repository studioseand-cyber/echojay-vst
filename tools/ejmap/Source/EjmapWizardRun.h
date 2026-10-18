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
#include "EjmapApbPreflight.h"
#include "EjmapSelfGuardRun.h"   // 16 Oct: the guard on the developer's own loads (guardedPlugins)
#include "EjmapReadbackSweepRun.h"   // 17 Oct (rulings 3): the VST3 lane - the bridge read and the probe's VST3 identity line
#include <fstream>
#include <atomic>
#include <csignal>
#include <thread>
#ifndef EJMAP_GIT_HASH
#define EJMAP_GIT_HASH "unknown"   // the suite includes this header; the ejmap target defines the real one
#endif
#include <unistd.h>

namespace ejmap::cert
{

struct WizardOptions { SweepOptions opt; juce::File db, skip; juce::StringArray only; bool measure = false, bundle = false, verbose = false, plan = false; juce::String runId; double checkpointMinutes = 60.0; };   // runId / checkpointMinutes: 13 Oct, partial uploads   // plan: the database step only, nothing loaded (plan.json)   // skip: the app's never-retry list (skip.json)

// the map store (read): (uid|version) -> { fp, category }
struct StoreEntry { juce::String fp, category; };
inline std::map<juce::String, StoreEntry> mapStore (const juce::File& ledger)
{
    std::map<juce::String, StoreEntry> m;
    for (const auto& f : ledger.getChildFile ("maps").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto d = juce::JSON::parse (f.loadFileAsString()); const auto id = d.getProperty ("identity", {});
        if (id.getProperty ("format", "").toString() != "AudioUnit" && id.getProperty ("format", "").toString() != "VST3") continue;   // 17 Oct: VST3 maps too (their uids are the class hashes: no clash)
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
inline wizard::Readback quickCheck (const SweepOptions& opt, const juce::PluginDescription& desc, const juce::var& profile, bool& window, ChildResult::Kind* listKind = nullptr)
{
    window = false;
    const juce::StringArray base { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) };
    auto listArgs = base; listArgs.add ("--list-params");
    const auto lp = runChild (listArgs, opt.timeoutMs);
    if (listKind != nullptr) *listKind = lp.kind;
    if (lp.kind == ChildResult::Kind::uiShown) { window = true; return { false, 0, "a window appeared while reading its controls", "list", 1 }; }
    if (! lp.cleanExit()) return { false, 0, "its controls could not be read (" + lp.describe() + ")", "list", 1 };
    std::map<juce::String, int> names; for (const auto& [i, n] : paramsOfList (lp.out)) if (! names.count (n)) names[n] = i;
    const auto writes = wizard::writesOf (profile);
    if (const auto miss = wizroute::missingWrittenControl (writes, names); miss.isNotEmpty())   // 14 Oct: one load, not one per control
        return { false, 0, "the plugin has no control named '" + miss + "'", "list", 1 };
    std::map<int, juce::StringArray> norms;
    for (const auto& w : writes) { int idx = w.index; if (const auto it = names.find (w.control); it != names.end()) idx = it->second; if (idx >= 0) norms[idx].addIfNotAlreadyThere (wizard::normKey (w.norm)); }
    std::map<int, std::map<juce::String, juce::String>> observed; juce::String mode = "per_control"; int loads = 1;
    if (! norms.empty())
    {   // 14 Oct (Kathy): ONE load for every control the check reads (each restored after its reads); the per-control way only if that
        // read is refused or fails. Its limit grows with the reads, and it never counts toward a WaveShell's hang (the list load does)
        int points = 0; for (const auto& [idx, ns] : norms) points += ns.size();
        auto a = base; a.add ("--text-at-norms-multi"); a.add (wizard::multiNormsSpec (norms));
        const auto r = runChild (a, opt.timeoutMs + 500 * points); ++loads;
        if (r.kind == ChildResult::Kind::uiShown) { window = true; return { false, 0, "a window appeared while reading its controls", "one_load", loads }; }
        if (wizroute::oneLoadReadUsable (r.cleanExit(), r.out)) { observed = wizard::parseTextAt (r.out); mode = "one_load"; }
    }
    if (mode != "one_load")
        for (const auto& [idx, ns] : norms)
        {
            auto a = base; a.add ("--text-at-norms"); a.add (juce::String (idx)); a.add (ns.joinIntoString (","));
            const auto r = runChild (a, opt.timeoutMs); ++loads;
            if (r.kind == ChildResult::Kind::uiShown) { window = true; return { false, 0, "a window appeared while reading its controls", "per_control", loads }; }
            for (const auto& [k, m] : wizard::parseTextAt (r.out, idx)) for (const auto& [n, t] : m) observed[k][n] = t;
        }
    auto rb = wizard::readback (writes, names, observed); rb.mode = mode; rb.loads = loads;
    return rb;
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
// 15 Oct (sharing the Mac): a map that TIMED OUT is not finished on that try - after the pool, each runs again ALONE (`retryAlone`, the
// caller's longer bound) and only that result is filed; last night's 6 "crashes" were timeouts at 4 at a time, and 9 mapped fine
// alone the next morning. The pool's job count follows `jobsNow` (2 while the user is active)
inline void mapForWizard (const std::vector<juce::PluginDescription>& ds, int jobs, const std::function<ChildResult (const juce::StringArray&)>& runner,
                          const std::function<jobs::Lane (const juce::PluginDescription&)>& laneOf, wizprog::Tracker& prog, const juce::File& probe,
                          const std::function<void (const juce::PluginDescription&, ChildResult&)>& finished,
                          const std::function<ChildResult (const juce::StringArray&)>& retryAlone = {}, const std::function<int()>& jobsNow = {})
{
    std::vector<size_t> idx; for (size_t i = 0; i < ds.size(); ++i) idx.push_back (i);
    jobs::Pool<size_t, juce::StringArray, ChildResult> pool;
    pool.laneOf = [&] (const size_t& i) { return laneOf ? laneOf (ds[i]) : jobs::Lane::parallel; };
    pool.stopped = [] { return wizardStop() != 0; };
    pool.prepare = [&] (const size_t& i) -> std::optional<juce::StringArray>
    { const auto& d = ds[i]; prog.started (d.name); return juce::StringArray { probe.getFullPathName(), d.name, d.fileOrIdentifier, juce::String::toHexString (d.uniqueId), "--list-params" }; };   // after a stop the pool starts nothing more (its own stopped())
    pool.run = [&] (const juce::StringArray& a) { return runner (a); };
    pool.discard = [] (const size_t&, juce::StringArray&) {};   // stopped: no state, no map; the marker goes with the stopped finish
    std::vector<std::pair<size_t, juce::StringArray>> later;   // timed out on the first try
    pool.finish = [&] (const size_t& i, juce::StringArray& a, ChildResult& r) { if (retryAlone && r.kind == ChildResult::Kind::timedOut) { later.push_back ({ i, a }); return; } finished (ds[i], r); };
    if (jobsNow) pool.jobsNow = jobsNow;
    pool (idx, juce::jmax (1, jobs));
    for (auto& [i, a] : later)
    {   // alone: nothing else of ours is running now
        if (wizardStop() != 0) return;
        auto r = retryAlone (a);
        if (wizardStop() != 0) return;   // stopped mid-retry: discarded, it runs again next time
        if (! r.cleanExit()) r.out << "\nretried_alone\tfirst try timed out with other work running\n";
        finished (ds[i], r);
    }
}

// each row's Phase B queues (the "phaseb" key), by product name - except a VST3-lane row (18 Oct): a VST3 twin shares its AU's name,
// never its queue; on 18 Oct 33 bridge-read twins took their AU's queues and ended the run "still need measuring"
inline void attributeQueues (juce::Array<juce::var>& rows, const std::map<juce::String, juce::StringArray>& queues)
{
    for (auto& rv : rows)
    {
        if (rv.getProperty ("lane", "").toString() == "vst3") continue;
        juce::StringArray pbs; for (const auto& [pb, l] : queues) if (l.contains (rv.getProperty ("product", "").toString())) pbs.add (pb);
        if (! pbs.isEmpty()) rv.getDynamicObject()->setProperty ("phaseb", pbs.joinIntoString (","));
    }
}
// 15 Oct: what a run that reached the end has LEFT - every queued (phaseb) plugin whose state is not a final outcome (the server's own list)
inline juce::Array<juce::var> leftAtEnd (const juce::Array<juce::var>& rows)
{
    juce::Array<juce::var> left;
    for (const auto& rv : rows)
        if (rv.getProperty ("phaseb", "").toString().isNotEmpty() && ! wizserver::stub::isFinalState (rv.getProperty ("state", "").toString()))
        { auto* o = new juce::DynamicObject(); o->setProperty ("product", rv.getProperty ("product", {})); o->setProperty ("plugin_id", rv.getProperty ("plugin_id", {}));
          o->setProperty ("state", rv.getProperty ("state", {})); o->setProperty ("why", rv.getProperty ("why", {})); left.add (juce::var (o)); }
    return left;
}

// THE SOUND CHECK (Kathy, 15 Oct: bx_opto Pedal and three APB passed digital silence, NEOLD U17 / U2A / V76U73 never processed - each was
// still fully measured, 2-8 min, before the certification filed it). Part of the demo check, on its own steady tone:
//   - SILENT: the input present and the output digital silence in every window -> needs_licence at once (no sound at all);
//   - INERT: the output IS the input (every window within kDrySameDb) - then up to 3 controls the certification's inert check would try
//     (power-type switches first, then gain / output / drive by name: sweep::inertPowerName / inertGainName) are each written to BOTH
//     ends on the same tone; if none moves the output by more than sweep::kInertSameDb, "processing never runs" -> needs_licence at once.
// A plugin that changes the signal at all, or offers no control to try, is never skipped: it is measured (the certification still judges).
namespace soundcheck
{
inline constexpr double kDrySameDb = 0.05, kSilentDb = -120.0, kInputPresentDb = -60.0;
struct Loud { std::vector<double> in, out; };
inline Loud loudOf (const juce::String& trace)
{
    Loud l;
    for (const auto& line : juce::StringArray::fromLines (trace))
    {
        if (! line.startsWith ("bwin\t")) continue;
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        auto val = [&] (const char* k) { const int i = f.indexOf (k); return i >= 0 && i + 1 < f.size() ? f[i + 1] : juce::String(); };
        if (val ("seg") != "loud" || val ("t_ms").getDoubleValue() < wizard::kDemoSettleMs) continue;
        l.in.push_back (val ("in_db").getDoubleValue()); l.out.push_back (val ("out_db").getDoubleValue());
    }
    return l;
}
inline double median (std::vector<double> x) { if (x.empty()) return -999.0; std::sort (x.begin(), x.end()); return x[x.size() / 2]; }
inline double medianOut (const juce::String& trace) { return median (loudOf (trace).out); }
inline bool silent (const juce::String& trace)
{
    const auto l = loudOf (trace); if (l.out.size() < 20 || median (l.in) < kInputPresentDb) return false;
    for (double o : l.out) if (o > kSilentDb) return false;
    return true;
}
inline bool dry (const juce::String& trace)
{
    const auto l = loudOf (trace); if (l.out.size() < 20 || median (l.out) < -100.0) return false;
    for (size_t i = 0; i < l.out.size(); ++i) if (std::abs (l.out[i] - l.in[i]) > kDrySameDb) return false;
    return true;
}
// THE CONTROLS THAT PROVE IT (16 Oct - the 15 Oct night run filed 33 working plugins "never processes": H-EQ, LinEQ, REQ, RChannel, CLA
// MixHub ... - their band On/Off switches, Bypass, "EQ In" and band gains cannot change a flat 997 Hz tone, processing or not): only a
// PLAIN LEVEL control - its whole name made of level words (Output, Input, Volume, Trim, Makeup, Master, Gain, Level), continuous - must
// change the level of any tone. Switches, band / section controls ("Band1 Gain", "LF Gain", "Output Gain M On") never count. No such
// control -> no verdict: the plugin is measured
inline bool plainLevelControl (const juce::String& name)
{
    static const juce::StringArray kLevel { "output", "out", "input", "in", "volume", "vol", "trim", "makeup", "make-up", "master", "gain", "level", "db" };
    static const juce::StringArray kMust { "output", "out", "input", "volume", "vol", "trim", "makeup", "make-up", "master", "gain", "level" };
    auto t = juce::StringArray::fromTokens (name.toLowerCase().replaceCharacters ("()[]:/_-.", "         "), " ", ""); t.removeEmptyStrings();
    if (t.isEmpty()) return false;
    bool must = false; for (const auto& w : t) { if (! kLevel.contains (w)) return false; if (kMust.contains (w)) must = true; }
    return must;
}
inline std::vector<std::pair<int, juce::String>> candidates (const juce::String& listOut)
{
    std::vector<std::pair<int, juce::String>> out;
    for (const auto& line : juce::StringArray::fromLines (listOut))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 6 || ! f[0].containsOnly ("0123456789") || f[0].isEmpty() || f[5] != "1") continue;   // automatable only
        const int idx = f[0].getIntValue(); const auto name = f[1]; const int steps = f[3].getIntValue();
        if (steps != 2 && f[4] != "1" && plainLevelControl (name)) out.push_back ({ idx, name });   // continuous plain level controls only
    }
    if (out.size() > 3) out.resize (3);
    return out;
}
// inert: something was tried and nothing moved the output
inline bool inert (double defaultDb, const std::vector<std::pair<juce::String, double>>& tries, juce::String& why)
{
    if (tries.empty()) { why = "no control to try"; return false; }
    juce::StringArray said; bool moved = false;
    for (const auto& [what, db] : tries) { said.add (what + ": " + juce::String (db - defaultDb, 2) + " dB"); if (std::abs (db - defaultDb) > sweep::kInertSameDb) moved = true; }
    why = (moved ? "it processes (" : "licence suspected: never processes - the output is the input, unchanged by every control tried (") + said.joinIntoString ("; ") + ")";
    return ! moved;
}
}

// THE DEMO CHECK (beta item 2): every plugin about to be measured gets one steady tone (wizard::demoDropouts); cached by plugin_id in
// <out>/demo_checks.json so the measuring run does not render it again. `runner` is the probe (the suite's fake).
inline std::map<juce::String, wizard::DemoVerdict> demoCheckForWizard (const std::vector<juce::PluginDescription>& ds, int jobs, const std::function<ChildResult (const juce::StringArray&)>& runner,
                                                                        const juce::File& probe, const juce::File& cacheFile,
                                                                        const std::function<void (const juce::String&, const wizard::DemoVerdict&, double)>& onChecked = {})
{
    std::map<juce::String, wizard::DemoVerdict> out; auto cache = juce::JSON::parse (cacheFile.loadFileAsString()); if (! cache.isObject()) cache = juce::var (new juce::DynamicObject());
    auto pidOf = [] (const juce::PluginDescription& d) { return "AudioUnit|" + juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version; };
    std::vector<size_t> todo;
    for (size_t i = 0; i < ds.size(); ++i)
    {
        const auto c = cache.getProperty (pidOf (ds[i]), {});
        if (c.isObject()) { wizard::DemoVerdict v; v.judged = (bool) c.getProperty ("judged", false); v.demo = (bool) c.getProperty ("demo", false); v.window = (bool) c.getProperty ("window", false); v.silent = (bool) c.getProperty ("silent", false); v.inert = (bool) c.getProperty ("inert", false); v.why = c.getProperty ("why", "").toString(); out[ds[i].name] = v; }
        else todo.push_back (i);
    }
    std::mutex m;
    jobs::Pool<size_t, juce::StringArray, ChildResult> pool;
    pool.laneOf = [] (const size_t&) { return jobs::Lane::parallel; };
    pool.stopped = [] { return wizardStop() != 0; };
    pool.prepare = [&] (const size_t& i) -> std::optional<juce::StringArray> { juce::StringArray a { probe.getFullPathName(), ds[i].name, ds[i].fileOrIdentifier, juce::String::toHexString (ds[i].uniqueId) }; a.addArray (wizard::demoProbeArgs()); return a; };
    std::map<juce::String, double> started;   // 14 Oct: each check's wall time (load + the offline render + the read), into the cache
    std::map<juce::String, std::vector<std::pair<juce::String, double>>> tries;   // 15 Oct: the sound check's control tries, by plugin
    pool.run = [&] (const juce::StringArray& a)
    {
        { std::lock_guard<std::mutex> g (m); started[a[1]] = juce::Time::getMillisecondCounterHiRes(); }
        auto r = runner (a);
        if (r.cleanExit() && soundcheck::dry (r.out))
        {   // the output IS the input: does any control change it? (only here - a plugin that colours the tone is never asked)
            juce::StringArray base { a[0], a[1], a[2], a[3] }; auto la = base; la.add ("--list-params");
            const auto lr = runner (la); std::vector<std::pair<juce::String, double>> t;
            if (lr.cleanExit()) for (const auto& [idx, name] : soundcheck::candidates (lr.out))
                for (float end : { 0.0f, 1.0f })
                {
                    auto ba = base; ba.addArray (wizard::demoProbeArgs()); ba.add ("set=" + juce::String (idx) + ":" + juce::String (end, 1));
                    const auto br = runner (ba);
                    if (br.cleanExit()) t.push_back ({ name + " -> " + juce::String (end, 1), soundcheck::medianOut (br.out) });
                }
            std::lock_guard<std::mutex> g (m); tries[a[1]] = t;
        }
        return r;
    };
    pool.discard = [] (const size_t&, juce::StringArray&) {};
    pool.finish = [&] (const size_t& i, juce::StringArray&, ChildResult& r)
    {
        auto v = r.cleanExit() ? wizard::demoDropouts (r.out) : wizard::DemoVerdict();   // a probe that failed here is the measurement's business, not a demo
        if (r.cleanExit() && soundcheck::silent (r.out)) { v.silent = true; v.judged = true; v.why = "no sound at all: a steady tone in, digital silence out (activation / licence) - not measured"; }
        else if (r.cleanExit() && soundcheck::dry (r.out))
        {   juce::String why; std::vector<std::pair<juce::String, double>> t; { std::lock_guard<std::mutex> g (m); t = tries[ds[i].name]; }
            if (soundcheck::inert (soundcheck::medianOut (r.out), t, why)) { v.inert = true; v.judged = true; v.why = why + " - not measured"; } }
        if (r.kind == ChildResult::Kind::uiShown) { v.window = true; v.why = "a licence window appeared at the load"; }   // 14 Oct: not loaded again
        std::lock_guard<std::mutex> g (m); out[ds[i].name] = v;
        const double secs = (juce::Time::getMillisecondCounterHiRes() - started[ds[i].name]) / 1000.0;
        if (onChecked) onChecked (ds[i].name, v, secs);
        if (r.cleanExit() || v.window) { auto* o = new juce::DynamicObject(); o->setProperty ("judged", v.judged); o->setProperty ("demo", v.demo); o->setProperty ("window", v.window); o->setProperty ("silent", v.silent); o->setProperty ("inert", v.inert); o->setProperty ("why", v.why); o->setProperty ("seconds", std::round (secs * 100.0) / 100.0); cache.getDynamicObject()->setProperty (pidOf (ds[i]), juce::var (o)); }
    };
    const int demoJobs = jobs::demoJobsFor (jobs);   // 15 Oct: load-bound - twice the run's jobs (at most 6), 2 while the user is active
    pool.jobsNow = [demoJobs] { return jobs::jobsNowForUser (demoJobs); };
    pool (todo, demoJobs);
    cacheFile.replaceWithText (juce::JSON::toString (cache));
    return out;
}

// EVERY CRASH IS REPORTED (16 Oct - the 15 Oct night run: 5 plugins crashed the probe and the wizard moved on, the manifest's "crashed" held
// only the crashes the APP went down with (none), so the receipt said crashed_reported 0): the app's crashes, each with its report, then
// every skipped row filed "crashed" that is not already there - stage "probe", no report (the probe's own log stays on this Mac)
inline juce::Array<juce::var> withProbeCrashes (juce::Array<juce::var> crashed, const juce::Array<juce::var>& skipped)
{
    juce::StringArray have; for (const auto& c : crashed) have.add (c.getProperty ("product", "").toString());
    for (const auto& s : skipped)
    {
        const auto product = s.getProperty ("product", "").toString();
        if (s.getProperty ("state", "").toString() != "crashed" || have.contains (product)) continue;
        auto* c = new juce::DynamicObject(); c->setProperty ("product", product); c->setProperty ("plugin_id", s.getProperty ("plugin_id", {})); c->setProperty ("stage", "probe");
        c->setProperty ("why", s.getProperty ("why", {})); c->setProperty ("crash_report", ""); c->setProperty ("report_found", false);
        crashed.add (juce::var (c)); have.add (product);
    }
    return crashed;
}

// THE SOUND CHECK ALONE (16 Oct, Kathy's safety proof for the inert fix): the wizard's own demoCheckForWizard on the named plugins, into
// <out> only (demo_checks.json + soundcheck.tsv: name, verdict, why). Nothing measured, nothing sent; the names come from a file, one per line
inline int runSoundCheckAlone (const SweepOptions& opt, const juce::File& namesFile, bool allowLicenceBound = false)
{
    installWizardStop(); opt.out.createDirectory();
    juce::StringArray names; names.addLines (namesFile.loadFileAsString()); names.trim(); names.removeEmptyStrings(); names.removeDuplicates (false);
    const auto ds = guardedPlugins (names, allowLicenceBound, "SOUNDCHECK");
    const auto v = demoCheckForWizard (ds, opt.jobs, [&] (const juce::StringArray& a) { return runChild (a, opt.timeoutMs); }, opt.probe, opt.out.getChildFile ("demo_checks.json"),
                                       [] (const juce::String& n, const wizard::DemoVerdict& d, double secs) { std::cout << "SOUNDCHECK: " << n << " (" << juce::String (secs, 1) << " s)" << (d.inert ? " INERT" : d.silent ? " SILENT" : "") << std::endl; });
    juce::String tsv;
    for (const auto& d : ds)
    {   const auto it = v.find (d.name); if (it == v.end()) { tsv << d.name << "\tnot checked\t\n"; continue; }
        const auto& x = it->second;
        tsv << d.name << "\t" << (x.inert ? "inert" : x.silent ? "silent" : x.window ? "window" : x.demo ? "demo" : "measure") << "\t" << x.why << "\n"; }
    opt.out.getChildFile ("soundcheck.tsv").replaceWithText (tsv);
    return 0;
}

// THE VST3 LANE (Kathy, 17 Oct, rulings 3: "measure once per product on AU, then bridge-read the VST3 twin; full VST3 measuring only
// when there is no AU twin"). A VST3 whose class name is an installed AudioUnit's name is a TWIN: it is not measured, its role-named
// controls are read beside the AU's at the same five settings (the bridge read; the server judges it - trust::bridgeFor). One with no
// AU of that name is MEASURED like an AudioUnit (its own map, its own Phase B rows, plugin_id "VST3|uid|version")
inline juce::String vst3Lane (const juce::String& className, const juce::StringArray& auNames, juce::String& twin)
{
    for (const auto& n : auNames) if (n.trim().equalsIgnoreCase (className.trim())) { twin = n; return "bridge"; }
    twin = {}; return "measure";
}
// the bridge's controls: parameters both formats carry under the same name whose name says a role (the trust role table), at most 12;
// (AU index, VST3 index, name)
struct BridgePick { int au = -1, v3 = -1; juce::String name; };
inline std::vector<BridgePick> bridgeControls (const std::vector<std::pair<int, juce::String>>& au, const std::vector<std::pair<int, juce::String>>& v3, int cap = 12)
{
    std::vector<BridgePick> out;
    for (const auto& [ai, an] : au)
    {
        auto* o = new juce::DynamicObject(); o->setProperty ("name", an); const juce::var c (o);
        if (trust::roleOf (c).role.isEmpty()) continue;
        for (const auto& [vi, vn] : v3) if (vn.trim() == an.trim()) { out.push_back ({ ai, vi, an }); break; }
        if ((int) out.size() >= cap) break;
    }
    return out;
}
inline juce::String bridgeRecordName (const juce::PluginDescription& d);
// THE BRIDGE READ'S OUTCOME (Kathy, 18 Oct: 33 twins ended the run "still need measuring"): the wizard compares its two records itself -
// every control read in both formats at BRIDGE_MIN_POINTS+ shared settings, each display the same (numbers within the coarser display's
// last digit, else the same text) -> "bridged"; anything else -> "bridge_no_match", with the first difference. Both are final outcomes;
// the server judges what a bridged VST3 inherits (its role controls, its AU's level)
inline juce::String bridgeVerdict (const juce::var& auRec, const juce::var& v3Rec, juce::String& why)
{
    std::map<juce::String, juce::var> v3; if (const auto* a = v3Rec.getProperty ("controls", {}).getArray()) for (const auto& c : *a) v3[c.getProperty ("name", "").toString()] = c;
    int controls = 0;
    if (const auto* a = auRec.getProperty ("controls", {}).getArray())
        for (const auto& c : *a)
        {
            const auto name = c.getProperty ("name", "").toString(); ++controls;
            if (! v3.count (name)) { why = "'" + name + "' was not read on the VST3"; return "bridge_no_match"; }
            const auto* ad = c.getProperty ("displayAt", {}).getDynamicObject(); const auto* vd = v3[name].getProperty ("displayAt", {}).getDynamicObject();
            int paired = 0;
            if (ad != nullptr && vd != nullptr)
                for (const auto& p : ad->getProperties())
                {
                    if (! vd->hasProperty (p.name)) continue;
                    ++paired; const auto x = p.value.toString().trim(), y = vd->getProperty (p.name).toString().trim(); double a1 = 0.0, b1 = 0.0;
                    const bool same = (trust::numberOf (x, a1) && trust::numberOf (y, b1)) ? std::abs (a1 - b1) <= std::max (trust::displayStepValue (x), trust::displayStepValue (y)) + 1e-9 : x.equalsIgnoreCase (y);
                    if (! same) { why = "'" + name + "' at " + p.name.toString() + ": AU '" + x + "', VST3 '" + y + "'"; return "bridge_no_match"; }
                }
            if (paired < trust::BRIDGE_MIN_POINTS) { why = "'" + name + "': " + juce::String (paired) + " setting(s) read in both formats, " + juce::String (trust::BRIDGE_MIN_POINTS) + " needed"; return "bridge_no_match"; }
        }
    if (controls == 0) { why = "no control read in both formats"; return "bridge_no_match"; }
    why = juce::String (controls) + " control(s) show the same in both formats"; return "bridged";
}
// the AU's role-named parameters (the trust role table), at most 12 - the bridge's controls when the VST3's own list is not in hand
inline std::vector<BridgePick> roleNamedParams (const std::vector<std::pair<int, juce::String>>& au, int cap = 12)
{
    std::vector<BridgePick> out;
    for (const auto& [ai, an] : au) { auto* o = new juce::DynamicObject(); o->setProperty ("name", an); if (trust::roleOf (juce::var (o)).role.isNotEmpty()) out.push_back ({ ai, -1, an }); if ((int) out.size() >= cap) break; }
    return out;
}
// the probe spec for one format: indices where known, else "@<escaped name>" (the probe finds it by name in the same load - VST3 SPEED)
inline juce::String bridgeSpec (const std::vector<BridgePick>& picks, bool vst3Side)
{
    juce::StringArray parts;
    for (const auto& p : picks)
    {
        const int idx = vst3Side ? p.v3 : p.au;
        parts.add ((idx >= 0 ? juce::String (idx) : "@" + juce::URL::addEscapeChars (p.name, false)) + ":0.100000,0.300000,0.500000,0.700000,0.900000");
    }
    return parts.joinIntoString (";");
}
// one bridge read (VST3 SPEED, 17 Oct): the AU's parameter list from its map in the store when there is one (else one list load), the VST3
// addressed by name when its own list is not in hand (identity from moduleinfo or the cache); then both formats at 10 / 30 / 50 / 70 / 90 %
// on the picked controls, one load each, written as read-back records ("purpose": "bridge", tagged by format) under <dir>
inline juce::String bridgeRead (const SweepOptions& opt, const juce::PluginDescription& au, const juce::PluginDescription& v3,
                                std::vector<std::pair<int, juce::String>> auParams, const std::vector<std::pair<int, juce::String>>& v3Params,
                                const juce::File& dir, bool& window, int* loads = nullptr)
{
    window = false; auto count = [&] { if (loads != nullptr) ++*loads; };
    if (auParams.empty())
    {
        const auto lr = runChild ({ opt.probe.getFullPathName(), au.name, au.fileOrIdentifier, juce::String::toHexString (au.uniqueId), "--list-params" }, opt.timeoutMs); count();
        if (lr.kind == ChildResult::Kind::uiShown) { window = true; return "a window appeared on the AU twin (licence?) - closed, not clicked"; }
        auParams = paramsOfList (lr.out);
    }
    const auto picks = v3Params.empty() ? roleNamedParams (auParams) : bridgeControls (auParams, v3Params);
    if (picks.empty()) return "no role-named control in both formats - nothing to read for the bridge";
    dir.createDirectory(); int read = 0;
    for (const bool isV3 : { false, true })
    {
        const auto& d = isV3 ? v3 : au;
        const auto r = runProbeCall ({ opt.probe.getFullPathName(), d.name, d.fileOrIdentifier, juce::String::toHexString (d.uniqueId), "--text-at-norms-multi", bridgeSpec (picks, isV3) }, opt.timeoutMs); count();
        if (r.kind == ChildResult::Kind::uiShown) { window = true; return "a window appeared (licence?) - closed, not clicked"; }
        const auto got = readback::parse (r.out);
        std::vector<readback::Want> ws;   // by what the probe read: a name found in this load carries its index here
        for (const auto& [idx, g] : got) for (const auto& p : picks) if (g.name.trim() == p.name.trim()) { readback::Want w; w.index = idx; w.name = p.name; w.kind = "anchored"; w.norms = { 0.1, 0.3, 0.5, 0.7, 0.9 }; ws.push_back (w); break; }
        if (isV3) read = (int) ws.size();
        auto* id = new juce::DynamicObject(); id->setProperty ("name", d.name); id->setProperty ("version", d.version); id->setProperty ("format", fmtTag (d));
        auto* mm = new juce::DynamicObject(); mm->setProperty ("identity", juce::var (id));
        auto rec = readback::record (juce::var (mm), ws, got, d.version);
        rec.getDynamicObject()->setProperty ("purpose", "bridge"); rec.getDynamicObject()->setProperty ("measuredAt", nowStamp());
        rec.getDynamicObject()->setProperty ("plugin_id", fmtTag (d) + "|" + juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version);
        rec.getDynamicObject()->setProperty ("twin_plugin_id", fmtTag (isV3 ? au : v3) + "|" + juce::String::toHexString ((isV3 ? au : v3).uniqueId).toLowerCase() + "|" + (isV3 ? au : v3).version);
        dir.getChildFile (bridgeRecordName (d)).replaceWithText (juce::JSON::toString (rec) + "\n");
    }
    return juce::String (read) + " control(s) read in both formats";
}
inline juce::String bridgeRecordName (const juce::PluginDescription& d) { return fmtTag (d) + "_" + juce::String::toHexString (d.uniqueId).toLowerCase() + "_" + d.version + ".readback.json"; }
// the AU's parameter list from its map in the store (the wizard's own map: params [{index, name}]) - empty when the store has none
inline std::vector<std::pair<int, juce::String>> paramsFromStore (const juce::File& ledger, const std::map<juce::String, StoreEntry>& store, const juce::PluginDescription& d)
{
    std::vector<std::pair<int, juce::String>> ps;
    const auto k = juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version; const auto it = store.find (k); if (it == store.end()) return ps;
    const auto m = juce::JSON::parse (ledger.getChildFile ("maps").getChildFile (it->second.fp + ".json").loadFileAsString());   // held by name
    if (const auto* a = m.getProperty ("params", {}).getArray()) for (const auto& p : *a) ps.push_back ({ (int) p.getProperty ("index", -1), p.getProperty ("name", "").toString() });
    return ps;
}

inline int runWizard (const WizardOptions& wo)
{
    installWizardStop();
    if (! wo.plan)
    {   // 14 Oct (Kathy's run: two engines kept loading for half an hour after the app was gone): an engine whose parent has gone (adopted by
        // launchd) stops itself the clean way - SIGINT to its own process group, as the app's pause does
        std::thread ([] { for (;;) { std::this_thread::sleep_for (std::chrono::seconds (2)); if (wizapp::orphaned (getppid())) { ::kill (0, SIGINT); return; } } }).detach();
    }
    {   // 13 Oct (beta item 2): never start below the free-disk floor - said in plain words, nothing loaded
        const auto dv = wizapp::diskVerdict (wizapp::freeBytes (wo.opt.out), true);
        if (! dv.ok) { std::cout << "WIZARD: " << dv.words << std::endl; wo.opt.out.createDirectory();
                       auto* p = new juce::DynamicObject(); p->setProperty ("stage", "Stopped"); p->setProperty ("summary", dv.words); p->setProperty ("disk_low", true);
                       wo.opt.out.getChildFile ("progress.json").replaceWithText (juce::JSON::toString (juce::var (p))); return 6; }
    }
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto& opt = wo.opt; const auto out = opt.out; out.createDirectory();
    // THE PROGRESS (11 Oct): the terminal shows one live line + a final summary; every detail line goes to wizard.log (--verbose: as before,
    // details on the terminal and the bar as a line per change); progress.json is what a window app reads
    std::ostream term (std::cout.rdbuf()); std::ofstream logFile; std::streambuf* const coutWas = std::cout.rdbuf();
    struct Restore { std::streambuf* b; ~Restore() { std::cout.rdbuf (b); phaseb::rowHooks() = {}; phaseb::jobsCap() = {}; } } restore { coutWas };
    phaseb::jobsCap() = [] (int base) { return jobs::jobsNowForUser (base); };   // 15 Oct (sharing the Mac): at most 2 at a time while the user is active
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
    const auto dbMultiband = wizroute::loadMultiband (wo.db);
    {   int idx = 0; for (const auto& e : db) idx += e.indexOnly ? 1 : 0;
        say ("WIZARD: " + juce::String ((int) db.size() - idx) + " profile(s) and " + juce::String (idx) + " mapped draft(s) in the database (" + wo.db.getFileName() + ", a local folder: nothing is fetched)"); }
    // THE NEVER-RETRY LIST (the app's crash recovery): a plugin the wizard went down with is not loaded again, by any stage
    const auto skipped = wo.skip != juce::File() ? wizapp::skipList (wo.skip.getParentDirectory()) : juce::StringArray();
    std::vector<InstalledRecord> plugins; juce::Array<juce::var> skippedRows;
    for (const auto& r : registryAudioUnits())   // AU here; the VST3s are found below (installedVst3Bundles)
    {
        if (! (wo.only.isEmpty() || wo.only.contains (r.desc.name))) continue;
        if (skipped.contains (r.desc.name))
        { auto* row = new juce::DynamicObject(); row->setProperty ("product", r.desc.name); row->setProperty ("plugin_id", wizroute::pluginId (juce::String::toHexString (r.desc.uniqueId), r.desc.version, fmtTag (r.desc))); row->setProperty ("format", fmtTag (r.desc));
          row->setProperty ("state", "crashed"); row->setProperty ("why", "the wizard closed while checking it: skipped for good, never retried"); skippedRows.add (juce::var (row));
          say ("  " + r.desc.name + ": on the never-retry list (the wizard closed while checking it) - not loaded"); continue; }
        plugins.push_back (r);
    }
    auto store = mapStore (opt.ledger);
    // THE VST3s (Kathy, 17 Oct, rulings 3; VST3 SPEED the same day): found from their bundles, identified WITHOUT a load wherever the bundle
    // says who it is (moduleinfo.json: class name, CID -> the JUCE uid, version, vendor) or an earlier run did (the identity cache, by bundle
    // path + version, failures too) - vst3IdentityPlan. Only a bundle with neither is loaded, ONCE, and its answer cached. A twin of an
    // installed AudioUnit is bridge-read (the AU's list from the store when it is mapped, the VST3 by name when its list is not in hand;
    // never twice for one version); one with no twin joins the run as its own plugin (EJ_VST3_EXTRAS, installedPlugins). The same licence /
    // PACE / hardware checks as an AudioUnit come before any load
    juce::Array<juce::var> vst3Rows; juce::Array<juce::var> vst3Extras;
    if (! wo.plan && std::getenv ("EJ_WIZARD_NO_VST3") == nullptr)
    {
        std::map<juce::String, juce::PluginDescription> auByName; juce::StringArray auNames; std::vector<std::pair<juce::String, juce::String>> auPairs;
        for (const auto& r : registryAudioUnits()) { auByName[r.desc.name.toLowerCase()] = r.desc; auNames.add (r.desc.name); auPairs.push_back ({ r.desc.name, r.desc.manufacturerName }); }
        const auto licF = out.getChildFile ("licence_windows.json"); const auto licMem = wizroute::loadLicenceMemory (licF);
        const bool paceOk = juce::File ("/Library/Application Support/PACEAntiPiracy/LicenseSupport").exists() || juce::File ("/Applications/iLok License Manager.app").exists();
        Vst3IdentityCache idCache (opt.ledger.getChildFile ("vst3_identity.json"));
        const auto bridgeDir = out.getChildFile ("phaseb").getChildFile ("readback").getChildFile ("bridge");
        auto vrow = [&] (const juce::String& product, const juce::String& pid, const juce::String& st, const juce::String& why)
        { auto* row = new juce::DynamicObject(); row->setProperty ("product", product); row->setProperty ("format", "VST3"); row->setProperty ("lane", "vst3"); if (pid.isNotEmpty()) row->setProperty ("plugin_id", pid);   // 18 Oct: lane - never given its AU twin's queue
          row->setProperty ("state", st); row->setProperty ("why", why); vst3Rows.add (juce::var (row)); return row; };
        const auto plan = vst3IdentityPlan (installedVst3Bundles(), auPairs, idCache);
        std::map<juce::String, int> by; int identityLoads = 0, bridgeLoads = 0, bridgesKept = 0;
        for (const auto& pr : plan)
        {
            if (wizardStop() != 0) break;
            const auto& b = pr.b; const auto path = b.bundle.getFullPathName(), bundleKey = "vst3-bundle|" + path.toLowerCase();
            auto matchesOnly = [&] (const juce::String& n) { return std::any_of (wo.only.begin(), wo.only.end(), [&] (const juce::String& o) { return o.equalsIgnoreCase (n) || n.containsIgnoreCase (o); }); };
            if (! wo.only.isEmpty() && ! matchesOnly (b.name) && std::none_of (pr.ids.begin(), pr.ids.end(), [&] (const Vst3Identity& i) { return matchesOnly (i.name); })) continue;
            ++by[pr.source];
            if (pr.source == "shell") { vrow (b.name, {}, "out_of_scope", "a WaveShell VST3 bundle: its plugins are measured through their AudioUnits"); continue; }
            if (pr.source == "cache_failed") { const auto e = idCache.get (pr.key); vrow (b.name, {}, e.getProperty ("state", "").toString(), e.getProperty ("why", "").toString() + " (remembered at this version: not loaded again)"); continue; }
            if (skipped.contains (b.name)) { vrow (b.name, {}, "crashed", "the wizard closed while checking it: skipped for good, never retried"); continue; }
            if (licMem.count (bundleKey)) { vrow (b.name, {}, "needs_licence", wizroute::licenceSkipWhy (wizroute::LicenceSkip::remembered)); continue; }
            juce::PluginDescription d0; d0.pluginFormatName = "VST3"; d0.name = b.name; d0.fileOrIdentifier = path; d0.version = b.version; d0.manufacturerName = b.vendor;
            juce::String pw; const bool pace = paceHeld (d0, {}, pw) && pw.startsWith ("PACE");
            if (pace && ! paceOk) { vrow (b.name, {}, "needs_licence", wizroute::licenceSkipWhy (wizroute::LicenceSkip::paceNoIlok)); continue; }
            if (apb::isApbProduct (d0.name, d0.manufacturerName)) { const auto& a = apb::device (apb::assumeFlag()); if (! a.present) { vrow (b.name, {}, "needs_hardware", juce::String (apb::kNotConnected) + " (" + a.how + "); not loaded"); continue; } }
            auto ids = pr.ids; std::vector<std::pair<int, juce::String>> ps;
            if (pr.source == "load")   // neither the bundle nor an earlier run says who it is: ONE load, and its answer (or its failure) is kept
            {
                const auto r = runChild ({ opt.probe.getFullPathName(), d0.name, path, "0", "--list-params" }, opt.timeoutMs); ++identityLoads;
                if (r.kind == ChildResult::Kind::uiShown) { wizroute::rememberLicenceWindow (licF, bundleKey); idCache.put (pr.key, {}, "needs_licence", "a window appeared (licence?) - closed, not clicked"); vrow (b.name, {}, "needs_licence", "a window appeared (licence?) - closed, not clicked"); continue; }
                ps = paramsOfList (r.out); const auto cls = readback::vst3ClassOf (r.out), uid = readback::vst3UidOf (r.out), ver = readback::vst3VersionOf (r.out);
                if (! r.cleanExit() || ps.empty() || uid.isEmpty())
                { const auto st = r.cleanExit() ? juce::String ("wont_load") : juce::String ("crashed"), why = "the VST3 did not load (" + (r.cleanExit() ? juce::String ("no parameters") : r.describe()) + ")";
                  idCache.put (pr.key, {}, st, why); vrow (b.name, {}, st, why); continue; }
                Vst3Identity id; id.name = cls.isNotEmpty() ? cls : b.name; id.version = ver.isNotEmpty() ? ver : b.version; id.uid = (int) (juce::int64) uid.getHexValue64(); id.source = "load";
                id.vendor = readback::vst3FieldOf (r.out, "vendor").isNotEmpty() ? readback::vst3FieldOf (r.out, "vendor") : b.vendor;
                ids = { id }; idCache.put (pr.key, ids);
            }
            else if (pr.source == "moduleinfo") idCache.put (pr.key, ids);   // the metadata's answer, kept with the rest
            for (const auto& id : ids)
            {
                juce::PluginDescription d = d0; d.name = id.name; d.version = id.version; d.uniqueId = d.deprecatedUid = id.uid; if (id.vendor.isNotEmpty()) d.manufacturerName = id.vendor;
                const auto uid = juce::String::toHexString (id.uid).toLowerCase(); const auto pid = wizroute::pluginId (uid, d.version, "VST3");
                juce::String twin;
                if (vst3Lane (d.name, auNames, twin) == "bridge")
                {
                    const auto& au = auByName[twin.toLowerCase()];
                    const auto twinPid = wizroute::pluginId (juce::String::toHexString (au.uniqueId), au.version, "AudioUnit");
                    if (bridgeDir.getChildFile (bridgeRecordName (d)).existsAsFile())   // read on an earlier run at this version: not loaded again
                    {   // read on an earlier run: its outcome from the two records on disk
                        juce::String vwhy; const auto v = bridgeVerdict (juce::JSON::parse (bridgeDir.getChildFile (bridgeRecordName (au)).loadFileAsString()), juce::JSON::parse (bridgeDir.getChildFile (bridgeRecordName (d)).loadFileAsString()), vwhy);
                        auto* row = vrow (d.name, pid, v, vwhy + " (read on an earlier run, not loaded again)"); row->setProperty ("twin_plugin_id", twinPid); ++bridgesKept; continue; }
                    juce::String apw; if (! paceOk && paceHeld (au, componentBundles(), apw) && apw.startsWith ("PACE")) { vrow (d.name, pid, "needs_licence", "its AU twin is iLok-protected and this Mac has no iLok: not read"); continue; }
                    bool window = false;
                    const auto why = bridgeRead (opt, au, d, paramsFromStore (opt.ledger, store, au), ps, bridgeDir, window, &bridgeLoads);
                    if (window) wizroute::rememberLicenceWindow (licF, bundleKey);
                    juce::String st = window ? juce::String ("needs_licence") : juce::String ("not_measurable"), rowWhy = why;
                    if (! window && why.endsWith ("in both formats") && ! why.startsWith ("0 "))   // 18 Oct: the read's own outcome, never a bare "read"
                        st = bridgeVerdict (juce::JSON::parse (bridgeDir.getChildFile (bridgeRecordName (au)).loadFileAsString()), juce::JSON::parse (bridgeDir.getChildFile (bridgeRecordName (d)).loadFileAsString()), rowWhy);
                    auto* row = vrow (d.name, pid, st, rowWhy);
                    row->setProperty ("twin_plugin_id", twinPid); row->setProperty ("identity_from", id.source);
                    say ("  " + d.name.paddedRight (' ', 30) + " VST3 twin of the AU (identity: " + id.source + "): " + why); continue;
                }
                if (! ps.empty())   // loaded just now: its parameter list is the map (the mapping step need not load it again)
                {
                    const auto fp = echojay::fingerprintForDescription (d, (int) ps.size()); const auto cat = wizard::wizardCategory (d.name, catalogue);
                    opt.ledger.getChildFile ("maps").createDirectory();
                    opt.ledger.getChildFile ("maps").getChildFile (fp + ".json").replaceWithText (juce::JSON::toString (wizard::paramMap (d.name, d.manufacturerName, uid, d.version, fp, ps, cat, "VST3")) + "\n");
                    store[uid + "|" + d.version] = { fp, cat };
                }
                plugins.push_back ({ d, echojay::identityKeyForDescription (d), "VST3|" + uid });
                auto* x = new juce::DynamicObject(); x->setProperty ("name", d.name); x->setProperty ("file", path); x->setProperty ("uid", uid); x->setProperty ("version", d.version); x->setProperty ("vendor", d.manufacturerName);
                vst3Extras.add (juce::var (x));
                say ("  " + d.name.paddedRight (' ', 30) + " VST3 with no AU twin (identity: " + id.source + "): joins the run as its own plugin");
            }
        }
        say ("WIZARD: VST3 - " + juce::String ((int) plan.size()) + " bundle(s); identity from moduleinfo " + juce::String (by["moduleinfo"]) + ", from the cache " + juce::String (by["cache"])
             + ", loaded " + juce::String (identityLoads) + " (now cached), remembered failing " + juce::String (by["cache_failed"]) + ", WaveShell " + juce::String (by["shell"])
             + "; bridge reads: " + juce::String (bridgeLoads) + " load(s), " + juce::String (bridgesKept) + " kept from an earlier run");
        const auto xf = out.getChildFile ("vst3_extras.json"); xf.replaceWithText (juce::JSON::toString (juce::var (vst3Extras)) + "\n");
        setenv ("EJ_VST3_EXTRAS", xf.getFullPathName().toRawUTF8(), 1);
    }
    prog.setPlugins ((int) plugins.size());
    const auto bundlesAll = componentBundles();
    setenv ("EJ_WIZARD_BACKGROUND", "1", 1); jobs::beBackgroundGuest();   // 15 Oct (sharing the Mac): this engine and every child it starts run as background guests
    setenv ("EJ_UAD_NATIVE", "1", 1);   // 15 Oct: UAD by sound, not by device - this process and every child it starts (EjmapUadPreflight.h)
    // THE HARDWARE CHECK AT THE SCAN (Kathy, 15 Oct): an APB plugin without its unit passes no sound (UAD: see EJ_UAD_NATIVE above),
    // so they are filed needs_hardware before anything loads - on every route that measures or reads sound (measure, the read-back check,
    // the signature reads). On 14 Oct only the read-back route asked (18 UAD); 158 more UAD and 6 APB were counted "to measure", then
    // Phase B left the UAD unrun and measured the APB into silence for 3-8 min each
    auto hardwareStop = [&] (const juce::PluginDescription& dd) -> juce::String
    {
        // UAD is NOT here (15 Oct, Kathy: native UADx / Spark need no hardware; no file tells them apart) - the sound check decides
        if (apb::isApbProduct (dd.name, dd.manufacturerName)) { const auto& a = apb::device (apb::assumeFlag()); if (! a.present) return juce::String (apb::kNotConnected) + " (" + a.how + "); not loaded"; }
        return {};
    };
    auto serialOf = [&] (const juce::PluginDescription& d) { jobs::LaneInputs in; in.uad = uad::isUadProduct (d.name, d.manufacturerName); in.pace = paceHeld (d, bundlesAll, in.paceWhy); return jobs::laneFor (in) == jobs::Lane::serial; };
    // NO PLUGIN LEFT BEHIND (12 Oct, EjmapWizardRoute.h): (a) the database first - nothing it settles is loaded; (b) the server's category;
    // (c) the name rules; (d) out of scope by type / words, not loaded; (e) only what is left gets EJ Map's signature reads
    const auto pending = wizroute::loadPending (wo.db);
    const auto expectedWork = wizprog::ExpectedWork::load (wo.db);   // 14 Oct: the bar's weights (Sean's seconds per plugin and kind)
    {   // THE REFERENCE STATES (custom defaults, 12 Oct): the server returns each plugin's agreed opening block with /lookup - STUB: the
        // database folder's references/ - and Phase B checks every plugin against it before measuring (EjmapCustomDefault.h)
        const auto refs = wo.db.getChildFile ("references"); const auto dst = out.getChildFile ("references");
        if (refs.isDirectory()) { dst.createDirectory(); for (const auto& f : refs.findChildFiles (juce::File::findFiles, false, "*.reference.json")) f.copyFileTo (dst.getChildFile (f.getFileName())); }
    }
    auto* state = new juce::DynamicObject(); juce::Array<juce::var> rows; std::map<juce::String, juce::StringArray> queues; std::map<juce::String, int> counts;   // queues: Phase B category -> products
    for (const auto& r : skippedRows) { rows.add (r); ++counts["crashed"]; }
    for (const auto& r : vst3Rows) { rows.add (r); ++counts[r.getProperty ("state", "").toString()]; }   // 17 Oct: the VST3 lane's own rows (twins, shells, skips)
    const auto problemState = [] (const juce::String& st) { return wizprog::isProblemState (st); };
    struct Pending { size_t i; wizroute::Decision d; juce::DynamicObject* row; };
    std::vector<Pending> toMap, toCategorise;
    auto typeOf = [] (const juce::PluginDescription& d) { return d.pluginFormatName == "VST3" ? juce::String ("aufx") : d.fileOrIdentifier.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf (",", false, false); };   // a VST3 is an effect here
    std::set<juce::String> limiterKnown;   // 13 Oct: limiters whose limiter pass the database holds (their compressor pass alone is queued)
    auto enqueue = [&] (const juce::String& name, const juce::String& cat, const juce::StringArray& passes = {}, bool limKnown = false)
    { for (const auto& pb : passes.isEmpty() ? wizroute::phaseBFor (cat) : passes) queues[pb].addIfNotAlreadyThere (name); if (limKnown) limiterKnown.insert (name); };
    auto queueFile = [&] (const juce::String& pb) { return out.getChildFile (pb == "wizard" ? juce::String ("wizard_queue.txt") : pb == "wizard_eq" ? juce::String ("wizard_queue_eq.txt") : "wizard_queue_" + pb + ".txt"); };
    auto queuedAnywhere = [&] { for (const auto& [pb, l] : queues) if (! l.isEmpty()) return true; return false; };
    // the measuring items, in the measuring order (Kathy, 12 Oct): one per product and Phase B category (a limiter has two)
    // 14 Oct: each product's DEMO CHECK is an item too (kind "demo"), in the group of its first pass - the estimate and the bar carry it
    auto measureItems = [&] { std::vector<wizprog::Item> q; int gi = 0; std::set<juce::String> demoQueued;
        for (const auto& grp : wizroute::measureOrder()) { for (const auto& pb : grp) if (queues.count (pb)) for (const auto& n : queues[pb])
        for (const auto& ir : plugins) if (ir.desc.name == n)
        {
            if (demoQueued.insert (n).second) { wizprog::Item d; d.product = n; d.kind = "demo"; d.serial = serialOf (ir.desc); d.group = gi; q.push_back (d); }
            wizprog::Item it; it.product = n; it.kind = wizroute::kindOf (pb); it.serial = serialOf (ir.desc); it.group = gi; it.weight = expectedWork.secondsFor (juce::String::toHexString (ir.desc.uniqueId), it.kind);
            it.expectedSteps = expectedWork.procsFor (juce::String::toHexString (ir.desc.uniqueId), it.kind);   // 15 Oct: steps within a plugin (its probe calls)
            it.stepDir = out.getChildFile ("phaseb").getChildFile (pb).getChildFile (".tmp-" + fmtTag (ir.desc) + "_" + juce::String::toHexString (ir.desc.uniqueId).toLowerCase() + "_" + ir.desc.version).getFullPathName();
            q.push_back (it); break;
        }
        ++gi; } return q; };
    auto landedRow = [&] (const juce::String& pb, const juce::String& product) -> juce::var {
        for (const auto& f : out.getChildFile ("phaseb").getChildFile (pb).findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
        { auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("product", "").toString() == product) { r.getDynamicObject()->setProperty ("_stem", f.getFileName().replace (".phaseb.json", "")); return r; } }
        return {}; };
    std::vector<wizprog::Item> planItems;
    // LICENCE WINDOWS (14 Oct): what raised one here before, and whether this Mac has any iLok (USB key, or PACE's licence software)
    const auto licenceMemF = out.getChildFile ("licence_windows.json"); const auto licenceMem = wizroute::loadLicenceMemory (licenceMemF);
    const bool paceSupport = juce::File ("/Library/Application Support/PACEAntiPiracy/LicenseSupport").exists() || juce::File ("/Applications/iLok License Manager.app").exists();
    int ilokUsbState = -1;   // read once, only if a PACE-wrapped plugin needs it (system_profiler takes a second or two)
    auto ilokUsb = [&] { if (ilokUsbState < 0) ilokUsbState = iLokPresence().startsWith ("present") ? 1 : 0; return ilokUsbState == 1; };
    // THE WAVESHELL GUARD (14 Oct): a plugin's shell from its bundle (no load); WaveShell loads give up sooner; a bad shell's plugins skipped
    wizroute::ShellGuard shells;
    auto shellOfId = [&] (const juce::String& fileOrIdentifier) { const auto it = bundlesAll.find (fileOrIdentifier.fromLastOccurrenceOf ("/", false, false));
                                                                 return it == bundlesAll.end() ? juce::String() : wizroute::waveShellOf (it->second.getFileName()); };
    const int waveShellLimitMs = [] { const char* e = std::getenv ("EJ_WIZARD_WAVESHELL_TIMEOUT_MS"); return e != nullptr && *e ? juce::jmax (100, juce::String (e).getIntValue()) : wizroute::kWaveShellLoadTimeoutMs; }();   // rehearsals: force a hang
    auto loadTimeoutFor = [&] (const juce::String& fileOrIdentifier) { return shellOfId (fileOrIdentifier).isNotEmpty() ? juce::jmin (opt.timeoutMs, waveShellLimitMs) : opt.timeoutMs; };
    std::map<juce::String, juce::String> earlyWhy;   // product -> the words for a state settled while loading (a bad shell)
    // 14 Oct: paceHeld reads a bundle's whole binary - WaveShell2-AU 12.0 hosts 589 plugins, so it was read 589 times (a plan of this Mac
    // took over 20 min); the answer is kept per bundle, each binary read once
    std::map<juce::String, std::pair<bool, juce::String>> paceByBundle;
    auto paceWrapped = [&] (const juce::PluginDescription& dd, juce::String& why)
    {
        if (wizroute::paceOptionalVendor (dd.manufacturerName)) return false;
        const auto it = bundlesAll.find (dd.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false));
        const auto key = it != bundlesAll.end() ? it->second.getFullPathName() : dd.fileOrIdentifier;
        if (const auto c = paceByBundle.find (key); c != paceByBundle.end()) { why = c->second.second; return c->second.first; }
        const bool held = paceHeld (dd, bundlesAll, why) && why.startsWith ("PACE-wrapped");
        paceByBundle[key] = { held, why }; return held;
    };
    // 1. THE DATABASE CHECK (Scanning): one answer per plugin; only rule B's quick read-back check loads anything here
    // 14 Oct (Kathy's stuck run): the database step loads NOTHING now - the read-back checks moved into Checking (below) - so Scanning has no
    // items; the scan screen's one count lives in the Checking stage
    prog.stage (wizprog::Stage::scanning, {});
    struct QuickPending { size_t i; wizroute::Decision d; juce::DynamicObject* row; const wizard::DbEntry* entry; };
    std::vector<QuickPending> toQuick;
    const auto quickCacheF = out.getChildFile ("quick_checks.json"); auto quickCache = juce::JSON::parse (quickCacheF.loadFileAsString()); if (! quickCache.isObject()) quickCache = juce::var (new juce::DynamicObject());
    for (size_t i = 0; i < plugins.size(); ++i)
    {
        const auto& ir = plugins[i]; const auto uid = juce::String::toHexString (ir.desc.uniqueId).toLowerCase(); const auto ver = ir.desc.version;
        const auto se = store.count (uid + "|" + ver) ? store.at (uid + "|" + ver) : StoreEntry();
        const auto ans = wizroute::ask (db, pending, catalogue, uid, ver, se.fp, ir.desc.name);
        const auto d = wizroute::route (ans, wizard::wizardCategory (ir.desc.name, juce::var()), categorise::outOfScope (typeOf (ir.desc), ir.desc.name, {}), wizroute::isMultiband (dbMultiband, uid), wizroute::surroundLayout (ir.desc.name));
        auto* row = new juce::DynamicObject(); row->setProperty ("product", ir.desc.name); row->setProperty ("plugin_id", wizroute::pluginId (uid, ver, fmtTag (ir.desc))); row->setProperty ("format", fmtTag (ir.desc)); row->setProperty ("map_fp", se.fp);
        row->setProperty ("category", d.category); row->setProperty ("route", wizroute::routeName (d.route)); row->setProperty ("vendor", ir.desc.manufacturerName);
        if (wo.plan) row->setProperty ("identifier", ir.desc.fileOrIdentifier);   // 14 Oct (WaveShell census): the AU type,subtype,manufacturer - plan only
        juce::String st, why = d.why;
        // LICENCE WINDOWS (14 Oct): looked up before any load - a plugin that showed one here before, or iLok-protected on a Mac with no
        // iLok, is skipped unloaded. Nothing the database settles is ever loaded (A / pending / out of scope stay as they are)
        // the PACE read only where it can matter: the skip rule needs it only on a Mac with no PACE software; a plan reports it
        juce::String paceWhy; const bool pw = ! wizroute::loadsNothing (d) && (wo.plan || ! paceSupport) && paceWrapped (ir.desc, paceWhy);
        row->setProperty ("pace", pw ? paceWhy : juce::String());
        const auto ls = wizroute::loadsNothing (d) ? wizroute::LicenceSkip::none
                      : wizroute::licenceSkip (licenceMem.count (wizroute::pluginId (uid, ver, fmtTag (ir.desc)).toLowerCase()) > 0, pw, pw && ! paceSupport && ilokUsb(), paceSupport);
        const auto hw = wizroute::needsSoundHardware (d.route) ? hardwareStop (ir.desc) : juce::String();
        if (ls != wizroute::LicenceSkip::none) { st = "needs_licence"; why = wizroute::licenceSkipWhy (ls); row->setProperty ("licence_skip", ls == wizroute::LicenceSkip::remembered ? "remembered" : "pace_no_ilok"); }
        else if (hw.isNotEmpty()) { st = "needs_hardware"; why = hw; }   // 15 Oct: UAD without a UAD-2 device, APB without its unit - on every route that measures or reads sound, not loaded
        else switch (d.route)
        {
            case wizroute::Route::skipHave: st = "ready"; break;
            case wizroute::Route::skipPending: st = "pending_review"; break;
            case wizroute::Route::outOfScope: st = "out_of_scope"; break;
            case wizroute::Route::quickCheck:
            {
                const auto gate = licenceGate (opt, ir.desc.name, ir.desc.manufacturerName, out, false);
                if (gate.stop.isNotEmpty()) { st = gate.stop.contains (uad::kNotConnected) ? "needs_hardware" : "needs_licence"; why = gate.stop; break; }
                // 14 Oct: no load here - a read-back already done (quick_checks.json, by plugin id) is reused; the rest wait for Checking
                row->setProperty ("from_version", ans.entry->version());
                const auto qc = quickCache.getProperty (juce::Identifier (wizroute::pluginId (uid, ver, fmtTag (ir.desc))), {});
                if (qc.isObject() && qc.getProperty ("from_version", "").toString() == ans.entry->version())
                {
                    const bool pass = (bool) qc.getProperty ("pass", false); const auto rbWhy = qc.getProperty ("why", "").toString(); row->setProperty ("readback", rbWhy);
                    if (pass && qc.getProperty ("near_matches", {}).size() > 0) { row->setProperty ("matched_with_tolerance", true); row->setProperty ("near_matches", qc.getProperty ("near_matches", {})); }
                    if (! wizapp::needsMeasuring (wizard::Match::sameControls, pass)) { st = "ready_from_version"; why = "the version " + ans.entry->version() + " profile: " + rbWhy; }
                    else { st = "queued"; why = "the version " + ans.entry->version() + " profile does not fit (" + rbWhy + "): measure this version"; toMap.push_back ({ i, d, row }); }
                }
                else if (wo.plan) { st = "quick_check"; why = "(plan: not run) " + why; }   // a plan reads the cache, never loads
                else { st = "checking"; toQuick.push_back ({ i, d, row, ans.entry }); }
                break;
            }
            case wizroute::Route::measure: st = "queued"; toMap.push_back ({ i, d, row }); break;
            case wizroute::Route::sendOther: st = "other_category"; toMap.push_back ({ i, d, row }); break;
            case wizroute::Route::held: st = "held"; toMap.push_back ({ i, d, row }); break;   // multiband: mapped, listed, not measured
            case wizroute::Route::categorise: st = "identifying"; toCategorise.push_back ({ i, d, row }); break;
            case wizroute::Route::needsCategory: st = "needs_category"; break;
        }
        row->setProperty ("state", st); row->setProperty ("why", why); rows.add (juce::var (row));
        if (wo.plan && (d.route == wizroute::Route::measure || d.route == wizroute::Route::quickCheck) && (st == "queued" || st == "quick_check" || st == "checking"))   // 15 Oct: only what will load - not needs_hardware / needs_licence   // a cached passing read-back measures nothing
        {   // THE PLAN's estimate (13 Oct): the same items and per-category seconds the scan-done screen uses (a quick check counted as a measure)
            bool demoAdded = false;   // 14 Oct: its demo check, in the group of its first pass
            for (const auto& pb : d.passes.isEmpty() ? wizroute::phaseBFor (d.category) : d.passes)
            { wizprog::Item it; it.product = ir.desc.name; it.kind = wizroute::kindOf (pb); it.serial = serialOf (ir.desc);
              if (! demoAdded) { demoAdded = true; wizprog::Item dm = it; dm.kind = "demo"; for (int g = 0; g < (int) wizroute::measureOrder().size(); ++g) if (wizroute::measureOrder()[(size_t) g].contains (pb)) dm.group = g; planItems.push_back (dm); }
              for (int g = 0; g < (int) wizroute::measureOrder().size(); ++g) if (wizroute::measureOrder()[(size_t) g].contains (pb)) it.group = g;
              it.weight = expectedWork.secondsFor (juce::String::toHexString (ir.desc.uniqueId), it.kind); planItems.push_back (it); }
        }
        prog.finished (ir.desc.name, problemState (st));
        say ("  " + ir.desc.name.paddedRight (' ', 30) + " " + ver.paddedRight (' ', 9) + " -> " + wizroute::routeName (d.route) + " / " + st + ": " + why);
    }
    if (wo.plan)
    {   // THE PLAN (13 Oct): the database step's answer for every plugin, nothing loaded; counts by plan word and by route
        std::map<juce::String, int> byWord, byRoute;
        for (const auto& r : rows) { const auto rt = r.getProperty ("route", "").toString(); ++byRoute[rt];
            for (int k = 0; k <= (int) wizroute::Route::held; ++k) if (wizroute::routeName ((wizroute::Route) k) == rt) ++byWord[wizroute::planWord ((wizroute::Route) k)]; }
        auto* p = new juce::DynamicObject(); const juce::var pv (p); auto* w = new juce::DynamicObject(); auto* ro = new juce::DynamicObject();
        for (const auto& [k, n] : byWord) w->setProperty (k, n); for (const auto& [k, n] : byRoute) ro->setProperty (k, n);
        { std::set<juce::String> distinct; for (const auto& it : planItems) distinct.insert (it.product);
          p->setProperty ("to_measure", (int) distinct.size()); p->setProperty ("measurements", (int) std::count_if (planItems.begin(), planItems.end(), [] (const wizprog::Item& i) { return i.kind != "demo"; }));
          p->setProperty ("demo_checks", (int) std::count_if (planItems.begin(), planItems.end(), [] (const wizprog::Item& i) { return i.kind == "demo"; })); p->setProperty ("jobs", opt.jobs);
          p->setProperty ("to_measure_eta_s", std::round (wizprog::stageSecondsLeft (planItems, wizprog::Times(), opt.jobs))); }
        p->setProperty ("schema", "ej_wizard_plan/1"); p->setProperty ("plugins", rows.size()); p->setProperty ("by_word", juce::var (w)); p->setProperty ("by_route", juce::var (ro)); p->setProperty ("rows", rows);
        out.getChildFile ("plan.json").replaceWithText (juce::JSON::toString (pv));
        juce::String line = "WIZARD PLAN: " + juce::String (rows.size()) + " plugin(s):"; for (const auto& [k, n] : byWord) line << " " << k << " " << n;
        term << line << std::endl; say (line);
        return 0;
    }
    auto stoppedInScan = [&] (const juce::String& where)
    {   // 13 Oct: a stop while Scanning / Checking - the maps and signature reads that finished stay; nothing is filed for what was in flight
        const auto [done, total] = prog.stageDone();
        say ("WIZARD: stopped while " + where + " (" + juce::String (done) + " of " + juce::String (total) + " checked): nothing filed for the plugin(s) in flight; the next run carries on");
        prog.finish (scanStoppedWords (done, total), true);
        return 130;
    };
    // 2. CHECKING - ONE COUNT FOR THE SCAN SCREEN (Kathy, 14 Oct: "75 of 1376", the bar empty, sat on CLA-76): N = every plugin except the
    // ones out of scope by type / name that load nothing (the 91 here) - 1285 on this Mac. What the database settled is done from the first
    // frame; the rest finish as their loads finish: the read-back checks, the maps, the signature reads. The bar never goes back
    std::set<juce::String> pendingLoad;   // products with a scan load still to come
    for (const auto& q : toQuick) pendingLoad.insert (plugins[q.i].desc.name);
    {
        const auto cached = juce::JSON::parse (out.getChildFile ("categorised.json").loadFileAsString());
        auto have = [&] (const juce::PluginDescription& dd) { return store.count (juce::String::toHexString (dd.uniqueId).toLowerCase() + "|" + dd.version) > 0; };
        for (const auto& p : toMap) if (! have (plugins[p.i].desc)) pendingLoad.insert (plugins[p.i].desc.name);
        for (const auto& p : toCategorise) if (! have (plugins[p.i].desc) || ! cached.hasProperty (p.row->getProperty ("plugin_id").toString())) pendingLoad.insert (plugins[p.i].desc.name);
        std::vector<wizprog::Item> its; std::vector<juce::String> settled;
        for (const auto& ir : plugins)
        {
            const auto name = ir.desc.name;
            if (! wizapp::scanCounts (categorise::outOfScope (typeOf (ir.desc), name, {}) + wizroute::surroundLayout (name), pendingLoad.count (name) > 0)) continue;   // 15 Oct: a surround variant loads nothing and is not counted
            wizprog::Item it; it.product = name; it.kind = "check"; its.push_back (it); if (! pendingLoad.count (name)) settled.push_back (name);
        }
        prog.stage (wizprog::Stage::checking, its);
        for (const auto& n : settled) prog.alreadyDone (n);   // the database answered, or a resumed scan already has it
    }
    // the read-back checks (rule B): a load each, stoppable, cached by plugin id - the measuring run never repeats them
    // 15 Oct: the warm probe here too (one process per plugin, a new instance per call; A/B in the commit) - this thread only
    { probeserve::ScopedServe warmReadBacks (opt.probeServe);
    for (const auto& q : toQuick)
    {
        if (wizardStop() != 0) return stoppedInScan ("checking");
        const auto& ir = plugins[q.i]; auto* row = q.row; const auto uid = juce::String::toHexString (ir.desc.uniqueId).toLowerCase(); const auto ver = ir.desc.version;
        juce::String st, why;
        const auto qshell = shellOfId (ir.desc.fileOrIdentifier);
        if (shells.isBad (qshell)) { st = "waves_skipped"; why = wizroute::wavesSkippedWhy (qshell, shells.why (qshell)); }
        else
        {
            prog.started (ir.desc.name);
            auto qopt = opt; qopt.timeoutMs = loadTimeoutFor (ir.desc.fileOrIdentifier);
            ChildResult::Kind qk = ChildResult::Kind::exited;
            bool window = false; const auto rb = quickCheck (qopt, ir.desc, q.entry->profile, window, &qk);
            if (wizardStop() != 0) return stoppedInScan ("checking");   // a check cut short by the stop is never filed or cached
            if (shells.report (qshell, qk == ChildResult::Kind::timedOut, qk == ChildResult::Kind::signaled)) say ("WIZARD: " + qshell + " " + shells.why (qshell) + " - its other Waves plugins are skipped this scan (check Waves Central)");
            row->setProperty ("readback", rb.why); row->setProperty ("readback_mode", rb.mode); row->setProperty ("readback_loads", rb.loads);
            const auto nearV = wizard::nearMatchesVar (rb.near);
            if (rb.pass && ! rb.near.empty()) { row->setProperty ("matched_with_tolerance", true); row->setProperty ("near_matches", nearV); }
            if (qshell.isNotEmpty() && (qk == ChildResult::Kind::timedOut || shells.isBad (qshell)) && qk != ChildResult::Kind::exited)
            { st = "waves_skipped"; why = wizroute::wavesSkippedWhy (qshell, shells.why (qshell).isNotEmpty() ? shells.why (qshell) : juce::String ("did not load")); }
            else if (window) { st = "needs_licence"; why = rb.why; wizroute::rememberLicenceWindow (licenceMemF, wizroute::pluginId (uid, ver, fmtTag (ir.desc))); }
            else
            {
                {   auto* c = new juce::DynamicObject(); c->setProperty ("from_version", q.entry->version()); c->setProperty ("pass", rb.pass); c->setProperty ("why", rb.why);
                    if (rb.pass && ! rb.near.empty()) c->setProperty ("near_matches", nearV);
                    quickCache.getDynamicObject()->setProperty (wizroute::pluginId (uid, ver, fmtTag (ir.desc)), juce::var (c)); quickCacheF.replaceWithText (juce::JSON::toString (quickCache)); }
                if (! wizapp::needsMeasuring (wizard::Match::sameControls, rb.pass)) { st = "ready_from_version"; why = "the version " + q.entry->version() + " profile: " + rb.why; }
                else { st = "queued"; why = "the version " + q.entry->version() + " profile does not fit (" + rb.why + "): measure this version"; toMap.push_back ({ q.i, q.d, row }); }
            }
        }
        row->setProperty ("state", st); row->setProperty ("why", why);
        const bool mapsNext = st == "queued" && ! store.count (uid + "|" + ver);
        if (! mapsNext) { pendingLoad.erase (ir.desc.name); prog.finished (ir.desc.name, problemState (st)); }   // a failed check is done when its map is
        else prog.loadEnded (ir.desc.name);   // 15 Oct: its load is over - off the "current" list (the name under the bar) while it waits for its map
        say ("  " + ir.desc.name.paddedRight (' ', 30) + " " + ver.paddedRight (' ', 9) + " -> quick_check / " + st + ": " + why);
    }
    }   // the read-backs' warm probe closes here
    std::set<juce::String> catNames; for (const auto& p : toCategorise) catNames.insert (plugins[p.i].desc.name);
    std::vector<size_t> mapIdx; for (const auto* l : { &toMap, &toCategorise }) for (const auto& p : *l) { const auto& d = plugins[p.i].desc; if (! store.count (juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version)) mapIdx.push_back (p.i); }
    std::map<juce::String, juce::String> early;   // product -> a state settled while mapping (needs_licence / crashed / wont_load)
    say ("WIZARD: map store " + opt.ledger.getChildFile ("maps").getFullPathName().replace (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(), "~") + ": "
         + juce::String ((int) store.size()) + " map(s); " + juce::String ((int) mapIdx.size()) + " plugin(s) to map (the database settled the rest without loading them)");
    if (! mapIdx.empty())
    {
        opt.ledger.getChildFile ("maps").createDirectory();
        std::vector<juce::PluginDescription> mapDs; for (auto i : mapIdx) mapDs.push_back (plugins[i].desc);
        auto laneOfD = [&] (const juce::PluginDescription& d) { jobs::LaneInputs in; in.uad = uad::isUadProduct (d.name, d.manufacturerName); in.pace = paceHeld (d, bundlesAll, in.paceWhy); return jobs::laneFor (in); };
        mapForWizard (mapDs, opt.jobs, [&] (const juce::StringArray& a)
                      {   // 14 Oct: a bad shell's plugin is not loaded; a WaveShell load gives up sooner
                          if (shells.isBad (shellOfId (a[2]))) { ChildResult skipped; skipped.kind = ChildResult::Kind::spawnFailed; skipped.out = "WAVESHELL_SKIPPED"; return skipped; }
                          return runChild (a, loadTimeoutFor (a[2])); }, laneOfD, prog, opt.probe,
                      [&] (const juce::PluginDescription& d, ChildResult& r)
        {
            const auto uid = juce::String::toHexString (d.uniqueId).toLowerCase();
            const auto shell = shellOfId (d.fileOrIdentifier);
            if (r.out == "WAVESHELL_SKIPPED") { early[d.name] = "waves_skipped"; earlyWhy[d.name] = wizroute::wavesSkippedWhy (shell, shells.why (shell)); say ("  " + d.name + ": " + earlyWhy[d.name]); prog.finished (d.name, true); return; }
            if (shells.report (shell, r.kind == ChildResult::Kind::timedOut, r.kind == ChildResult::Kind::signaled))
                say ("WIZARD: " + shell + " " + shells.why (shell) + " - its other Waves plugins are skipped this scan (check Waves Central)");
            if (shell.isNotEmpty() && (r.kind == ChildResult::Kind::timedOut || shells.isBad (shell)) && ! r.cleanExit())
            { early[d.name] = "waves_skipped"; earlyWhy[d.name] = wizroute::wavesSkippedWhy (shell, shells.why (shell).isNotEmpty() ? shells.why (shell) : juce::String ("did not load")); say ("  " + d.name + ": " + earlyWhy[d.name]); prog.finished (d.name, true); return; }
            if (r.kind == ChildResult::Kind::uiShown) { wizroute::rememberLicenceWindow (licenceMemF, wizroute::pluginId (uid, d.version, fmtTag (d))); early[d.name] = "needs_licence"; say ("  " + d.name + ": a window appeared (licence?) - closed, not clicked; not mapped"); prog.finished (d.name, true); return; }
            const auto ps = paramsOfList (r.out);
            if (! r.cleanExit() || ps.empty()) { early[d.name] = r.cleanExit() ? "wont_load" : "crashed"; say ("  " + d.name + ": not mapped (" + (ps.empty() && r.cleanExit() ? juce::String ("no parameters") : r.describe()) + ")"); prog.finished (d.name, true); return; }
            const auto fp = echojay::fingerprintForDescription (d, (int) ps.size());
            const auto cat = wizard::wizardCategory (d.name, catalogue);
            opt.ledger.getChildFile ("maps").getChildFile (fp + ".json").replaceWithText (juce::JSON::toString (wizard::paramMap (d.name, d.manufacturerName, uid, d.version, fp, ps, cat, fmtTag (d))) + "\n");
            store[uid + "|" + d.version] = { fp, cat };
            if (! catNames.count (d.name)) prog.finished (d.name, false);   // 14 Oct: a plugin whose signature reads follow is done after them
            say ("  " + d.name.paddedRight (' ', 30) + " mapped: " + juce::String ((int) ps.size()) + " parameter(s), map " + fp.substring (0, 12));
        },
        [&] (const juce::StringArray& a) { say ("  " + a[1] + ": timed out with other work running - mapping it again alone"); return runChild (a, 2 * loadTimeoutFor (a[2])); },   // 15 Oct: alone, twice the bound
        [&] { return jobs::jobsNowForUser (juce::jmax (1, opt.jobs)); });   // 15 Oct: 2 at a time while the user is active
    }
    if (wizardStop() != 0) return stoppedInScan ("mapping");
    auto fpOf = [&] (const juce::PluginDescription& d) { const auto k = juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version; return store.count (k) ? store.at (k).fp : juce::String(); };
    for (const auto& p : toMap)
    {
        const auto& d = plugins[p.i].desc; p.row->setProperty ("map_fp", fpOf (d));
        if (early.count (d.name)) { p.row->setProperty ("state", early[d.name]); p.row->setProperty ("why", earlyWhy.count (d.name) ? earlyWhy[d.name] : "it could not be mapped (" + early[d.name] + ")"); continue; }
        if (p.d.route == wizroute::Route::measure || p.row->getProperty ("state").toString() == "queued") enqueue (d.name, p.d.category, p.d.passes, p.d.limiterKnown);
    }
    // the signature reads (EJ Map's categorise step, one plugin at a time), cached by plugin_id
    const auto cacheF = out.getChildFile ("categorised.json"); auto cache = juce::JSON::parse (cacheF.loadFileAsString()); if (! cache.isObject()) cache = juce::var (new juce::DynamicObject());
    auto catOpt = opt; catOpt.includePace = true;   // the user's own Mac: PACE units load (a licence window is still filed, quietly)
    const auto catRaw = out.getChildFile ("categorise_raw"); catRaw.createDirectory();
    { probeserve::ScopedServe warmSignatures (opt.probeServe);   // 15 Oct: the warm probe for the signature reads too (this thread only)
    for (const auto& p : toCategorise)
    {
        if (wizardStop() != 0) return stoppedInScan ("reading signatures");
        const auto& ir = plugins[p.i]; const auto pid = p.row->getProperty ("plugin_id").toString(); p.row->setProperty ("map_fp", fpOf (ir.desc));
        if (early.count (ir.desc.name)) { p.row->setProperty ("state", early[ir.desc.name]); p.row->setProperty ("why", earlyWhy.count (ir.desc.name) ? earlyWhy[ir.desc.name] : "it could not be mapped (" + early[ir.desc.name] + ")"); continue; }
        juce::var sig;
        if (cache.hasProperty (pid)) { sig = cache[juce::Identifier (pid)]; prog.finished (ir.desc.name, false); say ("  " + ir.desc.name + ": signature reads from categorised.json (not loaded again)"); }
        else
        {
            if (const auto cshell = shellOfId (ir.desc.fileOrIdentifier); shells.isBad (cshell))
            {   p.row->setProperty ("state", "waves_skipped"); p.row->setProperty ("why", wizroute::wavesSkippedWhy (cshell, shells.why (cshell))); prog.finished (ir.desc.name, true); continue; }
            prog.started (ir.desc.name);
            auto cOpt = catOpt; cOpt.timeoutMs = loadTimeoutFor (ir.desc.fileOrIdentifier);
            const auto r = categoriseOne (cOpt, ir, {}, bundlesAll, catRaw);
            if (wizardStop() != 0) return stoppedInScan ("reading signatures");   // a read cut short by the stop is never cached
            sig = categorise::rowVar (r); sig.getDynamicObject()->setProperty ("reads_ran", r.measured);
            cache.getDynamicObject()->setProperty (pid, sig); cacheF.replaceWithText (juce::JSON::toString (cache));
            prog.finished (ir.desc.name, false);
        }
        if (sig.getProperty ("why", "").toString().startsWith ("a window")) wizroute::rememberLicenceWindow (licenceMemF, pid);   // 14 Oct
        const auto d = wizroute::afterSignature ((bool) sig.getProperty ("reads_ran", false), (bool) sig.getProperty ("settled", false), sig.getProperty ("category", "").toString(),
                                                 sig.getProperty ("out_of_scope", "").toString(), sig.getProperty ("why", "").toString());
        p.row->setProperty ("route", wizroute::routeName (d.route)); p.row->setProperty ("category", d.category); p.row->setProperty ("why", d.why); p.row->setProperty ("signature", sig);
        const juce::String st = d.route == wizroute::Route::measure ? "queued" : d.route == wizroute::Route::sendOther ? "other_category" : d.route == wizroute::Route::outOfScope ? "out_of_scope"
                              : d.route == wizroute::Route::held ? "held" : "needs_category";
        p.row->setProperty ("state", st);
        if (d.route == wizroute::Route::measure) enqueue (ir.desc.name, d.category);
        say ("  " + ir.desc.name.paddedRight (' ', 30) + " -> " + wizroute::routeName (d.route) + (d.category.isNotEmpty() ? " (" + d.category + ")" : juce::String()) + ": " + d.why);
    }
    }   // the signature reads' warm probe closes here
    // THE DEMO CHECK (14 Oct): no longer here - it is the first thing in each plugin's measuring (below), so the scan finishes fast
    counts.clear(); for (const auto& r : rows) ++counts[r.getProperty ("state", "").toString()];
    for (const auto& grp : wizroute::measureOrder()) for (const auto& pb : grp) queueFile (pb).replaceWithText ((queues.count (pb) ? queues[pb].joinIntoString ("\n") : juce::String()) + "\n");
    attributeQueues (rows, queues);
    say ("WIZARD: " + juce::String ((int) plugins.size()) + " plugin(s) found: " + wizard::summaryLine (counts, wo.bundle));
    {   // what the scan screen says: "N plugins we don't have yet - about X" (the measured per-category seconds once any exist, --jobs aware)
        const auto q = measureItems(); juce::StringArray distinct; for (const auto& it : q) distinct.addIfNotAlreadyThere (it.product);
        wizprog::Times tq; for (const auto& grp : wizroute::measureOrder()) for (const auto& pb : grp) for (const auto& f : out.getChildFile ("phaseb").getChildFile (pb).findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
        { const auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("outcome", "").toString() == "ok") tq.add (wizroute::kindOf (pb), (double) r.getProperty ("seconds", 0.0)); }
        state->setProperty ("to_measure", distinct.size()); state->setProperty ("to_measure_eta_s", std::round (wizprog::stageSecondsLeft (q, tq, opt.jobs)));
    }
    juce::String bundleWords; bool sentEarly = false;   // sentEarly: Send what's done (the finish request)
    // THE STATES (13 Oct: a lambda, so a checkpoint can settle a COPY of the rows mid-run): each queued row from what landed on disk
    auto settle = [&] (juce::Array<juce::var>& rows, std::map<juce::String, int>& counts, bool finishing)
    {
        counts.clear();
        for (auto& rv : rows)
        {
            auto* row = rv.getDynamicObject(); if (row->getProperty ("state").toString() != "queued") { ++counts[row->getProperty ("state").toString()]; continue; }
            const auto pbs = juce::StringArray::fromTokens (row->getProperty ("phaseb").toString(), ",", "");
            if (pbs.size() == 1 && pbs[0] == "limitercomp")
            {   // 13 Oct (limiters): the database holds the limiter pass; this Mac ran its compressor pass alone - sent like any compressor
                const auto r = landedRow ("limitercomp", row->getProperty ("product").toString()); juce::String st = "measuring", why; juce::StringArray draftsL;
                if (r.isObject())
                {
                    const auto stem = r.getProperty ("_stem", "").toString(); juce::String cs;
                    const auto ocV = juce::JSON::parse (out.getChildFile ("phaseb/limitercomp/outcome").getChildFile (stem + ".outcomes.json").loadFileAsString());
                    if (const auto* a = ocV.getArray()) for (const auto& o : *a) { cs = o.getProperty ("state", "").toString(); why = o.getProperty ("reason", "").toString(); }
                    bool pass = false;
                    for (const auto& f : out.getChildFile ("phaseb/limitercomp/profiles").findChildFiles (juce::File::findFiles, false, "*.json"))
                    {
                        if (f.getFileName().endsWith (".tonecheck.json")) continue;
                        if (juce::JSON::parse (f.loadFileAsString()).getProperty ("plugin", {}).getProperty ("plugin_id", "").toString() != row->getProperty ("plugin_id").toString()) continue;
                        const auto tc = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tonecheck.json");
                        pass = (bool) juce::JSON::parse (tc.loadFileAsString()).getProperty ("pass_within_0_5_db", false);
                        if (wizroute::sendCompressorPass (cs, pass)) { draftsL.add ("limitercomp/profiles/" + f.getFileName()); draftsL.add ("limitercomp/profiles/" + tc.getFileName()); }
                    }
                    st = wizroute::sendCompressorPass (cs, pass) ? juce::String ("measured") : wizard::plainOutcome (r.getProperty ("outcome", "").toString(), cs);
                    row->setProperty ("compressor_pass", cs + (pass ? ", tone check passed" : ", tone check did not pass"));
                }
                if (st == "measuring" && finishing) { st = "not_reached"; why = wizard::kNotReachedWhy; }
                if (why.isEmpty() && ! wizard::keepsRoutingWhy (st)) why = wizard::reasonFor (r.getProperty ("outcome", "").toString());   // 15 Oct: never the routing reason
                row->setProperty ("state", st); row->setProperty ("drafts", draftsL.joinIntoString (",")); if (why.isNotEmpty()) row->setProperty ("why", why); ++counts[st];
                continue;
            }
            if (! pbs.isEmpty() && pbs[0] != "wizard" && pbs[0] != "wizard_eq")
            {   // ALL CATEGORIES: measured when the category's own row is ok and its draft was derived; a limiter's compressor pass is said beside
                const auto r = landedRow (pbs[0], row->getProperty ("product").toString());
                juce::String st, why; juce::StringArray draftsL;
                if (! r.isObject()) { st = finishing ? "not_reached" : "measuring"; if (finishing) why = wizard::kNotReachedWhy; }
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
                if (why.isEmpty() && ! wizard::keepsRoutingWhy (st)) why = wizard::reasonFor (r.getProperty ("outcome", "").toString());   // 15 Oct: never the routing reason
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
            juce::String st = outcome.isEmpty() ? juce::String (finishing ? "not_reached" : "measuring") : wizard::plainOutcome (outcome, isEq ? juce::String ("exported") : certState);
            if (st == "not_reached") rowWhy = wizard::kNotReachedWhy;
            if (isEq && st == "measured")
            {   // an EQ is ready to send when its acceptance passed
                juce::String why; const auto draft = juce::JSON::parse (catDir.getChildFile ("drafts").getChildFile (stem + ".eq_profile.draft.json").loadFileAsString());
                if (! wizard::eqAccepted (draft, why)) { st = "not_measurable"; rowWhy = "measured, but " + why; } else { rowWhy = why; row->setProperty ("eq_draft", "drafts/" + stem + ".eq_profile.draft.json"); }
            }
            if (rowWhy.isEmpty() && ! wizard::keepsRoutingWhy (st)) rowWhy = wizard::reasonFor (outcome);   // 15 Oct: a needs_licence UAD row kept "the server's category: compressor"
            row->setProperty ("state", st); row->setProperty ("outcome", outcome); row->setProperty ("cert_state", certState); if (rowWhy.isNotEmpty()) row->setProperty ("why", rowWhy); ++counts[st];
        }
    };
    // THE BUNDLE (13 Oct: a lambda - the end, Send what's done, and the quiet checkpoints all pack the same way); 3 = refused (personal / category)
    int checkpointSeq = 0;
    auto pack = [&] (const juce::Array<juce::var>& rows, const juce::String& runKind, bool quiet) -> int
    {
        if (! quiet) { wizprog::Item it; it.product = "the contribution bundle"; it.kind = "pack"; prog.stage (wizprog::Stage::packing, { it }); prog.started (it.product); }
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
                    if (! (bool) tcv.getProperty ("pass_within_0_5_db", false)) { auto* s = new juce::DynamicObject(); s->setProperty ("product", product); s->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); s->setProperty ("why", "measured, but its tone check did not pass: not sent"); skipped.add (juce::var (s)); continue; }
                    f.copyFileTo (B.getChildFile ("profiles").getChildFile (f.getFileName())); tc.copyFileTo (B.getChildFile ("profiles").getChildFile (tc.getFileName()));
                    auto* it = new juce::DynamicObject(); it->setProperty ("product", product); it->setProperty ("kind", "compressor"); it->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); it->setProperty ("map_fp", p.getProperty ("plugin", {}).getProperty ("map_fp", {}));
                    it->setProperty ("profile", "profiles/" + f.getFileName()); it->setProperty ("tone_check", "profiles/" + tc.getFileName()); it->setProperty ("tone_check_g2_pass", true); items.add (juce::var (it));
                }
            else if (st == "ready_from_version")
            { auto* c = new juce::DynamicObject(); c->setProperty ("product", product); c->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); c->setProperty ("profile_from_version", rv.getProperty ("from_version", {})); c->setProperty ("readback", rv.getProperty ("readback", {}));
              if ((bool) rv.getProperty ("matched_with_tolerance", false)) { c->setProperty ("matched_with_tolerance", true); c->setProperty ("near_matches", rv.getProperty ("near_matches", {})); }   // 14 Oct: for the audit
              confirmations.add (juce::var (c)); }
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
            { auto* s = new juce::DynamicObject(); s->setProperty ("product", product); s->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); s->setProperty ("state", st); s->setProperty ("why", rv.getProperty ("why", {})); skipped.add (juce::var (s)); }
        }
        // 17 Oct (rulings 3): every entry the upload carries says its format (from its plugin_id: "AudioUnit|..." / "VST3|...")
        for (auto* list : { &items, &confirmations, &skipped, &crashedList, &needsCategory, &otherCategories, &outOfScopeList, &heldList, &referenceVotes })
            for (auto& e : *list) if (auto* o = e.getDynamicObject(); o != nullptr && ! o->hasProperty ("format")) o->setProperty ("format", wizard::formatOfPluginId (o->getProperty ("plugin_id").toString()));
        auto* M = new juce::DynamicObject(); M->setProperty ("schema", "ej_contribution/0"); M->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
        M->setProperty ("tool", juce::String ("EJ Map ") + EJMAP_GIT_HASH); M->setProperty ("platform", juce::SystemStats::getOperatingSystemName());
        M->setProperty ("profiles", items); M->setProperty ("confirmations", confirmations); M->setProperty ("skipped", skipped); M->setProperty ("crashed", withProbeCrashes (crashedList, skipped));
        M->setProperty ("needs_category", needsCategory); M->setProperty ("other_categories", otherCategories); M->setProperty ("out_of_scope", outOfScopeList); M->setProperty ("held", heldList);
        {   // 17 Oct (rulings 3): the VST3 twins' bridge reads travel as read-back records, each tagged by format (the server judges the bridge)
            juce::Array<juce::var> br; const auto src = out.getChildFile ("phaseb").getChildFile ("readback").getChildFile ("bridge");
            for (const auto& f : src.findChildFiles (juce::File::findFiles, false, "*.readback.json"))
            {
                const auto rec = juce::JSON::parse (f.loadFileAsString());
                B.getChildFile ("readback").getChildFile ("bridge").createDirectory(); f.copyFileTo (B.getChildFile ("readback").getChildFile ("bridge").getChildFile (f.getFileName()));
                auto* o = new juce::DynamicObject(); o->setProperty ("product", rec.getProperty ("product", {})); o->setProperty ("format", rec.getProperty ("format", {}));
                o->setProperty ("plugin_id", rec.getProperty ("plugin_id", {})); o->setProperty ("twin_plugin_id", rec.getProperty ("twin_plugin_id", {})); o->setProperty ("file", "readback/bridge/" + f.getFileName());
                br.add (juce::var (o));
            }
            M->setProperty ("bridge_reads", br);
        }
        {   // THE RUN (13 Oct, partial uploads): the run id, what this upload is (checkpoint / sent_early / finished), the run's queue and every
            // queued plugin's state now - the server merges by run id + plugin and decides the reward from the merged run, never from this
            auto* R = new juce::DynamicObject(); R->setProperty ("run_id", wo.runId); R->setProperty ("kind", runKind); M->setProperty ("run", juce::var (R));
            juce::Array<juce::var> queue, outcomes;
            for (const auto& rv : rows)
            {
                if (rv.getProperty ("phaseb", "").toString().isEmpty()) continue;
                queue.add (rv.getProperty ("plugin_id", {}));
                auto* o = new juce::DynamicObject(); o->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); o->setProperty ("product", rv.getProperty ("product", {}));
                o->setProperty ("state", rv.getProperty ("state", {})); outcomes.add (juce::var (o));
            }
            M->setProperty ("queue", queue); M->setProperty ("outcomes", outcomes);
        }
        M->setProperty ("reference_votes", referenceVotes);
        const juce::var Mv (M);   // ONE owner of the manifest (a second juce::var (M) after a temporary would use a deleted object)
        {   // THE BOUNDARY CHECK (12 Oct): every category the manifest carries is one of the server's 12, or nothing is packed
            const auto bad = wizserver::invalidOutgoing (Mv);
            if (! bad.isEmpty()) { say ("WIZARD: the bundle is NOT made - categories the server does not have: " + bad.joinIntoString ("; ")); if (! quiet) prog.finish ("Stopped: the bundle would carry a category the server does not have (" + bad.joinIntoString ("; ") + "). Nothing was made or sent."); return 3; }
        }
        M->setProperty ("note", "results only: profiles, their tone checks and this manifest - no plugin files, no audio, no account token");
        B.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (Mv) + "\n");
        char host[256] = {}; gethostname (host, sizeof host - 1);
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(); const auto user = juce::SystemStats::getLogonName();
        juce::StringArray problems;
        for (const auto& f : B.findChildFiles (juce::File::findFiles, true))
            for (const auto& p : wizard::personalProblems (f.loadFileAsString(), home, user, juce::String (host).upToFirstOccurrenceOf (".", false, false)))
                problems.add (f.getRelativePathFrom (B) + ": " + p);
        if (! problems.isEmpty()) { say ("WIZARD: the bundle is NOT made - it would carry: " + problems.joinIntoString ("; ")); if (! quiet) prog.finish ("Stopped: the bundle would carry something personal (" + problems.joinIntoString ("; ") + "). Nothing was made or sent."); return 3; }
        // 13 Oct (partial uploads): the zip and its manifest land by rename, then checkpoint.json names them - the app may read them while
        // the engine still measures
        const auto zip = out.getChildFile ("contribution.zip"); const auto zipTmp = out.getChildFile ("contribution.zip.tmp"); zipTmp.deleteFile();
        juce::ChildProcess z; z.start (juce::StringArray { "/bin/sh", "-c", "cd \"" + B.getFullPathName() + "\" && /usr/bin/zip -qr \"" + zipTmp.getFullPathName() + "\" ." }); z.waitForProcessToFinish (60000);
        zipTmp.moveFileTo (zip);
        { const auto mt = out.getChildFile ("contribution.manifest.json.tmp"); mt.replaceWithText (juce::JSON::toString (Mv) + "\n"); mt.moveFileTo (out.getChildFile ("contribution.manifest.json")); }
        {   auto* c = new juce::DynamicObject(); c->setProperty ("seq", ++checkpointSeq); c->setProperty ("kind", runKind); c->setProperty ("run_id", wo.runId);
            c->setProperty ("zip", "contribution.zip"); c->setProperty ("manifest", "contribution.manifest.json"); c->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));
            const auto ct = out.getChildFile ("checkpoint.json.tmp"); ct.replaceWithText (juce::JSON::toString (juce::var (c))); ct.moveFileTo (out.getChildFile ("checkpoint.json")); }
        say ("WIZARD: contribution bundle -> " + zip.getFileName() + " (" + juce::String (items.size()) + " new profile(s), " + juce::String (confirmations.size()) + " confirmation(s) of another version's profile, "
             + juce::String (skipped.size()) + " skipped with the reason; " + juce::String (zip.getSize() / 1024.0, 1) + " KB; checked: no paths, names or tokens)");
        say ("WIZARD: UPLOAD (stubbed): nothing is sent. A real app shows this list and sends only when the user presses Send.");
        if (! quiet) prog.finished ("the contribution bundle", false);
        bundleWords = "Ready to send: " + zip.getFileName() + " (" + juce::String (items.size()) + " new, " + juce::String (confirmations.size()) + " confirmation(s), " + juce::String (zip.getSize() / 1024.0, 1) + " KB) - nothing was sent.";
        return 0;
    };
    // A CHECKPOINT (13 Oct): the drafts for what landed, the states settled on a COPY of the rows (the run's own rows stay queued), packed quietly
    auto checkpoint = [&] (const juce::String& why)
    {
        if (! wo.bundle) return;
        juce::StringArray cats; for (const auto& [pb, l] : queues) if (! l.isEmpty() && pb != "wizard" && pb != "wizard_eq") cats.add (pb);
        auto optD = opt; optD.wizardQueues = true; if (! cats.isEmpty()) runPhaseBDrafts (optD, cats);
        auto copyV = juce::JSON::parse (juce::JSON::toString (juce::var (rows))); juce::Array<juce::var> copy; if (const auto* a = copyV.getArray()) copy = *a;
        std::map<juce::String, int> c2; settle (copy, c2, false);
        const int rc = pack (copy, "checkpoint", true);
        say ("WIZARD: checkpoint (" + why + "): " + (rc == 0 ? "packed - " + wizard::summaryLine (c2, false) : juce::String ("NOT packed (refused)")));
    };
    // 2. MEASURE THE QUEUES (Phase B: --jobs, resumable, every outcome; the tone check / the EQ acceptance)
    if (wo.measure && queuedAnywhere())
    {
        say ("WIZARD: measuring " + juce::String ((int) measureItems().size()) + " plugin measurement(s)" + (opt.jobs > 1 ? ", " + juce::String (opt.jobs) + " at a time" : juce::String()) + " - stop any time; it carries on next time");
        double lastCheckpointMs = juce::Time::getMillisecondCounterHiRes();   // 13 Oct: the quiet checkpoints count from the start of measuring
        {   // MEASURING: the two queues; a plugin whose Phase B row is already on disk is done (resume) and its seconds are a measured time
            auto its = measureItems();   // RESUME: an item whose Phase B row already landed is done; its seconds are a measured time
            {   const auto dc = juce::JSON::parse (out.getChildFile ("demo_checks.json").loadFileAsString());   // 14 Oct: a demo check already in the cache is done
                for (auto& it : its) if (it.kind == "demo") for (const auto& ir : plugins) if (ir.desc.name == it.product)
                { const auto c = dc.getProperty (juce::Identifier (wizroute::pluginId (juce::String::toHexString (ir.desc.uniqueId), ir.desc.version)), {});
                  if (c.isObject()) { it.done = true; if ((double) c.getProperty ("seconds", 0.0) > 0.0) prog.seedTime ("demo", (double) c.getProperty ("seconds", 0.0)); } break; } }
            for (auto& it : its) for (const auto& grp : wizroute::measureOrder()) for (const auto& pb : grp) if (wizroute::kindOf (pb) == it.kind)
                if (const auto r = landedRow (pb, it.product); r.isObject()) { it.done = true; it.problem = r.getProperty ("outcome", "").toString() != "ok";
                    if (it.problem) prog.addProblems (1); else prog.seedTime (it.kind, (double) r.getProperty ("seconds", 0.0)); }
            prog.stage (wizprog::Stage::measuring, its);
            phaseb::rowHooks().started = [&] (const juce::String& cat, const juce::String& product) { prog.started (product, wizroute::kindOf (cat)); };
            phaseb::rowHooks().finished = [&] (const juce::String& cat, const juce::String& product, const juce::String& outcome, double seconds)
            {   // 13 Oct: a row that landed ok but gave no result (a refused compressor, a failed EQ acceptance) is not "measured so far"
                juce::String certState; bool eqOk = false;
                if (outcome == "ok" && (cat == "wizard" || cat == "limitercomp" || cat == "wizard_eq"))
                    if (const auto r = landedRow (cat, product); r.isObject())
                    {
                        const auto stem = r.getProperty ("_stem", "").toString(); const auto dir = out.getChildFile ("phaseb").getChildFile (cat);
                        if (cat == "wizard_eq") { juce::String w; eqOk = wizard::eqAccepted (juce::JSON::parse (dir.getChildFile ("drafts").getChildFile (stem + ".eq_profile.draft.json").loadFileAsString()), w); }
                        else { const auto oc = juce::JSON::parse (dir.getChildFile ("outcome").getChildFile (stem + ".outcomes.json").loadFileAsString());
                               if (const auto* a = oc.getArray()) for (const auto& o : *a) certState = o.getProperty ("state", "").toString(); }
                    }
                prog.finished (product, ! wizroute::landedMeasured (cat, outcome, certState, eqOk), seconds, wizroute::kindOf (cat));
                const double now = juce::Time::getMillisecondCounterHiRes();   // 13 Oct: the quiet checkpoint, every ~60 min while measuring
                if (wizapp::checkpointDue (lastCheckpointMs, now, wo.checkpointMinutes)) { lastCheckpointMs = now; checkpoint ("every " + juce::String (wo.checkpointMinutes) + " min"); }
            };
            prog.startTicker();
        }
        // FINISH NOW (Kathy, 13 Oct): the app writes <out>/finish_requested, then stops the engine like a pause; a request left from an
        // earlier run never counts
        out.getChildFile (wizapp::kFinishRequestFile).deleteFile();
        // THE MEASURING ORDER: one Phase B pass per group, most valuable first; a stop (130) ends the order there
        auto optW = opt; optW.wizardQueues = true; optW.limiterPassKnown = limiterKnown; int pbCode = 0;
        // 13 Oct (beta item 2): the measuring stops cleanly when the disk runs low - checked at most every 10 s, a stop like Ctrl-C
        bool diskStopped = false; double lastDiskCheck = -1.0e9;
        phaseb::extraStop() = [&] {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (! diskStopped && now - lastDiskCheck >= 10000.0) { lastDiskCheck = now; if (! wizapp::diskVerdict (wizapp::freeBytes (out), false).ok) { diskStopped = true; say ("WIZARD: the disk is running low - stopping cleanly"); } }
            return diskStopped || wizardStop() != 0; };   // 13 Oct: a stop that lands before Phase B installs its own handler
        for (const auto& grp : wizroute::measureOrder())
        {
            // 13 Oct (the finish rehearsal): a stop between two passes was seen only after the NEXT whole pass ran (the gate ran 4.5 min
            // after Finish now) - it is checked before each pass starts
            if (wizardStop() != 0) { pbCode = 130; break; }
            {   // THE DEMO CHECK (14 Oct, moved from the scan): the first thing in each plugin's measuring - before its first category pass (the
                // cache makes it once per plugin). A demo is filed demo and leaves every queue; a pause / Send what's done stops it like any
                // measurement (in flight: discarded, not cached, checked again next time)
                const auto names = wizroute::demoNamesForGroup (grp, queues);
                std::vector<juce::PluginDescription> qd; for (const auto& ir : plugins) if (names.contains (ir.desc.name)) qd.push_back (ir.desc);
                if (! qd.empty())
                {
                    const auto verdicts = demoCheckForWizard (qd, opt.jobs, [&] (const juce::StringArray& a) { prog.started (a[1], "demo"); return runChild (a, opt.timeoutMs); }, opt.probe, out.getChildFile ("demo_checks.json"),
                                                              [&] (const juce::String& n, const wizard::DemoVerdict& v, double secs)
                                                              {   prog.finished (n, false, secs, "demo");
                                                                  // 15 Oct (Kathy: the bar sat at 0% while the checks went by): a plugin this check skips moves the bar NOW -
                                                                  // its measurements are done (a problem) with their whole expected work; the queues follow after the pool
                                                                  if (v.demo || v.window || v.silent || v.inert) for (const auto& [pb, l] : queues) if (l.contains (n)) prog.finished (n, true, -1.0, wizroute::kindOf (pb)); });
                    if (wizardStop() != 0) { pbCode = 130; break; }
                    juce::StringArray demos; for (const auto& [name, v] : verdicts) if (v.demo || v.window || v.silent || v.inert) demos.add (name);   // 14 Oct: a licence window leaves the queues too; 15 Oct: no sound / never processes too
                    const auto gone = wizroute::dropDemos (queues, demos);
                    for (const auto& [pb, name] : gone) prog.finished (name, true, -1.0, wizroute::kindOf (pb));
                    for (const auto& [name, v] : verdicts)
                    {
                        if (! v.demo && ! v.window && ! v.silent && ! v.inert) continue;
                        for (auto& rv : rows) if (rv.getProperty ("product", "").toString() == name)
                        {   rv.getDynamicObject()->setProperty ("state", v.window || v.silent || v.inert ? "needs_licence" : "demo"); rv.getDynamicObject()->setProperty ("why", v.window ? wizroute::licenceSkipWhy (wizroute::LicenceSkip::remembered) : v.why);
                            if (v.window) wizroute::rememberLicenceWindow (licenceMemF, rv.getProperty ("plugin_id", "").toString()); }
                        say ("  " + name.paddedRight (' ', 30) + " -> skipped: " + v.why);
                    }
                    if (! gone.empty()) for (const auto& g2 : wizroute::measureOrder()) for (const auto& pb : g2) queueFile (pb).replaceWithText ((queues.count (pb) ? queues[pb].joinIntoString ("\n") : juce::String()) + "\n");
                }
            }
            juce::StringArray run; for (const auto& pb : grp) if (queues.count (pb) && ! queues[pb].isEmpty()) run.add (pb);
            if (run.isEmpty()) continue;
            say ("WIZARD: measuring " + run.joinIntoString (" + ") + " (" + juce::String ((int) [&] { int n = 0; for (const auto& pb : run) n += queues[pb].size(); return n; }()) + ")");
            pbCode = runPhaseBAll (optW, run, {}, {});
            std::signal (SIGINT, [] (int) { wizardStop() = 1; }); std::signal (SIGTERM, [] (int) { wizardStop() = 1; });   // Phase B restores the defaults on its way out
            if (wizardStop() != 0) pbCode = 130;
            if (wizprog::measureStopped (pbCode)) break;
        }
        prog.stopTicker(); phaseb::rowHooks() = {}; phaseb::extraStop() = {};
        const bool finishing = wizapp::finishAfterStop (pbCode, out.getChildFile (wizapp::kFinishRequestFile).existsAsFile(), diskStopped);
        sentEarly = finishing;
        if (finishing)
        {   // the in-flight plugins were discarded by Phase B's stop (no row lands, their temp folders thrown away); pack what landed
            out.getChildFile (wizapp::kFinishRequestFile).deleteFile(); wizardStop() = 0; pbCode = 0;
            const auto [measured, toMeasure] = prog.stageDone();
            say ("WIZARD: finish now - " + juce::String (measured) + " of " + juce::String (toMeasure) + " landed; the plugins in flight were discarded; packing what is done");
        }
        {   // the drafts for the categories beyond compressor / EQ (their cert derive step, the same rules)
            juce::StringArray cats; for (const auto& [pb, l] : queues) if (! l.isEmpty() && pb != "wizard" && pb != "wizard_eq") cats.add (pb);
            if (! cats.isEmpty() && ! wizprog::measureStopped (pbCode)) runPhaseBDrafts (optW, cats);
        }
        if (wizprog::measureStopped (pbCode))
        {   // the user stopped it: no bundle, no "Done" - the rows that landed stay, the same command carries on
            const auto [measured, toMeasure] = prog.stageDone();
            if (! diskStopped) checkpoint ("the pause");   // 13 Oct: a pause sends / saves what is done (a disk stop: no room to pack)
            say ("WIZARD: stopped while measuring (" + juce::String (measured) + " of " + juce::String (toMeasure) + " landed): checkpoint packed; the same command carries on");
            prog.finish (diskStopped ? wizapp::diskVerdict (wizapp::freeBytes (out), false).words : wizprog::stoppedWords (prog.stoppedAt(), measured, toMeasure, prog.running()), true);
            return 130;
        }
        settle (rows, counts, finishing);
        say ("WIZARD: " + wizard::summaryLine (counts, wo.bundle));
    }
    state->setProperty ("schema", "ej_wizard_state/0"); state->setProperty ("plugins", rows); state->setProperty ("summary", wizard::summaryLine (counts, wo.bundle));
    out.getChildFile ("wizard_state.json").replaceWithText (juce::JSON::toString (juce::var (state)) + "\n");
    // 15 Oct: THE RUN REACHED THE END - but is every queued plugin settled? One with no final outcome (774fe72e left 158 UAD rows unrun) is
    // LEFT: not uploaded as finished - what is done goes as a checkpoint, engine/left.json says what is left and why, kLeftExit
    if (wo.bundle && wo.measure && ! sentEarly)
    {
        const auto left = leftAtEnd (rows);
        out.getChildFile ("left.json").replaceWithText (juce::JSON::toString (juce::var (left)) + "\n");
        if (! left.isEmpty())
        {
            checkpoint ("the end, with plugins left");
            say ("WIZARD: the run reached the end with " + juce::String (left.size()) + " plugin(s) left without an outcome: not sent as finished (left.json)");
            prog.finish (wizapp::leftWords (juce::var (left)), true);
            return wizapp::kLeftExit;
        }
    }
    // 3. THE BUNDLE (profiles + tone-check sidecars / EQ profiles + a manifest; nothing personal) and the STUBBED upload
    if (wo.bundle)
    {
        if (const int rc = pack (rows, sentEarly ? juce::String ("sent_early") : juce::String ("finished"), false); rc != 0) return rc;
    }
    prog.setProblems (wizprog::problemCount (counts));   // the final states decide (an EQ can land ok and fail its acceptance)
    prog.finish ("Done in " + wizprog::durationWords ((juce::Time::getMillisecondCounterHiRes() - tStart) / 1000.0) + ": " + juce::String ((int) plugins.size()) + " plugin(s) - " + wizard::summaryLine (counts, wo.bundle) + "."
                 + (bundleWords.isNotEmpty() ? " " + bundleWords : juce::String()) + (wo.verbose ? juce::String() : " Details: wizard.log"));
    return 0;
}

} // namespace ejmap::cert
