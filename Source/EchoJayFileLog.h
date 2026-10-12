// EchoJayFileLog.h — 21p item 4 (23 Sep 2026): the plugin writes its own rolling log.
//
// WHY. The 23 Sep noon distortion could not be explained from evidence: EchoJay logs through os_log, and the
// unified store held NO EchoJay line from that Pro Tools instance - the loop's readings, the gains it wrote, the
// trims per slot, the dial writes and their read-backs existed only there and were gone. A log the plugin owns
// cannot be dropped by someone else's traffic.
//
// WHAT. <state root>/Library/EchoJay/logs/echojay-0.log … echojay-4.log, 2 MB each, the oldest slot overwritten.
// Every EchoJay_NSLog line lands here as well as in os_log, so nothing is routed twice and the MOVE LOG rides along.
//
// NO JUCE HERE, deliberately: this header is included by NativeClip.mm, and pulling JuceHeader.h into an ObjC++
// translation unit that already imports Cocoa breaks the juce_gl include order. POSIX only, one mutex, one append.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
namespace echojay
{
class FileLog
{
public:
    static constexpr int  kFiles = 5;
    static constexpr long kMaxBytes = 2L * 1024L * 1024L;

    static FileLog& instance() { static FileLog f; return f; }

    static std::string dir()
    {
        const char* iso = std::getenv ("ECHOJAY_STATE_HOME");
        std::string root = (iso != nullptr && *iso != 0) ? std::string (iso)
                                                         : std::string (std::getenv ("HOME") != nullptr ? std::getenv ("HOME") : "/tmp");
        return root + "/Library/EchoJay/logs";
    }
    void write (const char* line)
    {
        if (line == nullptr) return;
        const std::lock_guard<std::mutex> lk (m_);
        if (! open_) openCurrent();
        if (path_.empty()) return;
        std::FILE* f = std::fopen (path_.c_str(), "a");
        if (f == nullptr) return;
        char stamp[32];
        const std::time_t t = std::time (nullptr);
        std::tm tm {};
        localtime_r (&t, &tm);
        std::strftime (stamp, sizeof (stamp), "%Y-%m-%d %H:%M:%S", &tm);
        // THE PID IS PART OF EVERY LINE (11 Oct 2026). One Logic session runs the V2 and eighty-odd Links, all
        // appending here, and until now a line could not be traced to the process that wrote it - so "the Link
        // never applied" and "the V2 never sent" were indistinguishable in the one place that could have settled
        // them. Attribution is not a luxury in a multi-process log; it is the difference between evidence and a
        // guess.
        const int n = std::fprintf (f, "%s  [%d] %s\n", stamp, (int) ::getpid(), line);
        std::fclose (f);
        if (n > 0) bytes_ += n;
        // bytes_ counts only OUR OWN appends. With many writers the file reaches the cap long before any single
        // process's count does, which is how echojay-2.log was observed at 2,433,420 bytes - over a 2 MB cap that
        // every writer believed it was honouring. Re-stat occasionally so the cap is measured, not assumed.
        if (++sinceStat_ >= 64)
        {
            sinceStat_ = 0;
            const long real = sizeOf (path_);
            if (real > bytes_) bytes_ = real;
        }
        if (bytes_ >= kMaxBytes) rotate();
    }
    std::string currentPath() { const std::lock_guard<std::mutex> lk (m_); if (! open_) openCurrent(); return path_; }

private:
    static void makeDirs (const std::string& p)
    {
        std::string acc;
        for (std::size_t i = 0; i <= p.size(); ++i)
            if (i == p.size() || p[i] == '/')
            { acc = p.substr (0, i); if (acc.size() > 1) ::mkdir (acc.c_str(), 0755); }
    }
    static long sizeOf (const std::string& p) { struct ::stat st {}; return ::stat (p.c_str(), &st) == 0 ? (long) st.st_size : -1; }
    static long mtimeOf (const std::string& p) { struct ::stat st {}; return ::stat (p.c_str(), &st) == 0 ? (long) st.st_mtime : -1; }
    std::string fileAt (int i) const { return dir() + "/echojay-" + std::to_string (i) + ".log"; }
    void openCurrent()
    {
        open_ = true;
        makeDirs (dir());
        pickLocked();
    }
    void rotate() { pickLocked(); }

    // ---- ONE RULE EVERY PROCESS AGREES ON (11 Oct 2026) -----------------------------------------------
    // What stood here picked the newest file by MTIME and rotated to (index + 1), each process keeping its own
    // index. With many writers that is not a ring, it is a demolition derby: process A, full at its own count,
    // rotated onto a slot process B was happily appending to and `std::remove`d B's lines. That is why an absence
    // in this log could not be read as "it did not happen" - the strongest claim a log can make was unavailable
    // in the one file meant to carry it.
    //
    // The rule now: THE NEWEST SLOT STILL UNDER THE CAP, and when every slot is full, recycle the OLDEST. It is a
    // pure function of what is on disk, so two processes evaluating it reach the same answer; the lock makes the
    // recycle step atomic so they cannot recycle different slots in the same instant.
    void pickLocked()
    {
        const std::string lockPath = dir() + "/.rotate.lock";
        const int lk = ::open (lockPath.c_str(), O_CREAT | O_RDWR, 0644);
        if (lk >= 0) ::flock (lk, LOCK_EX);

        long newestM = -1; int pick = -1;
        for (int i = 0; i < kFiles; ++i)
        {
            const long sz = sizeOf (fileAt (i));
            if (sz < 0) { pick = i; break; }                 // an unused slot wins: start there
            if (sz >= kMaxBytes) continue;                   // full: not a candidate
            const long m = mtimeOf (fileAt (i));
            if (m > newestM) { newestM = m; pick = i; }
        }
        if (pick < 0)
        {   // every slot is full: the OLDEST is recycled, which is the only choice that loses the least history
            long oldest = -1; pick = 0;
            for (int i = 0; i < kFiles; ++i)
            {
                const long m = mtimeOf (fileAt (i));
                if (oldest < 0 || m < oldest) { oldest = m; pick = i; }
            }
            std::remove (fileAt (pick).c_str());
        }
        index_ = pick; path_ = fileAt (index_);
        const long sz = sizeOf (path_);
        bytes_ = sz > 0 ? sz : 0;
        sinceStat_ = 0;

        if (lk >= 0) { ::flock (lk, LOCK_UN); ::close (lk); }
    }
    std::mutex m_;
    std::string path_;
    long bytes_ = 0;
    int  sinceStat_ = 0;
    int index_ = 0;
    bool open_ = false;
};
} // namespace echojay
