/*
  EjmapWindowWatch.h - WINDOWS A PROCESS TREE SHOWS, and what counts as a new one.

  Used by the certification driver (the probe's process tree: EjmapCertDriver.h) and, since 2 Oct, by the
  scan's watchdog on ejmap's OWN tree: a plugin whose module load raises a licence / activation window
  during the scan (PACE's "Activation is required" with the iLok away, a vendor's own serial dialog) sits
  there until someone clicks or the 90 s deadline kills it - three tries each. With nobody at the
  keyboard the window IS the answer: the load is killed at once, recorded as "needs licence", never
  retried. Nothing is ever clicked.

  The comparison is PURE (two owner->count maps in, the owners that appeared or grew out) so the suite can
  pin it; the enumeration asks the window server for on-screen windows and attributes each to its owning
  process and that process's ancestry.
*/
#pragma once
#include <juce_core/juce_core.h>
#include <CoreGraphics/CoreGraphics.h>
#include <libproc.h>
#include <map>

namespace ejmap::windowwatch
{
inline pid_t parentOf (pid_t pid)
{
    proc_bsdinfo info {};
    return proc_pidinfo (pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) == (int) sizeof info ? (pid_t) info.pbi_ppid : 0;
}

inline bool inTreeOf (pid_t pid, pid_t root)
{
    for (int depth = 0; pid > 1 && depth < 16; ++depth, pid = parentOf (pid))
        if (pid == root) return true;
    return false;
}

using OwnerCounts = std::map<juce::String, int>;   // "Owner [pid N]" -> on-screen windows

// Every window owned by `root` or a descendant, counted per owning process. onScreenOnly is the production
// setting (a dialog is a window the user can SEE); false lets a self-test use an off-screen window.
inline OwnerCounts windowCountsByOwner (pid_t root, bool onScreenOnly = true)
{
    OwnerCounts counts;
    const CGWindowListOption opt = onScreenOnly ? kCGWindowListOptionOnScreenOnly : kCGWindowListOptionAll;
    CFArrayRef list = CGWindowListCopyWindowInfo (opt, kCGNullWindowID);
    if (list == nullptr) return counts;
    for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
    {
        auto w = (CFDictionaryRef) CFArrayGetValueAtIndex (list, i);
        auto pidRef = (CFNumberRef) CFDictionaryGetValue (w, kCGWindowOwnerPID);
        int pid = 0;
        if (pidRef == nullptr || ! CFNumberGetValue (pidRef, kCFNumberIntType, &pid)) continue;
        if (! inTreeOf ((pid_t) pid, root)) continue;
        auto name = (CFStringRef) CFDictionaryGetValue (w, kCGWindowOwnerName);
        ++counts[(name != nullptr ? juce::String::fromCFString (name) : juce::String ("?")) + " [pid " + juce::String (pid) + "]"];
    }
    CFRelease (list);
    return counts;
}

inline juce::StringArray owners (const OwnerCounts& c) { juce::StringArray o; for (const auto& [k, n] : c) o.add (k); return o; }

// THE RULE: an owner that was not there at the baseline, or one whose window count grew, is a new window.
// A window that closed is not news. The host's own existing windows (the status window) are the baseline.
inline juce::StringArray newWindows (const OwnerCounts& baseline, const OwnerCounts& now)
{
    juce::StringArray out;
    for (const auto& [owner, n] : now)
    {
        auto it = baseline.find (owner);
        if (it == baseline.end() || n > it->second) out.add (owner);
    }
    return out;
}

inline bool isPaceOwner (const juce::StringArray& ownersSeen)
{
    for (const auto& o : ownersSeen) if (o.containsIgnoreCase ("PACE")) return true;
    return false;
}
} // namespace ejmap::windowwatch
