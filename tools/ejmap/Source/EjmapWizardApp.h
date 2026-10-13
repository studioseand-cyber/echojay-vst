/*
  EjmapWizardApp.h - the PURE rules of the wizard app (docs/WIZARD_SPEC.md, Kathy 11 Oct; branch feat/ejmap-wizard).

  1. THE CRASH MARKER. Before the engine loads a plugin (mapping, the quick check, measuring) it writes <work>/engine/inflight/<name>.json;
     when that plugin's step ends (any outcome, including a probe that crashed on its own) the file goes. A stop (pause) removes them all.
     So a marker that is still there when the app opens means the WIZARD itself went down with that plugin loaded: the app skips it for
     good (the never-retry list, <work>/skip.json), keeps a redacted copy of any crash report written at the time, and carries on.
  2. SKIP WHAT THE DATABASE ALREADY HAS: rule A (the same plugin, version and map), or rule B whose quick check read every written control
     back as the profile says, needs no measuring. Only rule C, or B failing its check, is measured.
  3. WHERE TO OPEN: the app keeps no session; the files say where the user was (signed in, scanned, measuring, stopped, a crash, sent,
     rewarded) and the app opens on that screen.

  Pinned in RoundTripTest.cpp (WZ-APP-*).
*/
#pragma once

#include "EjmapWizard.h"
#include <juce_core/juce_core.h>

namespace ejmap::wizapp
{
// --- 1. the crash marker --------------------------------------------------------------------------------------------------------------
inline juce::String markerName (const juce::String& product)
{
    juce::String s; for (auto c : product) s << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c) : juce::String ("_"));
    return s + "-" + juce::String::toHexString (product.hashCode()) + ".json";
}
inline void markerBegin (const juce::File& dir, const juce::String& product, const juce::String& stage)
{
    dir.createDirectory();
    auto* o = new juce::DynamicObject(); o->setProperty ("product", product); o->setProperty ("stage", stage);
    o->setProperty ("started", juce::Time::getCurrentTime().toISO8601 (true)); o->setProperty ("started_ms", (double) juce::Time::currentTimeMillis());
    const auto f = dir.getChildFile (markerName (product)); const auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    if (tmp.replaceWithText (juce::JSON::toString (juce::var (o)))) tmp.moveFileTo (f);
}
inline void markerEnd (const juce::File& dir, const juce::String& product) { dir.getChildFile (markerName (product)).deleteFile(); }
inline void markersClear (const juce::File& dir) { for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json")) f.deleteFile(); }
struct Crashed { juce::String product, stage, started; double startedMs = 0.0; juce::String report; };
inline std::vector<Crashed> leftoverMarkers (const juce::File& dir)
{
    std::vector<Crashed> out;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto v = juce::JSON::parse (f.loadFileAsString()); if (! v.isObject()) continue;
        out.push_back ({ v.getProperty ("product", "").toString(), v.getProperty ("stage", "").toString(), v.getProperty ("started", "").toString(), (double) v.getProperty ("started_ms", 0.0), {} });
    }
    std::sort (out.begin(), out.end(), [] (const Crashed& a, const Crashed& b) { return a.product < b.product; });
    return out;
}
// a crash report travels with the upload, but never the user's home folder, name or computer name
inline juce::String redact (juce::String text, const juce::String& home, const juce::String& user, const juce::String& host)
{
    if (home.isNotEmpty()) text = text.replace (home, "~");
    if (user.length() >= 3) text = text.replace (user, "<user>");
    if (host.length() >= 3) text = text.replace (host, "<host>");
    return text.replace ("/Users/", "/<users>/");   // any other account's folder too
}
inline juce::var readSkip (const juce::File& work)
{
    const auto v = juce::JSON::parse (work.getChildFile ("skip.json").loadFileAsString());
    return v.isArray() ? v : juce::var (juce::Array<juce::var>());
}
inline juce::StringArray skipList (const juce::File& work)
{
    juce::StringArray s; const auto v = readSkip (work);   // held: a pointer into a temporary var dangles
    if (const auto* a = v.getArray()) for (const auto& e : *a) s.addIfNotAlreadyThere (e.getProperty ("product", "").toString());
    return s;
}
// the time a report was WRITTEN, from its name (<process>-YYYY-MM-DD-HHMMSS.ips, local time): macOS rewrites old reports' modification
// times (seen 11 Oct: 5 Oct reports with an 11 Oct mtime), so the mtime alone could attach an old crash to a new one. 0 = no time in the name
inline double reportTimeMs (const juce::String& fileName)
{
    const auto base = fileName.upToLastOccurrenceOf (".", false, false);
    const auto parts = juce::StringArray::fromTokens (base, "-", ""); if (parts.size() < 5) return 0.0;
    const auto hms = parts[parts.size() - 1]; const int y = parts[parts.size() - 4].getIntValue(), mo = parts[parts.size() - 3].getIntValue(), d = parts[parts.size() - 2].getIntValue();
    if (y < 2000 || hms.length() != 6 || ! hms.containsOnly ("0123456789")) return 0.0;
    return (double) juce::Time (y, mo - 1, d, hms.substring (0, 2).getIntValue(), hms.substring (2, 4).getIntValue(), hms.substring (4, 6).getIntValue(), 0, true).toMilliseconds();
}
// the reports written since the plugin was loaded, by the probe or by the engine (the processes that hold a plugin)
inline juce::File newestReportSince (const juce::File& reportsDir, double sinceMs)
{
    juce::File best; double bestT = 0.0;
    for (const auto& f : reportsDir.findChildFiles (juce::File::findFiles, false, "*"))
    {
        const auto n = f.getFileName(); if (! (n.startsWith ("EchoJayProbe") || n.startsWith ("ejmap"))) continue;
        const double named = reportTimeMs (n); const double t = named > 0.0 ? named : (double) f.getLastModificationTime().toMilliseconds();
        if (t + 2000.0 < sinceMs) continue;   // the name's second resolution
        if (t > bestT) { bestT = t; best = f; }
    }
    return best;
}
// THE RECOVERY: every leftover marker -> the never-retry list (with a redacted report, or "none was written"), the marker removed
inline std::vector<Crashed> recover (const juce::File& work, const juce::File& markerDir, const juce::File& reportsDir,
                                     const juce::String& home, const juce::String& user, const juce::String& host)
{
    auto crashed = leftoverMarkers (markerDir);
    if (crashed.empty()) return crashed;
    auto list = readSkip (work); const auto rep = work.getChildFile ("crash_reports"); rep.createDirectory();
    for (auto& c : crashed)
    {
        const auto src = newestReportSince (reportsDir, c.startedMs);
        juce::String body = "EchoJay Wizard: the wizard closed while checking " + c.product + " (" + c.stage + ", started " + c.started + ").\n";
        body << (src.existsAsFile() ? "Crash report " + src.getFileName() + " (redacted):\n\n" + redact (src.loadFileAsString(), home, user, host)
                                    : juce::String ("No crash report was written (the processes ended without one, e.g. killed or power lost).\n"));
        c.report = juce::File::createLegalFileName (c.product) + ".crash.txt";
        rep.getChildFile (c.report).replaceWithText (body);
        auto* e = new juce::DynamicObject(); e->setProperty ("product", c.product); e->setProperty ("stage", c.stage); e->setProperty ("started", c.started);
        e->setProperty ("why", "the wizard closed while checking it: skipped for good, never retried"); e->setProperty ("crash_report", "crash_reports/" + c.report);
        e->setProperty ("report_found", src.existsAsFile());
        if (! skipList (work).contains (c.product)) list.getArray()->add (juce::var (e));
        markerEnd (markerDir, c.product);
    }
    work.getChildFile ("skip.json").replaceWithText (juce::JSON::toString (list));
    return crashed;
}

// --- 2. skip what the database already has --------------------------------------------------------------------------------------------
inline bool needsMeasuring (wizard::Match m, bool quickCheckPassed)
{
    if (m == wizard::Match::exact) return false;                 // rule A
    if (m == wizard::Match::sameControls) return ! quickCheckPassed;   // rule B: only a failed check measures
    return true;                                                 // rule C
}

// --- 3. where the app opens ----------------------------------------------------------------------------------------------------------
enum class Screen { welcome, scan, scanDone, crashRecovered, measuring, upload, reward };
inline juce::String screenName (Screen s)
{
    switch (s) { case Screen::welcome: return "welcome"; case Screen::scan: return "scan"; case Screen::scanDone: return "scan_done";
                 case Screen::crashRecovered: return "crash_recovered"; case Screen::measuring: return "measuring"; case Screen::upload: return "upload"; case Screen::reward: return "reward"; }
    return {};
}
struct Where { bool signedIn = false, scanned = false, measureStarted = false, measureDone = false, uploaded = false, rewarded = false; int crashed = 0; };
// THE STOP (Kathy, 13 Oct, beta item 1): the same words whether it stopped while checking or while measuring
inline juce::String stoppedNote() { return "Stopped - it carries on next time"; }
// what the window does when the engine ends: a scan that ran to its end -> its results; a measuring run that did -> the upload; a
// STOPPED one (130, or any other end) never counts as finished - the scan is not marked done, and the next launch carries on
enum class AfterEngine { scanDone, upload, stopped };
inline AfterEngine afterEngine (Screen s, int code)
{
    if (code == 0) return s == Screen::scan ? AfterEngine::scanDone : AfterEngine::upload;
    return AfterEngine::stopped;
}
inline Screen openOn (const Where& w)
{
    if (! w.signedIn) return Screen::welcome;
    if (w.rewarded) return Screen::reward;
    if (w.crashed > 0) return Screen::crashRecovered;             // a crash is said before anything carries on
    if (w.uploaded) return Screen::reward;
    if (w.measureDone) return Screen::upload;
    if (w.measureStarted) return Screen::measuring;                // closed or stopped mid-measure: carry on
    if (w.scanned) return Screen::scanDone;
    return Screen::scan;
}
} // namespace ejmap::wizapp
