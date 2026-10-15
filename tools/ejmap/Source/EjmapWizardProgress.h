/*
  EjmapWizardProgress.h - THE WIZARD'S PROGRESS (Kathy, 11 Oct): one number a window app can read, and one live line in the terminal.

  progress.json (in the wizard's --out folder, rewritten atomically on every event and every 2 s while a plugin is measuring):
    percent, stage (Scanning / Checking / Measuring / Packing / Done), current plugin(s), done / to do / problems, elapsed, an ETA
    from the measured per-category times (--jobs aware), and the same plain-words line the terminal shows.

  THE BAR NEVER GOES BACKWARDS:
  - each stage owns a FIXED band of the bar: Scanning 0-10, Checking 10-15, Measuring 15-97, Packing 97-100. The band does not
    change when a later stage's size becomes known, so finding out how much there is to measure cannot pull the bar back.
  - inside a band, the share done is weighted by each plugin's expected seconds (a compressor ~10x an EQ), and a plugin being measured
    earns the time it has run (up to 90% of its figure), so the bar moves with the clock, not only when a plugin lands.
  - a guard holds the bar at the highest value shown (lastPct): a share can only shrink if items are added mid-stage, and then the
    bar waits instead of jumping back.
  RESUME: plugins already done when a stage starts (a map already in the store, a Phase B row already on disk) count as done, so a
  resumed run starts the bar at the done share.
  THE ETA: per-category seconds MEASURED - rows already on disk and every plugin finished this run replace the starting figures
  (this Mac's 10 Oct rehearsal: mapping 4 s, checking 6 s, a compressor 340 s, an EQ 36 s). Remaining parallel work divides by
  --jobs; the serial lane (UAD / licence / PACE / past trouble) does not. A plugin being measured counts what it has left of its expected time (at least a tenth).

  PURE parts pinned in RoundTripTest.cpp (WZ-PROG); the Tracker draws and writes.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <ostream>
#include <vector>
#include <unistd.h>
#include "EjmapWizardApp.h"

namespace ejmap::wizprog
{
enum class Stage { scanning = 0, checking, measuring, packing, done };
struct Band { double from = 0.0, to = 0.0; };
inline Band bandOf (Stage s)
{
    switch (s)
    {
        case Stage::scanning:  return { 0.0, 10.0 };
        case Stage::checking:  return { 10.0, 15.0 };
        case Stage::measuring: return { 15.0, 97.0 };
        case Stage::packing:   return { 97.0, 100.0 };
        case Stage::done:      return { 100.0, 100.0 };
    }
    return {};
}
inline juce::String stageWord (Stage s)
{
    switch (s)
    {
        case Stage::scanning: return "Scanning"; case Stage::checking: return "Checking"; case Stage::measuring: return "Measuring";
        case Stage::packing: return "Packing"; case Stage::done: return "Done";
    }
    return {};
}

// the starting figures, seconds per plugin, until this run measures its own (this Mac, the 10 Oct rehearsals)
inline double defaultSeconds (const juce::String& kind)
{
    // the demo check (14 Oct, measured on 8 Waves v12: 4.0-7.3 s, mean 4.5 - the 60 s tone renders offline; the load is most of it)
    if (kind == "demo") return 4.5;
    if (kind == "map") return 4.0; if (kind == "check") return 6.0; if (kind == "eq") return 36.0; if (kind == "pack") return 2.0;
    // 12 Oct, all categories: the median seconds of Sean's "ok" Phase B rows per category (his 4-11 Oct nights); limitercomp is the
    // compressor path; strips: no row yet - the category's stated estimate
    if (kind == "deesser") return 18.0; if (kind == "limiter") return 28.0; if (kind == "limitercomp") return 340.0; if (kind == "saturation") return 11.0;
    if (kind == "gate") return 17.0; if (kind == "transient") return 22.0; if (kind == "multiband") return 51.0; if (kind == "tuners") return 111.0;
    if (kind == "reverb") return 41.0; if (kind == "delay") return 45.0; if (kind == "strips") return 1200.0;
    return 340.0;   // a compressor: the full path + the tone check
}
struct Times
{
    std::map<juce::String, std::vector<double>> measured;   // kind -> seconds actually taken
    void add (const juce::String& kind, double s) { if (s > 0.0) measured[kind].push_back (s); }
    double perItem (const juce::String& kind) const
    {
        const auto it = measured.find (kind);
        if (it == measured.end() || it->second.empty()) return defaultSeconds (kind);
        double t = 0.0; for (auto s : it->second) t += s; return t / (double) it->second.size();
    }
    bool isMeasured (const juce::String& kind) const { const auto it = measured.find (kind); return it != measured.end() && ! it->second.empty(); }
    // THE BAR'S WEIGHTS (Kathy, 14 Oct): a plugin with a weight (Sean's seconds for it, or its kind's median: ExpectedWork) is expected
    // to take weight x this Mac's ratio - learned from the weighted plugins finished so far (their seconds over their weights), 1 until
    // one finishes. A 20-minute reverb weighs like one, so the bar moves through a reverb pass at its real pace (the 14 Oct H-Reverb run sat
    // at "under a minute left" for 20 minutes on the reverb figure of 41 s). An item without a weight (a demo check, a map) keeps its kind's figure.
    double weightSum = 0.0, weightedActual = 0.0;
    void addWeighted (double weight, double s) { if (weight > 0.0 && s > 0.0) { weightSum += weight; weightedActual += s; } }
    double ratio() const { return weightSum > 0.0 ? weightedActual / weightSum : 1.0; }
    template <typename I> double expected (const I& i) const { return i.weight > 0.0 ? i.weight * ratio() : perItem (i.kind); }
};

struct Item { juce::String product, kind; bool done = false, running = false, problem = false, serial = false; double ranS = 0.0; int group = -1; double weight = 0.0;
              juce::String stepDir; int expectedSteps = 0, steps = 0; };   // 15 Oct (FEELS FROZEN): the row's temp folder, Sean's probe calls for it (or the kind's median), the calls done so far   // ranS: seconds a running item has run; group: its Phase B pass (-1: one pass); weight: Sean's seconds for this plugin and kind (0: the kind's figure)

// the weights' source: <database>/expected_work.json (tools/ejmap/scripts/expected_work.py, from Sean's real runs): per plugin uid and kind
// Sean's seconds, else the kind's median over his rows; 0 when the file or the kind is missing (the item keeps its kind's figure)
struct ExpectedWork
{
    juce::var data;
    static ExpectedWork load (const juce::File& db)
    {   // the database's own copy first; else the one the app carries (Contents/Resources/mock_db, assemble.sh) - a release stub has none of its own
        auto f = db.getChildFile ("expected_work.json");
        if (! f.existsAsFile()) f = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getSiblingFile ("Resources").getChildFile ("mock_db/expected_work.json");
        ExpectedWork w; w.data = juce::JSON::parse (f.loadFileAsString()); return w;
    }
    int procsFor (const juce::String& uidHex, const juce::String& kind) const   // 15 Oct: Sean's probe calls for it (the steps), else the kind's median
    {
        if (! data.isObject()) return 0;
        const auto own = data.getProperty ("plugins", {}).getProperty (juce::Identifier (uidHex.toLowerCase()), {}).getProperty (juce::Identifier (kind), {});
        if (own.isObject() && (int) own.getProperty ("procs", 0) > 0) return (int) own.getProperty ("procs", 0);
        return (int) data.getProperty ("_median", {}).getProperty (juce::Identifier (kind), {}).getProperty ("procs", 0);
    }
    double secondsFor (const juce::String& uidHex, const juce::String& kind) const
    {
        if (! data.isObject()) return 0.0;
        const auto own = data.getProperty ("plugins", {}).getProperty (juce::Identifier (uidHex.toLowerCase()), {}).getProperty (juce::Identifier (kind), {});
        if (own.isObject() && (double) own.getProperty ("seconds", 0.0) > 0.0) return (double) own.getProperty ("seconds", 0.0);
        return (double) data.getProperty ("_median", {}).getProperty (juce::Identifier (kind), {}).getProperty ("seconds", 0.0);
    }
};

// RESUME: a queue item whose row already landed (product -> its outcome) starts done; a landed row that is not "ok" is a problem
inline std::vector<Item> resumeItems (std::vector<Item> queue, const std::map<juce::String, juce::String>& landed)
{
    for (auto& i : queue) if (const auto it = landed.find (i.product); it != landed.end()) { i.done = true; i.problem = it->second != "ok"; }
    return queue;
}
// 15 Oct (FEELS FROZEN, Kathy: a plugin can take 10-20 min and nothing moves): a running plugin with an expected step count earns its
// weight x steps done / steps expected (probe calls - one trace file each), never more than 95% before it finishes; one without keeps
// the time credit (90% at most)
inline constexpr double kStepCap = 0.95;
inline double stepFraction (const Item& i) { return i.expectedSteps > 0 ? juce::jmin (kStepCap, (double) i.steps / (double) i.expectedSteps) : -1.0; }
inline double runningCredit (const Item& i, double w) { const double f = stepFraction (i); return f >= 0.0 ? f * w : juce::jmin (0.9 * w, juce::jmax (0.0, i.ranS)); }
// the steps a row has taken: its probe traces so far (".<attempt>.txt" files anywhere under the row's temp folder)
inline int countSteps (const juce::File& dir)
{
    if (! dir.isDirectory()) return 0;
    int n = 0; for (const auto& f : dir.findChildFiles (juce::File::findFiles, true, "*.txt"))
    { const auto nm = f.getFileName(); const auto pre = nm.dropLastCharacters (4); if (pre.length() > 2 && pre[pre.length() - 2] == '.' && juce::CharacterFunctions::isDigit (pre[pre.length() - 1])) ++n; }
    return n;
}
// the share of a stage done, weighted by expected seconds; an empty stage is complete. A RUNNING plugin earns its time run so far, up
// to 90% of its expected seconds (the 11 Oct rehearsal: with two 6-minute compressors running the bar sat still for 6 minutes)
inline double stageShare (const std::vector<Item>& items, const Times& t)
{
    double all = 0.0, done = 0.0;
    for (const auto& i : items) { const auto w = t.expected (i); all += w; if (i.done) done += w; else if (i.running) done += runningCredit (i, w); }
    return all <= 0.0 ? 1.0 : done / all;
}
// the bar: the stage's fixed band, the share inside it, never below what was shown (lastPct is updated)
inline double percent (Stage s, const std::vector<Item>& items, const Times& t, double& lastPct)
{
    const auto b = bandOf (s);
    const double raw = s == Stage::done ? 100.0 : b.from + (b.to - b.from) * juce::jlimit (0.0, 1.0, stageShare (items, t));
    lastPct = juce::jmax (lastPct, raw);
    return lastPct;
}
// A STOP keeps only what landed: Phase B discards the rows in flight (they run again on the resume), so the stopped bar is the done
// share alone - no running credit, no hold. It is the one place the bar goes back, and it lands exactly where the resume will open
// (the 11 Oct rehearsal: 'Stopped at 66%' then the resume opened at 41%).
inline double stoppedPercent (Stage s, std::vector<Item> items, const Times& t)
{
    for (auto& i : items) { i.running = false; i.ranS = 0.0; }
    double none = 0.0; return percent (s, items, t, none);
}
// seconds left in THIS stage: parallel work / jobs + the serial lane; a running plugin counts what it has left of its expected time
// (never under a tenth of it: a slow plugin past its figure is not 'done any second')
// THE PASSES (12 Oct, all categories): the wizard measures one category group after another, --jobs only inside a pass, so the time left
// is the sum over passes of (parallel work / jobs + the serial lane) - a pass with one plugin cannot use two jobs
inline double stageSecondsLeft (const std::vector<Item>& items, const Times& t, int jobs)
{
    std::map<int, std::pair<double, double>> byGroup;   // group -> (parallel, serial)
    for (const auto& i : items)
    {
        if (i.done) continue;
        const double e = t.expected (i); const double s = i.running ? juce::jmax (0.1 * e, e - i.ranS) : e;
        auto& g = byGroup[i.group]; (i.serial ? g.second : g.first) += s;
    }
    double left = 0.0;
    for (const auto& [grp, ps] : byGroup)
    {   // a pass's parallel work spreads over at most as many jobs as it has plugins: its time is at least its longest plugin
        double longest = 0.0; for (const auto& i : items) if (! i.done && i.group == grp && ! i.serial) longest = juce::jmax (longest, i.running ? juce::jmax (0.1 * t.expected (i), t.expected (i) - i.ranS) : t.expected (i));
        left += (grp < 0 ? ps.first / (double) juce::jmax (1, jobs) : juce::jmax (longest, ps.first / (double) juce::jmax (1, jobs))) + ps.second;
    }
    return left;
}
// seconds left overall; -1 = not known yet (the measuring queue is decided by checking)
inline double etaSeconds (Stage s, const std::vector<Item>& items, const Times& t, int jobs, bool queueKnown, const std::vector<Item>& queue)
{
    if (s == Stage::done) return 0.0;
    double left = stageSecondsLeft (items, t, jobs);
    if (s == Stage::scanning || s == Stage::checking) { if (! queueKnown) return -1.0; left += stageSecondsLeft (queue, t, jobs); }
    if (s != Stage::packing) left += defaultSeconds ("pack");
    return left;
}
// THE PROBLEM STATES (the wizard's plain words): the final count comes from the final states, not from Phase B's outcomes - an EQ that
// landed "ok" can still fail its acceptance afterwards (the 11 Oct rehearsal: Lindell 6X-500, summary "1 could not be measured", file 0)
inline bool isProblemState (const juce::String& st)
{ return st == "needs_licence" || st == "needs_hardware" || st == "crashed" || st == "wont_load" || st == "silent" || st == "not_measurable"; }
inline int problemCount (const std::map<juce::String, int>& counts) { int n = 0; for (const auto& [k, v] : counts) if (isProblemState (k)) n += v; return n; }
// AFTER THE MEASURE (11 Oct rehearsal): Phase B returns 130 when the user stopped it (Ctrl-C) - the wizard then makes NO bundle and
// says where it stopped; the same command carries on from the rows that landed
inline bool measureStopped (int phasebCode) { return phasebCode == 130; }
inline juce::String stoppedWords (double pct, int measured, int toMeasure, int wasRunning = 0)
{
    return "Stopped at " + juce::String ((int) std::floor (pct)) + "%: " + juce::String (measured) + " of " + juce::String (toMeasure) + " measured, "
           + juce::String (toMeasure - measured) + " left" + (wasRunning > 0 ? " (the " + juce::String (wasRunning) + " being measured start again)" : juce::String())
           + ". Nothing was packed or sent. Run the same command to carry on - it starts where it stopped.";
}
inline juce::String etaWords (double s)
{
    if (s < 0.0) return "working out the time";
    if (s < 60.0) return "under a minute left";
    const int m = (int) std::ceil (s / 60.0);
    if (m < 60) return "about " + juce::String (m) + " min left";
    return "about " + juce::String (m / 60) + " h " + juce::String (m % 60) + " min left";
}
inline juce::String barText (double pct, int width = 20)
{
    const int n = juce::jlimit (0, width, (int) std::round (pct / 100.0 * width));
    return "[" + juce::String::repeatedString ("#", n) + juce::String::repeatedString ("-", width - n) + "]";
}
inline juce::String durationWords (double s)
{
    const int t = (int) std::round (s);
    if (t < 60) return juce::String (t) + " s";
    if (t < 3600) return juce::String (t / 60) + " min " + juce::String (t % 60) + " s";
    return juce::String (t / 3600) + " h " + juce::String ((t % 3600) / 60) + " min";
}

// THE TRACKER: holds the run's state, writes progress.json, draws the line (a live line on a terminal, a line per change in a log)
class Tracker
{
public:
    Tracker (juce::File json, std::ostream& term, bool live, int jobs) : json_ (std::move (json)), term_ (term), live_ (live), jobs_ (juce::jmax (1, jobs)) { t0_ = juce::Time::getMillisecondCounterHiRes(); }
    ~Tracker() { stopTicker(); }

    void setPlugins (int n) { std::lock_guard<std::mutex> g (m_); plugins_ = n; }
    // THE CRASH MARKER (the wizard app, 11 Oct): a file per plugin while it is loaded; a stop or the end removes them all
    void setMarkerDir (const juce::File& d) { std::lock_guard<std::mutex> g (m_); markers_ = d; }
    void stage (Stage s, std::vector<Item> items)
    {
        std::lock_guard<std::mutex> g (m_);
        stage_ = s; items_ = std::move (items); current_.clear();
        if (s == Stage::measuring) queueKnown_ = true;
        emitLocked (true);
    }
    void setQueue (std::vector<Item> q) { std::lock_guard<std::mutex> g (m_); queue_ = std::move (q); queueKnown_ = true; }
    void seedTime (const juce::String& kind, double s) { std::lock_guard<std::mutex> g (m_); times_.add (kind, s); }
    void started (const juce::String& product, const juce::String& kind = {})   // kind: which of a product's items (a limiter has two)
    {
        std::lock_guard<std::mutex> g (m_);
        heartbeatMs_ = juce::Time::currentTimeMillis();
        for (auto& i : items_) if (i.product == product && ! i.done && ! i.running && (kind.isEmpty() || i.kind == kind)) { i.running = true; startedAt_[product + "|" + i.kind] = juce::Time::getMillisecondCounterHiRes(); break; }
        current_.removeString (product); current_.add (product);   // 15 Oct: the newest start is last (the name under the bar)
        if (markers_ != juce::File()) wizapp::markerBegin (markers_, product, stageWord (stage_));
        if (markers_ != juce::File() && stage_ != Stage::packing) markers_.getSiblingFile ("loads.log").appendText (juce::Time::getCurrentTime().toISO8601 (true) + "\t" + stageWord (stage_) + "\t" + product + "\n");   // every load, for the record
        emitLocked (false);
    }
    void finished (const juce::String& product, bool problem, double seconds = -1.0, const juce::String& kind = {})
    {
        std::lock_guard<std::mutex> g (m_);
        heartbeatMs_ = juce::Time::currentTimeMillis();
        for (auto& i : items_) if (i.product == product && ! i.done && (kind.isEmpty() || i.kind == kind))
        {
            i.done = true; i.running = false; i.problem = problem;
            const auto key = product + "|" + i.kind;
            double s = seconds; if (s < 0.0 && startedAt_.count (key)) s = (juce::Time::getMillisecondCounterHiRes() - startedAt_[key]) / 1000.0;
            if (! problem && s > 0.0) { times_.add (i.kind, s); times_.addWeighted (i.weight, s); }
            if (problem) ++problems_;
            break;
        }
        current_.removeString (product);
        if (markers_ != juce::File()) wizapp::markerEnd (markers_, product);
        emitLocked (true);
    }
    // THE NAME UNDER THE BAR (Kathy, 15 Oct: it sat on "CLA-76 (m)" for two hours while the count climbed): a plugin whose LOAD ended but
    // whose item is not done yet (a failed read-back check waits for its map) leaves the "current" list now - it is no longer loaded.
    // The list stays in start order and the name shown is the most recently started (latestLocked)
    void loadEnded (const juce::String& product)
    {
        std::lock_guard<std::mutex> g (m_);
        for (auto& i : items_) if (i.product == product && i.running && ! i.done) i.running = false;
        current_.removeString (product);
        emitLocked (true);
    }
    juce::String latest() { std::lock_guard<std::mutex> g (m_); return latestLocked(); }
    // 13 Oct (the scan bar): an item with nothing left to do when its stage begins (a resumed scan: the map is already in the store) counts
    // as done from the first frame - no time recorded, no problem - so the stage's "n of N" starts at its true share
    void alreadyDone (const juce::String& product)
    {
        std::lock_guard<std::mutex> g (m_);
        for (auto& i : items_) if (i.product == product && ! i.done) { i.done = true; i.running = false; break; }
        emitLocked (true);
    }
    void ranFor (const juce::String& product, double s)   // the clock as if the plugin started s seconds ago (pins; a window app's restore)
    { std::lock_guard<std::mutex> g (m_); for (const auto& i : items_) if (i.product == product) startedAt_[product + "|" + i.kind] = juce::Time::getMillisecondCounterHiRes() - s * 1000.0; emitLocked (false); }
    void addProblems (int n) { std::lock_guard<std::mutex> g (m_); problems_ += n; }
    void setProblems (int n) { std::lock_guard<std::mutex> g (m_); problems_ = n; }
    void finish (const juce::String& summary, bool stopped = false)   // stopped: the bar and the file keep where it stopped ("Stopped", not 100%)
    {
        stopTicker();
        std::lock_guard<std::mutex> g (m_);
        if (stopped) { stopped_ = true; lastPct_ = stoppedPercent (stage_, items_, times_); for (auto& i : items_) { i.running = false; i.ranS = 0.0; } }
        else { stage_ = Stage::done; items_.clear(); }
        current_.clear(); summary_ = summary;
        if (markers_ != juce::File()) wizapp::markersClear (markers_);   // a stop or the end: nothing is loaded any more
        emitLocked (true);
        term_ << (live_ ? "\n" : "") << summary << std::endl;
    }
    void startTicker()   // while a plugin is being measured, the clock moves: redraw every second, rewrite the file every 2 s
    {
        if (ticker_.joinable()) return;
        run_ = true;
        ticker_ = std::thread ([this] { int k = 0; while (run_) { std::this_thread::sleep_for (std::chrono::milliseconds (1000)); if (! run_) break; std::lock_guard<std::mutex> g (m_); emitLocked (false, (++k % 2) == 0); } });
    }
    void stopTicker() { run_ = false; if (ticker_.joinable()) ticker_.join(); }
    double lastPercent() const { return lastPct_; }
    double weightRatio() { std::lock_guard<std::mutex> g (m_); return times_.ratio(); }   // this Mac's seconds per weighted second so far (WZ-BAR-1)
    std::pair<int, int> stageDone() { std::lock_guard<std::mutex> g (m_); int d = 0; for (const auto& i : items_) if (i.done) ++d; return { d, (int) items_.size() }; }
    int running() { std::lock_guard<std::mutex> g (m_); int r = 0; for (const auto& i : items_) if (i.running && ! i.done) ++r; return r; }
    double stoppedAt() { std::lock_guard<std::mutex> g (m_); return stoppedPercent (stage_, items_, times_); }
    juce::String lastLine() const { return lastLine_; }

private:
    juce::String latestLocked() const { return current_.isEmpty() ? juce::String() : current_[current_.size() - 1]; }   // the most recently started
    juce::String lineLocked (double pct, double eta) const
    {
        int done = 0; for (const auto& i : items_) if (i.done) ++done;
        juce::String what = stageWord (stage_);
        if (stage_ != Stage::done && ! items_.empty()) what << " " << juce::String (juce::jmin ((int) items_.size(), done + 1)) << " of " << juce::String ((int) items_.size());
        if (current_.size() > 0) what << " - " << latestLocked() << (jobs_ > 1 && (stage_ == Stage::scanning || stage_ == Stage::measuring) ? " (" + juce::String (jobs_) + " at a time)" : juce::String());
        if (stopped_) return barText (pct) + " " + juce::String ((int) std::floor (pct)).paddedLeft (' ', 3) + "%  Stopped";
        return barText (pct) + " " + juce::String ((int) std::floor (pct)).paddedLeft (' ', 3) + "%  " + what + (stage_ == Stage::done ? juce::String() : " - " + etaWords (eta));
    }
    void emitLocked (bool event, bool writeFile = true)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        for (auto& i : items_) if (i.running && startedAt_.count (i.product + "|" + i.kind)) i.ranS = (nowMs - startedAt_[i.product + "|" + i.kind]) / 1000.0;
        if (nowMs - lastStepScan_ >= 1000.0)
        {   // 15 Oct: each running row's steps so far - a new step is a heartbeat
            lastStepScan_ = nowMs;
            for (auto& i : items_) if (i.running && ! i.done && i.stepDir.isNotEmpty())
            { const int n = countSteps (juce::File (i.stepDir)); if (n > i.steps) { i.steps = n; heartbeatMs_ = juce::Time::currentTimeMillis(); } }
        }
        const double pct = percent (stage_, items_, times_, lastPct_);
        const double eta = etaSeconds (stage_, items_, times_, jobs_, queueKnown_, queue_);
        const auto line = lineLocked (pct, eta);
        const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0_) / 1000.0;
        if (writeFile || event) writeJson (pct, eta, elapsed, line);
        if (live_) { term_ << "\r" << line << "\033[K" << std::flush; }
        else
        {   // a log or a pipe: a line when something changes (not the clock), or every 30 s
            const auto key = line.upToLastOccurrenceOf (" - ", false, false);
            if (key != lastKey_ || elapsed - lastPrinted_ >= 30.0) { term_ << line << std::endl; lastKey_ = key; lastPrinted_ = elapsed; }
        }
        lastLine_ = line;
    }
    void writeJson (double pct, double eta, double elapsed, const juce::String& line)
    {
        int done = 0, todo = 0; for (const auto& i : items_) (i.done ? done : todo)++;
        auto* o = new juce::DynamicObject();
        o->setProperty ("schema", "ej_wizard_progress/0");
        o->setProperty ("percent", std::round (pct * 10.0) / 10.0);
        o->setProperty ("stage", stopped_ ? juce::String ("Stopped") : stageWord (stage_)); o->setProperty ("stage_index", (int) stage_); o->setProperty ("stopped", stopped_);
        juce::Array<juce::var> cur; for (const auto& c : current_) cur.add (c); o->setProperty ("current", cur); o->setProperty ("current_latest", latestLocked());
        o->setProperty ("plugins", plugins_);
        auto* st = new juce::DynamicObject(); st->setProperty ("done", done); st->setProperty ("to_do", todo); st->setProperty ("total", (int) items_.size()); o->setProperty ("stage_counts", juce::var (st));
        o->setProperty ("problems", problems_);
        {   // 14 Oct: "N measured so far" - finished measurements without a problem, never a demo check
            int md = 0; for (const auto& i : items_) if (i.done && ! i.problem && i.kind != "demo") ++md; o->setProperty ("measured_done", md);
            // 14 Oct: the Measuring bar's "n of N" (measurements, not demo checks; done counts every landed one)
            int mt = 0, mdn = 0; for (const auto& i : items_) if (i.kind != "demo") { ++mt; if (i.done) ++mdn; }
            auto* mm = new juce::DynamicObject(); mm->setProperty ("done", mdn); mm->setProperty ("total", mt); o->setProperty ("measurements", juce::var (mm)); }
        {   // 15 Oct (Kathy: "Getting ready" sat with an empty bar): the demo / sound checks of the pass now being checked - their own count
            int grp = -2; for (const auto& i : items_) if (i.kind == "demo" && i.running && ! i.done) { grp = i.group; break; }
            int dd = 0, dt = 0; if (grp != -2) for (const auto& i : items_) if (i.kind == "demo" && i.group == grp) { ++dt; if (i.done) ++dd; }
            auto* dm = new juce::DynamicObject(); dm->setProperty ("done", dd); dm->setProperty ("total", dt); dm->setProperty ("running", grp != -2); o->setProperty ("demo_checks", juce::var (dm)); }
        {   // 15 Oct (FEELS FROZEN): each running job (up to 4) with its steps; the plugins done (every measurement of it has an outcome); the heartbeat
            juce::Array<juce::var> jobs;
            for (const auto& i : items_) if (i.running && ! i.done && jobs.size() < 4)
            { auto* j = new juce::DynamicObject(); j->setProperty ("product", i.product); j->setProperty ("kind", i.kind); j->setProperty ("steps", i.steps); j->setProperty ("expected_steps", i.expectedSteps);
              j->setProperty ("fraction", i.kind == "demo" ? -1.0 : std::round (juce::jmax (0.0, stepFraction (i)) * 1000.0) / 1000.0); jobs.add (juce::var (j)); }
            o->setProperty ("jobs_now", jobs);
            std::map<juce::String, std::pair<int, int>> per; for (const auto& i : items_) if (i.kind != "demo") { auto& pr = per[i.product]; ++pr.second; if (i.done) ++pr.first; }
            int pd = 0; for (const auto& [p, dt] : per) if (dt.first == dt.second) ++pd;
            o->setProperty ("plugins_done", pd); o->setProperty ("plugins_total", (int) per.size()); o->setProperty ("heartbeat_ms", (juce::int64) heartbeatMs_);
        }
        o->setProperty ("elapsed_s", std::round (elapsed)); o->setProperty ("eta_s", eta < 0.0 ? juce::var() : juce::var (std::round (eta)));
        o->setProperty ("eta_words", stage_ == Stage::done ? juce::String ("finished") : stopped_ ? juce::String ("stopped") : etaWords (eta)); o->setProperty ("jobs", jobs_);
        auto* tm = new juce::DynamicObject();
        for (const auto* k : { "map", "check", "demo", "compressor", "eq", "deesser", "limiter", "limitercomp", "saturation", "gate", "transient", "multiband", "tuners", "reverb", "delay", "strips" }) { auto* e = new juce::DynamicObject(); e->setProperty ("seconds", std::round (times_.perItem (k) * 10.0) / 10.0); e->setProperty ("source", times_.isMeasured (k) ? "measured" : "starting figure"); tm->setProperty (k, juce::var (e)); }
        o->setProperty ("seconds_per_plugin", juce::var (tm));
        o->setProperty ("line", line); if (summary_.isNotEmpty()) o->setProperty ("summary", summary_);
        o->setProperty ("updated", juce::Time::getCurrentTime().toISO8601 (true));
        const auto tmp = json_.getSiblingFile (json_.getFileName() + ".tmp");
        if (tmp.replaceWithText (juce::JSON::toString (juce::var (o)) + "\n")) tmp.moveFileTo (json_);
    }

    juce::File json_, markers_; std::ostream& term_; bool live_ = false; int jobs_ = 1;
    std::mutex m_; std::thread ticker_; std::atomic<bool> run_ { false };
    Stage stage_ = Stage::scanning; std::vector<Item> items_, queue_; bool queueKnown_ = false;
    Times times_; juce::StringArray current_; std::map<juce::String, double> startedAt_;
    juce::int64 heartbeatMs_ = juce::Time::currentTimeMillis(); double lastStepScan_ = 0.0;
    bool stopped_ = false; int plugins_ = 0, problems_ = 0; double lastPct_ = 0.0, t0_ = 0.0, lastPrinted_ = -1e9;
    juce::String lastKey_, lastLine_, summary_;
};
} // namespace ejmap::wizprog
