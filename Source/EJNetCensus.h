#pragma once
// =============================================================================
//  THE NETWORK CENSUS (7 Oct 2026, round 06d item 1)
//
//  WHY THIS EXISTS. ~EchoJayProcessor is careful about its OWN threads - it
//  calls pluginScanner.requestStop() then loadThread.join(), then waits up to
//  5 s for saveThread - and then returns with up to seven DETACHED network
//  workers still inside a blocking read. Every one of them checks its `alive`
//  flag only BEFORE the blocking call, and the config fetch's connection
//  timeout is 60 SECONDS, so quitting a host within a minute of opening
//  EchoJay - offline, or on a slow network - leaves the shared NSURLSession
//  being torn down while the process is coming down.
//
//  THAT IS A CRASH, and it is not ours to fix inside CFNetwork. The 06c gate
//  caught it (.ips BC815958, 7 Oct 19:12:25): SIGSEGV at 0x0 on
//  com.apple.NSURLSession-work, inside
//      -[__NSCFURLSessionDelegateWrapper didBecomeInvalidWithError:]
//      -[NSURLSession finalizeDelegateWithError:]
//  CFNetwork finalising the session and messaging a delegate that has already
//  been released - JUCE's JUCE_URLDelegate_, owned by SharedSession
//  (juce_Network_mac.mm:132-200) and destroyed when the LAST in-flight
//  WebInputStream goes away. Under MallocScribble the released delegate's
//  memory is poisoned instead of merely stale, which is why only the scribble
//  leg of a guard ever saw it.
//
//  SO THE INVARIANT IS OURS: no EchoJay network worker is in flight once the
//  last EchoJayProcessor's destructor has returned. Three parts:
//    1. a census every launched worker enters and leaves (Worker, below), so
//       the count and the NAMES of what is in flight are always available;
//    2. a one-way stopping flag the workers check AFTER the blocking call, so
//       a late completion posts nothing;
//    3. cancellation: `open()` is a drop-in for url.createInputStream(options)
//       that returns a juce::WebInputStream and registers it, so beginShutdown()
//       can call WebInputStream::cancel() on it - which UNBLOCKS the read
//       rather than waiting out the 60 s timeout. That is the same mechanism
//       the chat stream has used since 21q (ChatStreamHandle::attach/cancel);
//       this generalises it to the five JSON verbs and the editor's fetches.
//
//  THE LAST PROCESSOR, NOT ANY PROCESSOR. ChainHost::noteHostTeardownBegan is
//  deliberately process-wide and one-way, because a hosted AU dispose is
//  dangerous in any instance once the process is coming down. The network is
//  the opposite case: taking EchoJay off ONE track while another instance is
//  mid-chat must not cancel that chat. So liveProcessors() is counted and the
//  shutdown runs only when it reaches zero.
// =============================================================================
#include <juce_core/juce_core.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace echojay::net
{

class Worker;

// ---- the census state -------------------------------------------------------
// Header-only on purpose (Source/*.cpp is an explicit CMake list in four
// targets; a new .cpp would be four edits for ten lines of state).
inline std::mutex&              censusLock()     { static std::mutex m;                 return m; }
inline std::vector<Worker*>&    censusWorkers()  { static std::vector<Worker*> v;       return v; }   // guarded by censusLock()
inline std::atomic<int>&        liveWorkers()    { static std::atomic<int> n { 0 };     return n; }
inline std::atomic<bool>&       stoppingFlag()   { static std::atomic<bool> b { false }; return b; }
inline std::atomic<int>&        liveProcessors() { static std::atomic<int> n { 0 };     return n; }

inline bool stopping() noexcept { return stoppingFlag().load(); }
inline int  inFlight() noexcept { return liveWorkers().load(); }

// ---- one per launched worker ------------------------------------------------
// Declared at the TOP of the worker lambda, before any request. Its name is
// what the teardown log prints when the bounded wait times out, so it is the
// request's name, not the thread's.
class Worker
{
public:
    explicit Worker (juce::String whatFor)
        : what_ (whatFor.isNotEmpty() ? std::move (whatFor) : juce::String ("an unnamed request"))
    {
        const std::scoped_lock lock { censusLock() };
        censusWorkers().push_back (this);
        liveWorkers().fetch_add (1);
    }

    ~Worker()
    {
        const std::scoped_lock lock { censusLock() };
        auto& v = censusWorkers();
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == this) { v.erase (v.begin() + (long) i); break; }
        stream_ = nullptr;
        liveWorkers().fetch_sub (1);
    }

    Worker (const Worker&) = delete;
    Worker& operator= (const Worker&) = delete;

    // The flag every worker checks AFTER its blocking call, before it writes
    // anything or posts a completion.
    bool stopping() const noexcept { return echojay::net::stopping(); }

    juce::String what() const { return what_; }

    // Called by open() with the stream it is about to connect, and by the
    // stream's own destruction with nullptr. Under the census lock, so a
    // cancelling thread either reaches a live stream or a nullptr - never a
    // destroyed one. (The same argument ChatStreamHandle::attach documents.)
    void setStream (juce::WebInputStream* s) noexcept
    {
        const std::scoped_lock lock { censusLock() };
        stream_ = s;
    }

    // CALLER HOLDS censusLock(). cancel() is safe from any thread: it flips the
    // stream's own flag and closes its socket, which unblocks a read in
    // progress instead of waiting out the connection timeout.
    void cancelLocked() noexcept { if (stream_ != nullptr) stream_->cancel(); }

private:
    juce::String what_;
    juce::WebInputStream* stream_ = nullptr;
};

// What is in flight, by name, for the one log line that has to say it.
inline juce::String inFlightNames()
{
    const std::scoped_lock lock { censusLock() };
    juce::StringArray names;
    for (auto* w : censusWorkers())
        if (w != nullptr) names.add (w->what());
    return names.isEmpty() ? juce::String ("nothing") : names.joinIntoString (", ");
}

// ---- the drop-in for url.createInputStream(options) -------------------------
// Request IS the stream, for the duration of the read, registered the whole
// time. Byte for byte the same request as before: the body below is
// juce::URL::createInputStream's own (juce_URL.cpp), minus the local-file
// branch no API call can take. The progress callback is not forwarded because
// no EchoJay request sets one; the one site that wants progress (the update
// download) keeps createInputStream and says so.
//
// A CALL SITE CANNOT GET THE ORDER WRONG, which is the point of wrapping it:
// the destructor clears the worker's slot and only THEN drops the stream, so a
// cancelling thread holding the census lock can never reach a stream that is
// being destroyed.
class Request
{
public:
    Request (Worker& worker, const juce::URL& url, const juce::URL::InputStreamOptions& options)
        : worker_ (worker)
    {
        if (stopping()) return;   // teardown has begun: never open a new socket

        const auto usePost = options.getParameterHandling() == juce::URL::ParameterHandling::inPostData;
        auto s = std::make_unique<juce::WebInputStream> (url, usePost);

        if (const auto extra = options.getExtraHeaders(); extra.isNotEmpty())
            s->withExtraHeaders (extra);
        if (const auto timeout = options.getConnectionTimeoutMs(); timeout != 0)
            s->withConnectionTimeout (timeout);
        if (const auto cmd = options.getHttpRequestCmd(); cmd.isNotEmpty())
            s->withCustomRequestCommand (cmd);
        s->withNumRedirectsToFollow (options.getNumRedirectsToFollow());

        worker_.setStream (s.get());
        if (stopping()) s->cancel();   // a shutdown that landed between the check above and here

        const auto success = s->connect (nullptr);

        if (auto* status = options.getStatusCode())
            *status = s->getStatusCode();
        if (auto* headers = options.getResponseHeaders())
            *headers = s->getResponseHeaders();

        if (success && ! s->isError())
            stream_ = std::move (s);
        else
            worker_.setStream (nullptr);
    }

    ~Request()
    {
        worker_.setStream (nullptr);   // FIRST: nobody can cancel it from here on
        stream_.reset();               // THEN the socket goes
    }

    Request (const Request&) = delete;
    Request& operator= (const Request&) = delete;

    // `if (stream)` where the old code said `if (stream != nullptr)`.
    explicit operator bool() const noexcept { return stream_ != nullptr; }
    juce::WebInputStream* operator-> () const noexcept { return stream_.get(); }
    juce::WebInputStream* get() const noexcept { return stream_.get(); }

private:
    Worker& worker_;
    std::unique_ptr<juce::WebInputStream> stream_;
};

// ---- shutdown ---------------------------------------------------------------
// One-way, like the teardown flag, and for the same reason: a process that has
// started coming down does not start talking to the network again.
inline void beginShutdown() noexcept
{
    stoppingFlag().store (true);
    const std::scoped_lock lock { censusLock() };
    for (auto* w : censusWorkers())
        if (w != nullptr) w->cancelLocked();
}

// Bounded, and it reports which way it ended - a wait that cannot time out is
// a hang, and a timeout nobody can see is the bug this whole file is about.
inline bool waitUntilQuiet (int timeoutMs) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
    while (inFlight() > 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    return inFlight() == 0;
}

// TESTS ONLY. The flag is one-way in a real process; a harness that builds and
// destroys several processors in one run needs it back.
inline void resetForTest() noexcept { stoppingFlag().store (false); }

} // namespace echojay::net
