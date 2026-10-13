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
// 13 Oct (beta items 3, 4, 7, 8): consent before scanning; the beta build (no sign-in, the results saved as one file, no promo); the
// server's eligibility and once-only answers. Mode is a build flag (EJ_WIZARD_BETA); every rule below takes it, so both are pinned.
enum class Mode { normal, beta };
enum class Screen { welcome, consent, scan, alreadyDone, scanDone, crashRecovered, measuring, upload, reward, noCode, betaSaved };
inline juce::String screenName (Screen s)
{
    switch (s) { case Screen::welcome: return "welcome"; case Screen::consent: return "consent"; case Screen::scan: return "scan"; case Screen::alreadyDone: return "already_done";
                 case Screen::scanDone: return "scan_done"; case Screen::crashRecovered: return "crash_recovered"; case Screen::measuring: return "measuring";
                 case Screen::upload: return "upload"; case Screen::reward: return "reward"; case Screen::noCode: return "no_code"; case Screen::betaSaved: return "beta_saved"; }
    return {};
}
struct Where { bool signedIn = false, consented = false, scanned = false, measureStarted = false, measureDone = false, uploaded = false, rewarded = false, saved = false; int crashed = 0; };
// THE CONSENT (item 3; the wording is Kathy's to approve - docs/WIZARD_SPEC.md section 2b): one paragraph, and a tick that must be set
struct Copy { juce::String title, text, primary, secondary; };
inline juce::String consentText (Mode m)
{
    // Kathy, 13 Oct (consent edits): the beta SAVES the results for the user to send; both say they become part of the shared plugin map
    return (m == Mode::beta ? juce::String ("The wizard measures how your audio plugins respond to test tones and saves the results for you to send to EchoJay: ")
                            : juce::String ("The wizard measures how your audio plugins respond to test tones and sends EchoJay the results: "))
         + "each plugin's name, maker, version, its list of controls and the measurements. It never sends audio, your files, your projects, your name, "
         + "your computer's name or folder paths. "
         + (m == Mode::beta ? "If you send it, the results become part of EchoJay's shared plugin map. "
                              "In this beta the results are saved as one file on your Desktop for you to email to us; nothing is sent by the app."
                            : "The results go to EchoJay's servers, linked to your EchoJay account, and become part of EchoJay's shared plugin map, used to help "
                              "EchoJay control plugins for everyone who uses it. "
                              "To make sure each Mac takes part once, the wizard also sends a one-way code made from this Mac's hardware ID - never the ID itself.");   // the release names the hashed ID; the beta sends nothing
}
inline juce::String consentTick() { return "I agree"; }
// the low-disk refusal (13 Oct, Kathy): the words and a Quit button - nothing has started
inline Copy diskRefusedCopy (const juce::String& words) { return { "Not enough disk space", words, "Quit", {} }; }
// the results file (item 4, beta): one zip on the Desktop
inline juce::String resultsFileName (const juce::Time& t) { return "EchoJay-Wizard-results-" + t.formatted ("%Y-%m-%d") + ".zip"; }
// AFTER THE SCAN (items 7 / 8): the server's answer decides - already taken part -> nothing runs; else the results with eligibility
struct Eligibility { bool already = false, eligible = false; int count = 0, minimum = 5; };
inline Eligibility eligibilityOf (const juce::var& v)
{ Eligibility e; e.already = (bool) v.getProperty ("already_contributed", false); e.eligible = (bool) v.getProperty ("eligible", false); e.count = (int) v.getProperty ("count", 0); e.minimum = (int) v.getProperty ("minimum", 5); return e; }
inline Screen afterScan (const Eligibility& e) { return e.already ? Screen::alreadyDone : Screen::scanDone; }
// once only needs no scan: the account and the Mac decide it, so /lookup is asked right after consent and an account or Mac that has
// already taken part loads nothing at all (13 Oct, rehearsal)
inline Screen afterConsent (const Eligibility& e) { return e.already ? Screen::alreadyDone : Screen::scan; }
inline Copy alreadyDoneCopy() { return { "You've already taken part - thank you", "This account or this Mac has already contributed its results, so there's nothing to run.", "Done", {} }; }
inline Copy scanDoneCopy (Mode m, const Eligibility& e, const juce::String& etaWords)
{
    const auto n = juce::String (e.count) + (e.count == 1 ? " plugin" : " plugins");
    if (m == Mode::beta)
        return e.count > 0 ? Copy { "You have " + n + " we don't have yet", "Thank you for helping map them. " + etaWords + " Closing the window pauses it.", "Start", {} }
                           : Copy { "Thanks - your plugins are already mapped", "There's nothing to measure right now.", "Done", {} };
    if (e.eligible) return { "You have " + n + " we don't have yet", "Help map them and get a free month. " + etaWords + " Closing the window pauses it.", "Start", {} };
    return { "Thanks - your plugins are already mapped", "So there's nothing to do right now.", "Done", e.count > 0 ? "Contribute anyway (no reward)" : juce::String() };
}
// after measuring: the upload (normal) or the saved file (beta: no upload, no promo)
inline Screen afterMeasure (Mode m) { return m == Mode::beta ? Screen::betaSaved : Screen::upload; }
// after the upload (normal): the code only when the server granted it on the ACCEPTED results; else its plain message
inline Screen afterUpload (const juce::var& receipt, const juce::var& promo) { return (bool) receipt.getProperty ("reward_eligible", false) && promo.isObject() ? Screen::reward : Screen::noCode; }
// FREE DISK (Kathy, 13 Oct, beta item 2): a wizard run writes ~5 MB per plugin (13 plugins: ~70 MB; a 500-plugin Mac ~2.5 GB). It does
// not start below kStartFloorBytes and stops cleanly (as a stop: it carries on next time) below kRunFloorBytes. Rehearsals: the file
// named by EJ_WIZARD_FAKE_FREE_FILE (one number, MB) stands in for the volume's free space.
inline constexpr juce::int64 kStartFloorBytes = (juce::int64) 3 * 1024 * 1024 * 1024, kRunFloorBytes = (juce::int64) 1 * 1024 * 1024 * 1024;
inline juce::int64 freeBytes (const juce::File& onVolume)
{
    if (const char* f = std::getenv ("EJ_WIZARD_FAKE_FREE_FILE"); f != nullptr && *f) { const juce::File ff (f); if (ff.existsAsFile()) return (juce::int64) (ff.loadFileAsString().trim().getDoubleValue() * 1024.0 * 1024.0); }
    auto d = onVolume; while (! d.exists() && d.getParentDirectory() != d) d = d.getParentDirectory();
    return d.getBytesFreeOnVolume();
}
inline juce::String gb (juce::int64 b) { return juce::String ((double) b / (1024.0 * 1024.0 * 1024.0), 1) + " GB"; }
struct DiskVerdict { bool ok = true; juce::String words; };
inline DiskVerdict diskVerdict (juce::int64 free, bool starting)
{
    const auto floor = starting ? kStartFloorBytes : kRunFloorBytes;
    if (free >= floor) return {};
    return { false, starting ? "Not enough free disk space to start: the wizard needs " + gb (kStartFloorBytes) + " free and this Mac has " + gb (free) + ". Free some space and open it again."
                             : "Stopped - your Mac is running out of disk space (" + gb (free) + " free, the wizard stops below " + gb (kRunFloorBytes) + "). Free some space and open it again: it carries on where it stopped." };
}
// THE SCAN SCREEN'S BAR (Kathy, 13 Oct: the bar at ~67% while the text said "15 of 1285"): its own progress - the Checking stage's
// n of N - never the engine's overall percent. The overall bar gives Scanning 0-10 and Checking only 10-15, so overall / 15 put the bar
// at two-thirds the moment Checking began. Scanning (listing what is installed, seconds) is 0; past Checking (or a stop after it) is 1.
inline double scanBarValue (const juce::var& progress)
{
    const int idx = (int) progress.getProperty ("stage_index", 0);
    if (idx <= 0) return 0.0;
    if (idx >= 2) return 1.0;
    const auto sc = progress.getProperty ("stage_counts", {});
    const int total = (int) sc.getProperty ("total", 0), done = (int) sc.getProperty ("done", 0);
    return total > 0 ? juce::jlimit (0.0, 1.0, (double) done / (double) total) : 0.0;
}
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
inline Screen openOn (const Where& w, Mode m = Mode::normal)
{
    if (m == Mode::normal && ! w.signedIn) return Screen::welcome;
    if (m == Mode::beta && ! w.consented && ! w.scanned && ! w.measureStarted) return Screen::welcome;   // the beta's own welcome, then consent
    if (! w.consented && ! w.scanned && ! w.measureStarted) return Screen::consent;   // 13 Oct: consent before anything is scanned
    if (w.rewarded) return Screen::reward;
    if (w.crashed > 0) return Screen::crashRecovered;             // a crash is said before anything carries on
    if (m == Mode::beta && w.saved) return Screen::betaSaved;
    if (w.uploaded) return Screen::reward;
    if (w.measureDone) return afterMeasure (m);
    if (w.measureStarted) return Screen::measuring;                // closed or stopped mid-measure: carry on
    if (w.scanned) return Screen::scanDone;
    return Screen::scan;
}
} // namespace ejmap::wizapp
