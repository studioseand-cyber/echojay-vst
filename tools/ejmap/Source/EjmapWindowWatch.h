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
#include <ApplicationServices/ApplicationServices.h>
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

// THE WINDOW'S TEXT (Sean, 6 Oct): whenever the watch catches a window, its owner, title and static text are recorded before
// the tree is killed - everywhere. The title and bounds come from the window server; the static text needs the Accessibility
// API, which reads another process's UI only when ejmap is trusted in System Settings > Privacy & Security > Accessibility:
// untrusted, the record says so (ax_trusted false) and carries the title alone. Nothing is clicked.
inline void collectAxText (AXUIElementRef el, juce::StringArray& out, int depth, int& budget)
{
    if (el == nullptr || depth > 8 || budget <= 0) return;
    CFTypeRef role = nullptr; if (AXUIElementCopyAttributeValue (el, kAXRoleAttribute, &role) == kAXErrorSuccess && role != nullptr)
    {
        const auto r = juce::String::fromCFString ((CFStringRef) role); CFRelease (role);
        if (r == "AXStaticText" || r == "AXButton" || r == "AXLink" || r == "AXCheckBox" || r == "AXTextField")
        {
            for (auto attr : { kAXValueAttribute, kAXTitleAttribute, kAXDescriptionAttribute })
            {
                CFTypeRef v = nullptr;
                if (AXUIElementCopyAttributeValue (el, attr, &v) == kAXErrorSuccess && v != nullptr)
                {
                    if (CFGetTypeID (v) == CFStringGetTypeID()) { const auto t = juce::String::fromCFString ((CFStringRef) v).trim(); if (t.isNotEmpty() && ! out.contains (t)) { out.add (r == "AXButton" ? "[button] " + t : t); --budget; } }
                    CFRelease (v);
                }
            }
        }
    }
    CFTypeRef kids = nullptr;
    if (AXUIElementCopyAttributeValue (el, kAXChildrenAttribute, &kids) == kAXErrorSuccess && kids != nullptr)
    {
        if (CFGetTypeID (kids) == CFArrayGetTypeID()) for (CFIndex i = 0; i < CFArrayGetCount ((CFArrayRef) kids) && budget > 0; ++i) collectAxText ((AXUIElementRef) CFArrayGetValueAtIndex ((CFArrayRef) kids, i), out, depth + 1, budget);
        CFRelease (kids);
    }
}
inline juce::Array<juce::var> describeWindows (pid_t root, bool onScreenOnly = true)
{
    juce::Array<juce::var> out;
    const bool trusted = AXIsProcessTrusted();
    const CGWindowListOption opt = onScreenOnly ? kCGWindowListOptionOnScreenOnly : kCGWindowListOptionAll;
    CFArrayRef list = CGWindowListCopyWindowInfo (opt, kCGNullWindowID);
    if (list == nullptr) return out;
    std::map<int, juce::StringArray> axByPid;
    for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
    {
        auto w = (CFDictionaryRef) CFArrayGetValueAtIndex (list, i);
        auto pidRef = (CFNumberRef) CFDictionaryGetValue (w, kCGWindowOwnerPID); int pid = 0;
        if (pidRef == nullptr || ! CFNumberGetValue (pidRef, kCFNumberIntType, &pid)) continue;
        if (! inTreeOf ((pid_t) pid, root)) continue;
        auto* o = new juce::DynamicObject();
        auto owner = (CFStringRef) CFDictionaryGetValue (w, kCGWindowOwnerName); auto title = (CFStringRef) CFDictionaryGetValue (w, kCGWindowName);
        o->setProperty ("owner", owner != nullptr ? juce::String::fromCFString (owner) : juce::String ("?")); o->setProperty ("pid", pid);
        o->setProperty ("title", title != nullptr ? juce::String::fromCFString (title) : juce::String());
        if (auto bounds = (CFDictionaryRef) CFDictionaryGetValue (w, kCGWindowBounds)) { CGRect r; if (CGRectMakeWithDictionaryRepresentation (bounds, &r)) o->setProperty ("bounds", juce::String ((int) r.size.width) + "x" + juce::String ((int) r.size.height)); }
        o->setProperty ("ax_trusted", trusted);
        if (trusted && ! axByPid.count (pid))
        {
            juce::StringArray texts; int budget = 60;
            if (auto app = AXUIElementCreateApplication ((pid_t) pid))
            {
                CFTypeRef wins = nullptr;
                if (AXUIElementCopyAttributeValue (app, kAXWindowsAttribute, &wins) == kAXErrorSuccess && wins != nullptr)
                { if (CFGetTypeID (wins) == CFArrayGetTypeID()) for (CFIndex k = 0; k < CFArrayGetCount ((CFArrayRef) wins); ++k) collectAxText ((AXUIElementRef) CFArrayGetValueAtIndex ((CFArrayRef) wins, k), texts, 0, budget); CFRelease (wins); }
                CFRelease (app);
            }
            axByPid[pid] = texts;
        }
        juce::Array<juce::var> ts; if (axByPid.count (pid)) for (const auto& t : axByPid[pid]) ts.add (t); o->setProperty ("text", ts);
        out.add (juce::var (o));
    }
    CFRelease (list);
    return out;
}
// a log line "windows\t<json array>" back to the details
inline juce::Array<juce::var> parseDetailLines (const juce::String& line)
{
    juce::Array<juce::var> out; const auto v = juce::JSON::parse (line.fromFirstOccurrenceOf ("windows\t", false, false));
    if (const auto* a = v.getArray()) for (const auto& w : *a) out.add (w);
    return out;
}
// the licence words in a window's title or text (Sean, 6 Oct: demo / expired / authoriz; PACE's activation and licence words too): the
// first match, or empty. PURE (pins WT1-WT2).
inline juce::String licenceWords (const juce::Array<juce::var>& details)
{
    for (const auto& w : details)
    {
        juce::StringArray all { w.getProperty ("title", "").toString() }; if (const auto* ts = w.getProperty ("text", {}).getArray()) for (const auto& t : *ts) all.add (t.toString());
        for (const auto& t : all) { const auto l = t.toLowerCase(); for (const char* k : { "demo", "expired", "authoriz", "authoris", "activation", "licen", "trial" }) if (l.contains (k)) return "'" + t.substring (0, 80) + "'"; }
    }
    return {};
}

inline bool isPaceOwner (const juce::StringArray& ownersSeen)
{
    for (const auto& o : ownersSeen) if (o.containsIgnoreCase ("PACE")) return true;
    return false;
}
} // namespace ejmap::windowwatch
