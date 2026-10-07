// net_quiet_guard (7 Oct 2026, round 06d item 1) - NO ECHOJAY NETWORK WORKER OUTLIVES THE LAST PROCESSOR.
//
// WHAT THIS IS A TEST OF, AND WHY IT IS NOT A TEST OF THE CRASH. The 06c gate went red on
// dialinfo_keep_guard's scribble leg with a SIGSEGV that was not in our code: CFNetwork finalising the
// shared NSURLSession and messaging JUCE's already-released delegate, on com.apple.NSURLSession-work
// (.ips BC815958, 7 Oct 19:12:25). Measured before any fix, that crash is 0 of 20 runs sealed and 0 of 20
// unsealed - about one in forty - because it needs the session invalidation to land inside the exit window.
// A 20-run leg on a 1-in-40 crash proves nothing in either direction. So this guard tests the CONDITION we
// control and can make deterministic: a request still in flight when ~EchoJayProcessor returns.
//
// (1) THE PRODUCT INVARIANT, on the real processor. Deterministic because the base URL is pointed at
//     10.255.255.1, which answers nothing, so the connect blocks for its full timeout - 60 SECONDS for the
//     config fetch. Written into the ISOLATED state root, so no live file is touched.
//     ITS RED IS NOT A RUN, AND I AM SAYING SO: before this round there was no census to read and no wait
//     in the destructor at all, so the pre-fix tree cannot compile this leg. The RED evidence is the
//     measurement in the handoff: 15 of 20 runs had a request in flight at exit, and the destructor
//     returned in milliseconds every time.
// (2) and (3) ARE two-sided, in one binary, on the waiting machinery itself - the part a future change can
//     break silently: a worker that HONOURS cancellation goes quiet inside the bound, and a worker that
//     IGNORES it makes the wait return FALSE inside the bound rather than hang. A wait that cannot time out
//     is a hang, and a timeout nobody can see is the bug this whole round is about.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EJNetCensus.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>
#include <thread>

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(),
                 d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
// A LOCAL ENDPOINT THAT ACCEPTS AND NEVER ANSWERS - and the reason it is not a
// blackhole address. The first cut pointed the base URL at 10.255.255.1, which
// nothing answers, so the worker sat in a CONNECT. Measured: the destructor's
// cancel could not shorten that, because an unresolved connect is abandoned on
// the OS's schedule, not ours - 2.1 s in one run, over 8 s in the next. That is
// not the case a host quit presents either. A host quit interrupts a request to
// a server that ANSWERED and is being read, and NSURLSession aborts a connected
// task at once. So the fixture accepts the connection and then says nothing:
// the worker is deterministically inside a read, which is what we must be able
// to cancel. Hermetic - 127.0.0.1, no network, no server.
class DeafServer
{
public:
    bool start()
    {
        for (int port = 49230; port < 49330; ++port)
            if (listener_.createListener (port, "127.0.0.1")) { port_ = port; break; }
        if (port_ == 0) return false;
        thread_ = std::thread ([this]
        {
            while (! stop_.load())
                if (auto* c = listener_.waitForNextConnection())
                {
                    const std::scoped_lock lock { mutex_ };
                    held_.emplace_back (c);          // held open, never written to
                }
        });
        return true;
    }
    int port() const noexcept { return port_; }
    ~DeafServer()
    {
        stop_ = true;
        listener_.close();
        if (thread_.joinable()) thread_.join();
        const std::scoped_lock lock { mutex_ };
        held_.clear();
    }
private:
    juce::StreamingSocket listener_;
    std::vector<std::unique_ptr<juce::StreamingSocket>> held_;
    std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stop_ { false };
    int port_ = 0;
};

void pointAt (int port)
{
    const auto dir = echojay::userAppData().getChildFile ("EchoJay");
    dir.createDirectory();
    dir.getChildFile ("dev_base_url.txt")
       .replaceWithText ("http://127.0.0.1:" + juce::String (port));
}
constexpr int kDestructorBoundMs = 6000;   // the destructor waits 2000; a hang blows this
constexpr int kWaitBoundMs       = 400;    // legs 2/3: the bound the wait itself is given
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    echojay::requireIsolationOrDie ("net_quiet_guard");
    juce::ScopedJuceInitialiser_GUI gui;
    DeafServer deaf;
    if (! deaf.start())
    {
        std::printf ("  RUNNER REFUSED: no local port for the fixture, so leg (1) would test nothing\n");
        return 2;
    }
    pointAt (deaf.port());
    std::printf ("net_quiet_guard: no network worker outlives the last processor"
                 " (fixture: 127.0.0.1:%d accepts and never answers)\n", deaf.port());

    std::printf ("== (1) the product invariant: a blocked request, then the destructor ==\n");
    {
        auto p = std::make_unique<EchoJayProcessor>();
        p->prepareToPlay (48000.0, 512);

        // Wait for the worker to be inside the request AND past the connect, so the
        // cancel under test is a cancel of a live socket. A leg that raced the thread
        // start would pass by measuring nothing at all.
        for (int i = 0; i < 300 && echojay::net::inFlight() == 0; ++i)
            juce::Thread::sleep (10);
        juce::Thread::sleep (750);   // connect + request sent; the read is now blocked on the deaf server

        const int inFlight = echojay::net::inFlight();
        const auto names   = echojay::net::inFlightNames();
        check (inFlight > 0,
               "a request is in flight while the processor is alive (the precondition: without it this leg tests nothing)",
               juce::String (inFlight) + " in flight: " + names);

        const auto t0 = juce::Time::getMillisecondCounter();
        p.reset();                                  // <-- ~EchoJayProcessor, timed exactly
        const int tookMs = (int) (juce::Time::getMillisecondCounter() - t0);

        check (echojay::net::inFlight() == 0,
               "THE INVARIANT: the census is empty once ~EchoJayProcessor has returned",
               juce::String (echojay::net::inFlight()) + " still in flight: " + echojay::net::inFlightNames());
        check (tookMs <= kDestructorBoundMs,
               "and it returned inside the bound (a wait that cannot time out is a hang)",
               juce::String (tookMs) + " ms, bound " + juce::String (kDestructorBoundMs));
        check (echojay::net::stopping(),
               "the one-way network shutdown flag is set by the last processor's destructor");
    }

    // The flag is one-way in a real process; these two legs need it back. And they
    // need an EMPTY census: leg (2) first read 2, because leg (1)'s blackhole worker
    // was still leaving. A leg that counts someone else's worker is not a leg.
    echojay::net::resetForTest();
    for (int i = 0; i < 600 && echojay::net::inFlight() != 0; ++i) juce::Thread::sleep (10);
    check (echojay::net::inFlight() == 0,
           "the census is empty again before the next leg starts (leg isolation)",
           juce::String (echojay::net::inFlight()) + ": " + echojay::net::inFlightNames());

    std::printf ("== (2) a worker that honours cancellation: quiet inside the bound ==\n");
    {
        std::atomic<bool> stop { false }, started { false };
        std::thread t ([&]
        {
            echojay::net::Worker w ("an obedient worker");
            started = true;
            while (! w.stopping() && ! stop.load()) juce::Thread::sleep (2);
        });
        for (int i = 0; i < 300 && ! started.load(); ++i) juce::Thread::sleep (2);
        check (echojay::net::inFlight() == 1, "it is the one worker in the census",
               juce::String (echojay::net::inFlight()) + ": " + echojay::net::inFlightNames());

        const auto t0 = juce::Time::getMillisecondCounter();
        echojay::net::beginShutdown();
        const bool quiet = echojay::net::waitUntilQuiet (kWaitBoundMs);
        const int took = (int) (juce::Time::getMillisecondCounter() - t0);
        stop = true; t.join();
        check (quiet, "waitUntilQuiet returned TRUE", juce::String (took) + " ms");
        check (took <= kWaitBoundMs, "inside the bound", juce::String (took) + " ms of " + juce::String (kWaitBoundMs));
    }

    echojay::net::resetForTest();

    std::printf ("== (3) the other direction: a worker that IGNORES cancellation is reported, not waited on for ever ==\n");
    {
        std::atomic<bool> release { false }, started { false };
        std::thread t ([&]
        {
            echojay::net::Worker w ("a deaf worker");       // never checks stopping()
            started = true;
            while (! release.load()) juce::Thread::sleep (2);
        });
        for (int i = 0; i < 300 && ! started.load(); ++i) juce::Thread::sleep (2);

        const auto t0 = juce::Time::getMillisecondCounter();
        echojay::net::beginShutdown();
        const bool quiet = echojay::net::waitUntilQuiet (kWaitBoundMs);
        const int took = (int) (juce::Time::getMillisecondCounter() - t0);
        check (! quiet, "waitUntilQuiet returned FALSE - it does not claim quiet it did not get");
        check (took >= kWaitBoundMs - 50 && took <= kWaitBoundMs + 500,
               "and it returned at the bound, neither early nor never",
               juce::String (took) + " ms of " + juce::String (kWaitBoundMs));
        check (echojay::net::inFlightNames().contains ("a deaf worker"),
               "and it can NAME what is still in flight, which is what the teardown log prints",
               echojay::net::inFlightNames());
        release = true; t.join();
    }

    std::printf ("\n==== net_quiet_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
