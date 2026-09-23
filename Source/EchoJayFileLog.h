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
        const int n = std::fprintf (f, "%s  %s\n", stamp, line);
        std::fclose (f);
        if (n > 0) bytes_ += n;
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
        long newest = -1; int pick = 0;
        for (int i = 0; i < kFiles; ++i)
        {
            const long m = mtimeOf (fileAt (i));
            if (m < 0) { pick = i; newest = -1; break; }     // an unused slot wins: start there
            if (m > newest) { newest = m; pick = i; }
        }
        index_ = pick; path_ = fileAt (index_);
        const long sz = sizeOf (path_);
        bytes_ = sz > 0 ? sz : 0;
        if (bytes_ >= kMaxBytes) rotate();
    }
    void rotate()
    {
        index_ = (index_ + 1) % kFiles;
        path_ = fileAt (index_);
        std::remove (path_.c_str());   // the oldest slot is overwritten, never appended to
        bytes_ = 0;
    }
    std::mutex m_;
    std::string path_;
    long bytes_ = 0;
    int index_ = 0;
    bool open_ = false;
};
} // namespace echojay
