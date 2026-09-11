/*
    EedLatencyLog.h - the timestamped latency log (round 49, 5 Sep 2026,
    DEFECT_PRESS_PLAY_PHASING item 1). Compiled in ONLY when EJ_LATENCY_LOG=1
    (CMake option EJ_LATENCY_LOG, the build-latencylog directory); in every
    other build every macro here is a no-op and ejSetLatencyLogged is a plain
    forward to setLatencySamples.

    Writes /Users/SeanD/echojay-vst/latency-logs/latency.log (a CONNECTED
    folder): wall clock, ms since the log
    opened, thread, then the line. Writes happen on whichever thread calls -
    including the audio thread - which is why this is a debug build and not
    the one Sean works in.
*/
#pragma once

#if EJ_LATENCY_LOG
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

inline void ejLatencyLog (const char* fmt, ...)
{
    static std::mutex m;
    static FILE* f = nullptr;
    static bool tried = false;
    static auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock (m);
    if (f == nullptr)
    {
        if (tried) return;
        tried = true;
        // STANDING RULE (round 50): ANY FILE THE USER MUST PRODUCE OR RETRIEVE
        // GOES IN A CONNECTED FOLDER - the repo, never ~/Library, never the
        // Desktop, never a temp path. The cloud side reads it from here.
        const char* home = std::getenv ("HOME");
        std::string dir = std::string (home != nullptr ? home : "/Users/SeanD") + "/echojay-vst/latency-logs";
        ::mkdir (dir.c_str(), 0755);
        f = std::fopen ((dir + "/latency.log").c_str(), "a");
        if (f == nullptr) return;
        std::fprintf (f, "\n==== EchoJay latency log opened, pid %d ====\n", (int) ::getpid());
    }
    timeval tv; ::gettimeofday (&tv, nullptr);
    tm lt; ::localtime_r (&tv.tv_sec, &lt);
    const double sinceMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    char head[96];
    std::snprintf (head, sizeof head, "%02d:%02d:%02d.%03d  +%10.1f ms  thr %08llx  ",
                   lt.tm_hour, lt.tm_min, lt.tm_sec, (int) (tv.tv_usec / 1000), sinceMs,
                   (unsigned long long) std::hash<std::thread::id>{} (std::this_thread::get_id()) & 0xffffffffULL);
    std::fputs (head, f);
    va_list ap; va_start (ap, fmt); std::vfprintf (f, fmt, ap); va_end (ap);
    std::fputc ('\n', f);
    std::fflush (f);
}
#define EJ_LAT_LOG(...) ejLatencyLog (__VA_ARGS__)

template <class P>
inline void ejSetLatencyLogged (P& p, int v, const char* who)
{
    const int old = p.getLatencySamples();
    ejLatencyLog ("setLatencySamples  %-44s old %6d  new %6d  %s", who, old, v,
                  old == v ? "unchanged (no host notification)" : "CHANGED -> host notified");
    p.setLatencySamples (v);
}
#else
#define EJ_LAT_LOG(...) ((void) 0)
template <class P>
inline void ejSetLatencyLogged (P& p, int v, const char*) { p.setLatencySamples (v); }
#endif
