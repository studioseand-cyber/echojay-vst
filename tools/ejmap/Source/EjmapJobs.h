/*
  EjmapJobs.h - --jobs N: Phase B rows measured N at a time (Kathy, 10 Oct; the test: cert-traces/2026-10-10-parallel).

  THE RULES
  - Rows run in parallel INSIDE a Phase B step only; the run-all steps keep their order.
  - THE PARENT IS THE ONLY WRITER: a job's prepare (tmp folder, licence gate, arguments) and its finish (filing, moving files into
    place, drafts, the atomic row, progress) run on the calling thread; only the child process (runChild) runs on a worker.
  - THE SERIAL LANE (run alone, the pool drained first): UAD (shared DSP + the Satellite), a product the licence file governs or
    a demo, a PACE / iLok-wrapped product, and any product whose past row was window / needs_licence / timed_out / unhostable /
    silent_output / probe_crashed. Why: the window watch counts windows in the CHILD'S process tree, and a PACE / iLok window
    belongs to a helper outside it - with other jobs running it could not be pinned to a product.
  - Default N = 1 (exactly the serial run); no new job while free memory is low (unless nothing is running); the hang guard and
    the sleep reading stay per child; after a stop (07:00 / Ctrl-C) no new child starts and a job that ends after it writes no row
    (its child was stopped with it) - the row runs again on the resume, as a half-done product always has.
*/
#pragma once

#include <juce_core/juce_core.h>
#include <mach/mach.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace ejmap::jobs
{

enum class Lane { parallel, serial };
struct LaneInputs { bool uad = false, governed = false, demo = false, pace = false; juce::String paceWhy; std::set<juce::String> pastOutcomes; };
inline const juce::StringArray& kSerialHistory() { static const juce::StringArray k { "window", "needs_licence", "timed_out", "unhostable", "silent_output", "probe_crashed" }; return k; }
inline Lane laneFor (const LaneInputs& in, juce::String* why = nullptr)
{
    auto say = [&] (const juce::String& w) { if (why != nullptr) *why = w; return Lane::serial; };
    if (in.uad) return say ("UAD (shared DSP, the Satellite)");
    if (in.governed || in.demo) return say ("governed by the licence file");
    if (in.pace) return say ("PACE / iLok: " + (in.paceWhy.isNotEmpty() ? in.paceWhy : juce::String ("wrapped")));
    for (const auto& o : in.pastOutcomes) if (kSerialHistory().contains (o)) return say ("a past row filed " + o);
    return Lane::parallel;
}

// free + inactive + purgeable pages: what a new job can take without paging
inline double freeMemoryBytes()
{
    vm_statistics64_data_t s {}; mach_msg_type_number_t n = HOST_VM_INFO64_COUNT; vm_size_t page = 0;
    if (host_page_size (mach_host_self(), &page) != KERN_SUCCESS || host_statistics64 (mach_host_self(), HOST_VM_INFO64, (host_info64_t) &s, &n) != KERN_SUCCESS) return -1.0;
    return (double) (s.free_count + s.inactive_count + s.purgeable_count) * (double) page;
}
inline constexpr double kMinFreeBytes = 1.5 * 1024.0 * 1024.0 * 1024.0;
inline bool memoryLow (double freeBytes) { return freeBytes >= 0.0 && freeBytes < kMinFreeBytes; }   // unreadable (-1) is not "low"

// THE SCHEDULER, pure: what to do with the head of the queue
enum class Act { launch, wait, done };
struct Sched { int jobs = 1, running = 0; bool serialRunning = false, stop = false, memLow = false; };
inline Act next (const Sched& s, std::optional<Lane> head)
{
    if (! head || s.stop) return s.running > 0 ? Act::wait : Act::done;   // a stop starts no new child
    if (s.serialRunning) return Act::wait;                                 // a serial job runs alone
    if (*head == Lane::serial) return s.running == 0 ? Act::launch : Act::wait;   // ... after the pool has drained
    if (s.running >= juce::jmax (1, s.jobs)) return Act::wait;
    if (s.memLow && s.running > 0) return Act::wait;                       // no new job while memory is low; one always runs
    return Act::launch;
}

// THE POOL. prepare (calling thread) returns the job's work, or nullopt when it was handled without a child (a licence stop, a
// device row left unrun); run (a worker) is the child alone; finish (calling thread) files it; discard (calling thread) is a job
// that ended after a stop. Items run in queue order; a serial item waits for the pool to drain and then runs alone.
template <class Item, class Work, class Result>
struct Pool
{
    std::function<Lane (const Item&)> laneOf;
    std::function<std::optional<Work> (const Item&)> prepare;
    std::function<Result (const Work&)> run;
    std::function<void (const Item&, Work&, Result&)> finish;
    std::function<void (const Item&, Work&)> discard;
    std::function<bool()> stopped = [] { return false; };
    std::function<bool()> lowMemory = [] { return false; };

    void operator() (const std::vector<Item>& items, int jobs)
    {
        struct Running { size_t item; Work work; Lane lane; std::thread t; bool doneFlag = false; std::optional<Result> result; };
        std::mutex m; std::condition_variable cv;
        std::vector<std::unique_ptr<Running>> live; size_t head = 0;
        for (;;)
        {
            // collect every finished job first (calling thread), so a finish never races another finish
            for (auto it = live.begin(); it != live.end();)
            {
                bool fin; { std::lock_guard<std::mutex> g (m); fin = (*it)->doneFlag; }
                if (! fin) { ++it; continue; }
                (*it)->t.join();
                if (stopped()) discard (items[(*it)->item], (*it)->work); else finish (items[(*it)->item], (*it)->work, *(*it)->result);
                it = live.erase (it);
            }
            Sched s; s.jobs = jobs; s.running = (int) live.size(); s.stop = stopped(); s.memLow = lowMemory();
            for (const auto& r : live) if (r->lane == Lane::serial) s.serialRunning = true;
            const auto act = next (s, head < items.size() ? std::optional<Lane> (laneOf (items[head])) : std::nullopt);
            if (act == Act::done) break;
            if (act == Act::wait) { std::unique_lock<std::mutex> lk (m); cv.wait_for (lk, std::chrono::milliseconds (50), [&] { for (const auto& r : live) if (r->doneFlag) return true; return false; }); continue; }
            const size_t idx = head++;
            auto w = prepare (items[idx]);
            if (! w) continue;
            auto r = std::make_unique<Running>(); r->item = idx; r->work = std::move (*w); r->lane = laneOf (items[idx]);
            auto* rp = r.get();
            r->t = std::thread ([this, rp, &m, &cv] { auto res = run (rp->work); { std::lock_guard<std::mutex> g (m); rp->result = std::move (res); rp->doneFlag = true; } cv.notify_all(); });
            live.push_back (std::move (r));
        }
    }
};

} // namespace ejmap::jobs
