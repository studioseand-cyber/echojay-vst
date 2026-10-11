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
    if (kind == "map") return 4.0; if (kind == "check") return 6.0; if (kind == "eq") return 36.0; if (kind == "pack") return 2.0;
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
};

struct Item { juce::String product, kind; bool done = false, running = false, problem = false, serial = false; double ranS = 0.0; };   // ranS: seconds a running item has run

// RESUME: a queue item whose row already landed (product -> its outcome) starts done; a landed row that is not "ok" is a problem
inline std::vector<Item> resumeItems (std::vector<Item> queue, const std::map<juce::String, juce::String>& landed)
{
    for (auto& i : queue) if (const auto it = landed.find (i.product); it != landed.end()) { i.done = true; i.problem = it->second != "ok"; }
    return queue;
}
// the share of a stage done, weighted by expected seconds; an empty stage is complete. A RUNNING plugin earns its time run so far, up
// to 90% of its expected seconds (the 11 Oct rehearsal: with two 6-minute compressors running the bar sat still for 6 minutes)
inline double stageShare (const std::vector<Item>& items, const Times& t)
{
    double all = 0.0, done = 0.0;
    for (const auto& i : items) { const auto w = t.perItem (i.kind); all += w; if (i.done) done += w; else if (i.running) done += juce::jmin (0.9 * w, juce::jmax (0.0, i.ranS)); }
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
inline double stageSecondsLeft (const std::vector<Item>& items, const Times& t, int jobs)
{
    double par = 0.0, ser = 0.0;
    for (const auto& i : items)
    {
        if (i.done) continue;
        const double e = t.perItem (i.kind); const double s = i.running ? juce::jmax (0.1 * e, e - i.ranS) : e;
        (i.serial ? ser : par) += s;
    }
    return par / (double) juce::jmax (1, jobs) + ser;
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
    void started (const juce::String& product)
    {
        std::lock_guard<std::mutex> g (m_);
        for (auto& i : items_) if (i.product == product && ! i.done) { i.running = true; startedAt_[product] = juce::Time::getMillisecondCounterHiRes(); }
        if (! current_.contains (product)) current_.add (product);
        if (markers_ != juce::File()) wizapp::markerBegin (markers_, product, stageWord (stage_));
        emitLocked (false);
    }
    void finished (const juce::String& product, bool problem, double seconds = -1.0)
    {
        std::lock_guard<std::mutex> g (m_);
        for (auto& i : items_) if (i.product == product && ! i.done)
        {
            i.done = true; i.running = false; i.problem = problem;
            double s = seconds; if (s < 0.0 && startedAt_.count (product)) s = (juce::Time::getMillisecondCounterHiRes() - startedAt_[product]) / 1000.0;
            if (! problem && s > 0.0) times_.add (i.kind, s);
            if (problem) ++problems_;
            break;
        }
        current_.removeString (product);
        if (markers_ != juce::File()) wizapp::markerEnd (markers_, product);
        emitLocked (true);
    }
    void ranFor (const juce::String& product, double s)   // the clock as if the plugin started s seconds ago (pins; a window app's restore)
    { std::lock_guard<std::mutex> g (m_); startedAt_[product] = juce::Time::getMillisecondCounterHiRes() - s * 1000.0; emitLocked (false); }
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
    std::pair<int, int> stageDone() { std::lock_guard<std::mutex> g (m_); int d = 0; for (const auto& i : items_) if (i.done) ++d; return { d, (int) items_.size() }; }
    int running() { std::lock_guard<std::mutex> g (m_); int r = 0; for (const auto& i : items_) if (i.running && ! i.done) ++r; return r; }
    double stoppedAt() { std::lock_guard<std::mutex> g (m_); return stoppedPercent (stage_, items_, times_); }
    juce::String lastLine() const { return lastLine_; }

private:
    juce::String lineLocked (double pct, double eta) const
    {
        int done = 0; for (const auto& i : items_) if (i.done) ++done;
        juce::String what = stageWord (stage_);
        if (stage_ != Stage::done && ! items_.empty()) what << " " << juce::String (juce::jmin ((int) items_.size(), done + 1)) << " of " << juce::String ((int) items_.size());
        if (current_.size() > 0) what << " - " << current_[0] << (jobs_ > 1 && (stage_ == Stage::scanning || stage_ == Stage::measuring) ? " (" + juce::String (jobs_) + " at a time)" : juce::String());
        if (stopped_) return barText (pct) + " " + juce::String ((int) std::floor (pct)).paddedLeft (' ', 3) + "%  Stopped";
        return barText (pct) + " " + juce::String ((int) std::floor (pct)).paddedLeft (' ', 3) + "%  " + what + (stage_ == Stage::done ? juce::String() : " - " + etaWords (eta));
    }
    void emitLocked (bool event, bool writeFile = true)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        for (auto& i : items_) if (i.running && startedAt_.count (i.product)) i.ranS = (nowMs - startedAt_[i.product]) / 1000.0;
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
        juce::Array<juce::var> cur; for (const auto& c : current_) cur.add (c); o->setProperty ("current", cur);
        o->setProperty ("plugins", plugins_);
        auto* st = new juce::DynamicObject(); st->setProperty ("done", done); st->setProperty ("to_do", todo); st->setProperty ("total", (int) items_.size()); o->setProperty ("stage_counts", juce::var (st));
        o->setProperty ("problems", problems_);
        o->setProperty ("elapsed_s", std::round (elapsed)); o->setProperty ("eta_s", eta < 0.0 ? juce::var() : juce::var (std::round (eta)));
        o->setProperty ("eta_words", stage_ == Stage::done ? juce::String ("finished") : stopped_ ? juce::String ("stopped") : etaWords (eta)); o->setProperty ("jobs", jobs_);
        auto* tm = new juce::DynamicObject();
        for (const auto* k : { "map", "check", "compressor", "eq" }) { auto* e = new juce::DynamicObject(); e->setProperty ("seconds", std::round (times_.perItem (k) * 10.0) / 10.0); e->setProperty ("source", times_.isMeasured (k) ? "measured" : "starting figure"); tm->setProperty (k, juce::var (e)); }
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
    bool stopped_ = false; int plugins_ = 0, problems_ = 0; double lastPct_ = 0.0, t0_ = 0.0, lastPrinted_ = -1e9;
    juce::String lastKey_, lastLine_, summary_;
};
} // namespace ejmap::wizprog
