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
#include <libproc.h>
#include <signal.h>
#include <unistd.h>

#include "EjmapWizard.h"
#include "EjmapWizardContract.h"   // 21 Oct: the beta's consent is the contract's
#include <cstdio>
namespace ejmap::wizapp
{
// THE ATOMIC REPLACE (Kathy, 22 Oct - the bar flicker): juce::File::moveFileTo DELETES the target before it renames, so a reader can find
// no file at all for a moment (the 21 Oct run's bar dropped to 0 fifteen times). POSIX rename() replaces in one step: a reader sees the
// old file or the new one, never none
inline bool replaceAtomically (const juce::File& tmp, const juce::File& dst) { return std::rename (tmp.getFullPathName().toRawUTF8(), dst.getFullPathName().toRawUTF8()) == 0; }
// THE PROMO'S EXPIRY (contract v1.1 §5, Kathy 22 Oct): the server's expires_at, in words - "Use it by 31 January 2027."; "" when none
inline juce::String expiryWords (const juce::String& expiresAt)
{
    if (expiresAt.length() < 10) return {};
    const int y = expiresAt.substring (0, 4).getIntValue(), m = expiresAt.substring (5, 7).getIntValue(), d = expiresAt.substring (8, 10).getIntValue();
    static const char* months[] { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return {};
    return "Use it by " + juce::String (d) + " " + months[m - 1] + " " + juce::String (y) + ".";
}
// a progress read the app may use: a whole JSON object with a stage. An empty or partial read keeps the last good one on screen
inline bool progressUsable (const juce::var& p) { return p.isObject() && p.getProperty ("stage", "").toString().isNotEmpty(); }
}
#include <juce_core/juce_core.h>
#include <functional>

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
// THE ENGINE'S GROUP AT THE END (Kathy, 19 Oct: two warm probes hosting UAD strips were still running half an hour after the run ended).
// The engine starts in its own process group, and everything it starts - its rows, their probes, an orphan whose parent died - keeps that
// group. When the engine has ended, nothing of it should be left: every member still in the group is closed (SIGKILL), and named
inline std::vector<pid_t> processGroupMembers (pid_t pgid)
{
    std::vector<pid_t> out; if (pgid <= 1) return out;
    int n = proc_listpgrppids (pgid, nullptr, 0); if (n <= 0) return out;
    std::vector<pid_t> v ((size_t) n + 16); n = proc_listpgrppids (pgid, v.data(), (int) (v.size() * sizeof (pid_t)));
    for (int i = 0; i < n; ++i) if (v[(size_t) i] > 1 && v[(size_t) i] != getpid()) out.push_back (v[(size_t) i]);
    return out;
}
inline juce::String processName (pid_t p) { char b[PROC_PIDPATHINFO_MAXSIZE] = {}; return proc_pidpath (p, b, sizeof b) > 0 ? juce::File (juce::String::fromUTF8 (b)).getFileName() : juce::String (p); }
inline juce::StringArray closeProcessGroup (pid_t pgid)   // what it closed ("EchoJayProbe (pid 57694)"); empty = nothing was left
{
    juce::StringArray closed;
    for (pid_t p : processGroupMembers (pgid)) { const auto n = processName (p); if (kill (p, SIGKILL) == 0) closed.add (n + " (pid " + juce::String (p) + ")"); }
    return closed;
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
enum class Screen { welcome, consent, scan, alreadyDone, scanDone, crashRecovered, measuring, upload, reward, noCode, betaSaved, paused, signIn };   // paused: 13 Oct; signIn: 21 Oct (the beta's browser sign-in)
inline juce::String screenName (Screen s)
{
    switch (s) { case Screen::welcome: return "welcome"; case Screen::consent: return "consent"; case Screen::scan: return "scan"; case Screen::alreadyDone: return "already_done";
                 case Screen::scanDone: return "scan_done"; case Screen::crashRecovered: return "crash_recovered"; case Screen::measuring: return "measuring";
                 case Screen::upload: return "upload"; case Screen::reward: return "reward"; case Screen::noCode: return "no_code"; case Screen::betaSaved: return "beta_saved"; case Screen::paused: return "paused"; case Screen::signIn: return "sign_in"; }
    return {};
}
// THE WORK FOLDERS (Kathy, 13 Oct): the beta and the release never share one - "Application Support/EchoJay Wizard Beta" vs "EchoJay Wizard"
inline juce::String workFolderName (Mode m, bool test = false) { return test ? "EchoJay Wizard Test" : m == Mode::beta ? "EchoJay Wizard Beta" : "EchoJay Wizard"; }
// THE TEST BUILD (Kathy, 14 Oct: "EchoJay Wizard Test"): the RELEASE flow with the stub server inside the app, seeded with the beta
// snapshot, a banner on every screen, its own folder and icon, an editable server config and a Reset test button. Nothing leaves the Mac
inline juce::String testBanner() { return "TEST VERSION - nothing leaves this Mac"; }
// Reset test is offered wherever nothing runs (14 Oct rehearsal: after a finish the app reopens on the reward screen, never the welcome)
inline bool resetTestShownOn (Screen s) { return s != Screen::scan && s != Screen::measuring && s != Screen::upload; }
// the stub server's database seeded from the bundled snapshot: every file the stub db lacks is copied (subfolders too); nothing is replaced
inline int seedTestServer (const juce::File& bundledDb, const juce::File& stubDb)
{
    int copied = 0; if (! bundledDb.isDirectory()) return 0;
    for (const auto& f : bundledDb.findChildFiles (juce::File::findFiles, true))
    {
        const auto t = stubDb.getChildFile (f.getRelativePathFrom (bundledDb));
        if (t.existsAsFile()) continue;
        t.getParentDirectory().createDirectory(); if (f.copyFileTo (t)) ++copied;
    }
    return copied;
}
// the server's settings, a plain file Kathy edits: <work>/server_stub/config.json (written once with the defaults; a reset keeps it)
inline juce::String defaultTestConfig()
{
    return "{\n  \"qualify_minimum\": 25,\n  \"once_only\": true,\n"
           "  \"test_only_plugins\": [],\n"
           "  \"_help\": \"qualify_minimum: new plugins needed at scan time, and accepted results needed for the free month. once_only: one completed contribution per account and per Mac. "
           "test_only_plugins (test build only): plugin names - the wizard scans and measures only these (empty = the whole Mac). test_qualify_minimum (test build only, optional): replaces qualify_minimum for a small run. Edit, save, reopen the wizard.\"\n}\n";
}
// THE SMALL RUN (Kathy, 14 Oct, Test build only): "test_only_plugins" in the config - the scan and the measuring see only those (the
// engine's --only); an empty or missing list means the whole Mac
inline juce::StringArray testOnlyPlugins (const juce::File& work)
{
    juce::StringArray n; const auto c = juce::JSON::parse (work.getChildFile ("server_stub/config.json").loadFileAsString());
    if (const auto* a = c.getProperty ("test_only_plugins", {}).getArray()) for (const auto& x : *a) if (x.toString().trim().isNotEmpty()) n.addIfNotAlreadyThere (x.toString().trim());
    return n;
}
inline bool ensureTestConfig (const juce::File& work)
{
    const auto f = work.getChildFile ("server_stub/config.json"); if (f.existsAsFile()) return false;
    f.getParentDirectory().createDirectory(); return f.replaceWithText (defaultTestConfig());
}
// RESET TEST: the work folder cleared - the stub server's accounts / devices / promo ledger / runs / uploads, the app's state, the
// engine's work and the map store - keeping only the server's config.json; then the snapshot is seeded again by the next launch / open
inline int resetTest (const juce::File& work)
{
    const auto cfg = work.getChildFile ("server_stub/config.json"); const auto keep = cfg.loadFileAsString(); const bool had = cfg.existsAsFile();
    int removed = 0; for (const auto& c : work.findChildFiles (juce::File::findFilesAndDirectories, false)) { if (c.isDirectory() ? c.deleteRecursively() : c.deleteFile()) ++removed; }
    if (had) { cfg.getParentDirectory().createDirectory(); cfg.replaceWithText (keep); }
    return removed;
}
// THE DATABASE (Kathy, 13 Oct): the beta reads the read-only snapshot of Sean's cert 5 inside its own signed bundle (Contents/Resources/
// beta_db, wizard_app/beta_db - scripts/beta_db_snapshot.py); the release keeps the stubbed server's folder in its work folder
inline juce::File databaseFolder (Mode m, const juce::File& appBundle, const juce::File& work)
{ return m == Mode::beta ? appBundle.getChildFile ("Contents/Resources/beta_db") : work.getChildFile ("server_stub/db"); }
struct Where { bool signedIn = false, consented = false, scanned = false, measureStarted = false, measureDone = false, uploaded = false, rewarded = false, saved = false, serverCompleted = false; int crashed = 0; };
// THE CONSENT (item 3; the wording is Kathy's to approve - docs/WIZARD_SPEC.md section 2b): one paragraph, and a tick that must be set
struct Copy { juce::String title, text, primary, secondary; };
inline juce::String consentText (Mode m)
{
    if (m == Mode::beta) return wizcontract::consentText();   // 21 Oct (contract v1 §6): the beta sends - exactly what, word for word
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
struct Eligibility { bool already = false, eligible = false; int count = 0, minimum = 5, known = 0; };
inline Eligibility eligibilityOf (const juce::var& v)
{ Eligibility e; e.already = (bool) v.getProperty ("already_contributed", false); e.eligible = (bool) v.getProperty ("eligible", false); e.count = (int) v.getProperty ("count", 0); e.minimum = (int) v.getProperty ("minimum", 5); e.known = (int) v.getProperty ("known", 0); return e; }
// THE CHECK BEFORE MEASURING (Kathy, 21 Oct): "N already known, M to measure"
inline juce::String knownWords (int known, int toMeasure) { return juce::String (known) + " already known, " + juce::String (toMeasure) + " to measure"; }
// THE LIVE UPLOAD's line (21 Oct): what was sent, what waits and why - never a token, never a number the server did not confirm
// THE SENDING SCREEN SAYS WHY IT WAITS (Kathy, 26 Oct, staging run 1: "0 results sent, 10 waiting" for 15 min with no reason). The body
// text under the title: the server's words and when the next try is, or why sending stopped and that the results are kept; empty when
// nothing needs saying (the screen keeps its usual line)
// THE RUN'S OWN PLUGIN LIST (26 Oct, the v2 rehearsal: a run started with --only and reopened mid-measuring without it scanned the whole
// Mac and loaded 13 VST3 twins, 8 of them ones the guard refuses): --only belongs to the run. It is saved at consent; a reopen without
// --only takes the run's list; a --only given on the command line wins
inline juce::StringArray runOnlyList (const juce::StringArray& given, const juce::var& state)
{
    if (! given.isEmpty()) return given;
    juce::StringArray o; if (const auto* a = state.getProperty ("only", {}).getArray()) for (const auto& x : *a) if (x.toString().trim().isNotEmpty()) o.add (x.toString());
    return o;
}
inline constexpr const char* kUploadText = "Just the measurements - no audio, no plugin files, nothing personal.";
struct UploadView { int waiting = 0; bool offline = false, signIn = false, stopped = false, noRun = false, runCompleted = false; juce::String words; int retryInS = -1; };
inline juce::String uploadDetail (const UploadView& v)
{
    if (v.runCompleted)   // 27 Oct (B: 409 run_completed): EchoJay's words, and never a promise to send them - a new run is the way on
        return (v.words.isNotEmpty() ? v.words.trim() : juce::String ("This run is finished. Start a new run in the EchoJay Wizard to check again.")) + " What was measured is kept on this Mac.";
    if (v.waiting <= 0 || v.signIn) return {};
    if (v.noRun) return "Nothing can be sent: the wizard can't find this run's details on this Mac. Your results are kept here - quit and open the wizard again, and tell EchoJay if this comes back.";
    if (v.stopped) return "Not sent: " + (v.words.isNotEmpty() ? v.words : juce::String ("EchoJay didn't accept the request.")) + " Your results are kept on this Mac and will be sent the next time you open the wizard.";
    juce::String s = v.offline ? juce::String ("You're offline.") : v.words.trim();
    if (s.isEmpty()) return {};
    if (v.retryInS > 0) s << " Trying again in " << v.retryInS << " s."; else if (v.retryInS == 0) s << " Trying again now.";
    return s;
}
inline juce::String uploadLine (int sent, int waiting, int rejected, bool offline, bool signIn)
{
    juce::String s = juce::String (sent) + (sent == 1 ? " result sent" : " results sent");
    if (waiting > 0) s << ", " << waiting << " waiting" << (signIn ? " - please sign in again" : offline ? " - you're offline, they'll go when you're back" : "");
    if (rejected > 0) s << ", " << rejected << " not accepted";
    return s;
}
// ONE START CARRIES THE WHOLE RUN (Kathy, 15 Oct: the overnight run sat on "Ready to measure - 504 plugins - Start" all night): when
// there is something to measure (the beta: anything new; the release: eligible) the scan goes straight into measuring - no second
// button. "Ready to measure" is now the Measuring screen's line (measuringText). Not eligible / nothing new still stops on its screen:
// that is a choice (Done, or Contribute anyway), not a second start.
inline bool measureStraightAfterScan (Mode m, const Eligibility& e) { return ! e.already && e.count > 0 && (m == Mode::beta || e.eligible); }
inline Screen afterScan (const Eligibility& e, Mode m = Mode::normal) { return e.already ? Screen::alreadyDone : measureStraightAfterScan (m, e) ? Screen::measuring : Screen::scanDone; }
// once only needs no scan: the account and the Mac decide it, so /lookup is asked right after consent and an account or Mac that has
// already taken part loads nothing at all (13 Oct, rehearsal)
inline Screen afterConsent (const Eligibility& e) { return e.already ? Screen::alreadyDone : Screen::scan; }
inline Copy alreadyDoneCopy() { return { "You've already taken part - thank you", "This account or this Mac has already contributed its results, so there's nothing to run.", "Done", {} }; }
// NO TIMES ON SCREEN (Kathy, 14 Oct): hours scare people off and an estimate can be wrong - the scan-done words say what happens, never how
// long. The estimate stays internal (wizard_state.json to_measure_eta_s, progress.json eta_s, the logs)
inline juce::String scanDoneBody (Mode m, int count)
{
    const auto n = juce::String (count) + (count == 1 ? " plugin" : " plugins");
    const juce::String body = "We found " + n + " we haven't mapped yet. Measuring them runs by itself in the background - it's best left running overnight, and you can pause any time and carry on later.";
    return m == Mode::normal ? body + " Finish them all to get your free month." : body;
}
inline Copy scanDoneCopy (Mode m, const Eligibility& e)
{
    const auto n = juce::String (e.count) + (e.count == 1 ? " plugin" : " plugins");
    if (m == Mode::beta)   // 21 Oct: the catalogue check's own count - "N already known, M to measure"
        return e.count > 0 ? Copy { "Ready to measure", knownWords (e.known, e.count) + ". " + scanDoneBody (m, e.count), "Start", {} }
                           : Copy { "Thanks - EchoJay already knows your plugins", knownWords (e.known, 0) + ". There's nothing to measure right now.", "Done", {} };
    if (e.eligible) return { "Ready to measure", scanDoneBody (m, e.count), "Start", {} };
    // below the qualify minimum (Kathy, 13 Oct): no reward; when there is something new, say how much and that they may still contribute
    if (e.count > 0) return { "Thanks - your plugins are already mapped", "You have " + n + " we don't have yet - the free month needs " + juce::String (e.minimum) + " or more. You can still contribute them.", "Done", "Contribute anyway (no reward)" };
    return { "Thanks - your plugins are already mapped", "So there's nothing to do right now.", "Done", {} };
}
// the Measuring screen's words and its bar's right side: n of N measurements (a demo check is not one) - never time left
// the scan screen's warning (Kathy, 14 Oct)
// THE SCAN SCREEN'S COUNT (Kathy, 14 Oct: "75 of 1376"): every plugin counts except one out of scope by type / name that loads nothing (the
// 91 here) - so this Mac shows n of 1285
inline bool scanCounts (const juce::String& outOfScopeByType, bool loadsDuringScan) { return outOfScopeByType.isEmpty() || loadsDuringScan; }
// an engine whose parent is gone (launchd, pid 1, adopted it): the app quit without stopping it
inline bool orphaned (int parentPid) { return parentPid == 1; }
inline juce::String scanWarning() { return "Some plugins may briefly show a licence or sign-in window. That's normal - please don't click it; the wizard closes it and moves on."; }
// 18 Oct (Kathy: after Carry on the screen said "Measuring 472 plugins" with 33 left): a RESUMED run (measuring had started, and the last
// run left engine/left.json) says how many are left; a first run says how many it measures
inline juce::String measuringResumedText (int left)
{
    return "Carrying on: " + juce::String (left) + (left == 1 ? " plugin" : " plugins") + " left to measure. Leave it running - the Mac stays awake. You can pause and carry on any time.";
}
inline int leftCount (bool measureStarted, const juce::var& leftJson) { return measureStarted && leftJson.isArray() ? leftJson.size() : 0; }
inline juce::String measuringText (int toMeasure = 0)
{   // 15 Oct: the scan-done screen's "Ready to measure" words, as the Measuring screen's line (never a time)
    const juce::String keep = "Leave it running - the Mac stays awake. You can pause and carry on any time.";
    if (toMeasure <= 0) return keep;
    return "Measuring " + juce::String (toMeasure) + (toMeasure == 1 ? " plugin" : " plugins") + " we haven't mapped yet. " + keep;   // 15 Oct (Kathy): it is already measuring
}
inline juce::String measuringCount (const juce::var& progress)
{
    const auto mc = progress.getProperty ("measurements", {});
    if (! mc.isObject() || (int) mc.getProperty ("total", 0) <= 0) return {};
    return juce::String ((int) mc.getProperty ("done", 0)) + " of " + juce::String ((int) mc.getProperty ("total", 0));
}
// THE MEASURING BAR AS A PERCENTAGE (Kathy, 15 Oct: "23%", not "12 of 517"): the engine's weighted bar (each plugin weighs Sean's real
// seconds; a running plugin earns its time so far) over its Measuring band 15..97, in whole numbers, never below what was shown
inline int measuringPercent (const juce::var& progress, int lastShown)
{
    const double pct = (double) progress.getProperty ("percent", 0.0);
    const int now = juce::jlimit (0, 100, (int) std::floor ((pct - 15.0) / 82.0 * 100.0 + 1.0e-9));
    return juce::jmax (lastShown, now);
}
inline juce::String percentWords (int pct) { return juce::String (pct) + "%"; }
// FEELS FROZEN (Kathy, 15 Oct: a plugin can take 10-20 min and nothing on screen changes) - the measuring screen moves every few seconds:
// (1) the % with one decimal, from the engine's step-weighted bar (a running plugin earns its probe calls), never backwards
inline double measuringPercentTenths (const juce::var& progress, double lastShown)
{
    const double pct = (double) progress.getProperty ("percent", 0.0);
    const double now = juce::jlimit (0.0, 100.0, std::floor ((pct - 15.0) / 82.0 * 1000.0 + 1.0e-9) / 10.0);
    return juce::jmax (lastShown, now);
}
inline juce::String percentWordsTenths (double pct) { return juce::String (pct, 1) + "%"; }
// 18 Oct (BAR % WRONG): the measuring %, from the engine's per-plugin share (skipped plugins off the total) - it agrees with "19 of 389
// done"; an older engine without it: the band formula above. Never below what was shown
inline double measuringShareTenths (const juce::var& progress, double lastShown)
{
    const auto mp = progress.getProperty ("measure_progress", {});
    if (! mp.isObject()) return measuringPercentTenths (progress, lastShown);
    return juce::jmax (lastShown, std::floor (juce::jlimit (0.0, 1.0, (double) mp.getProperty ("share", 0.0)) * 1000.0 + 1.0e-9) / 10.0);
}
// (2) each running job (up to 4): its name and its steps as a share (-1: a demo / sound check - no steps, "checking")
struct JobLine { juce::String name; double fraction = -1.0; };
inline std::vector<JobLine> jobLines (const juce::var& progress)
{
    std::vector<JobLine> out;
    if (const auto* a = progress.getProperty ("jobs_now", {}).getArray())
        for (const auto& j : *a) if (out.size() < 4) out.push_back ({ j.getProperty ("product", "").toString(), (double) j.getProperty ("fraction", -1.0) });
    return out;
}
// (3) the live wave by the title follows the engine's heartbeat (a step, a start or a finish): no heartbeat for 2 min -> "Still working on a
// slow plugin...", for 15 min -> "Something may be stuck - Pause and carry on to restart it"
enum class Live { live, slow, stuck };
inline constexpr double kSlowS = 120.0, kStuckS = 900.0;
inline Live liveState (juce::int64 nowMs, juce::int64 heartbeatMs)
{
    if (heartbeatMs <= 0) return Live::live;   // no heartbeat read yet (an older engine, the first second)
    const double s = (double) (nowMs - heartbeatMs) / 1000.0;
    return s >= kStuckS ? Live::stuck : s >= kSlowS ? Live::slow : Live::live;
}
inline juce::String liveWords (Live l) { return l == Live::stuck ? juce::String ("Something may be stuck - Pause and carry on to restart it") : l == Live::slow ? juce::String ("Still working on a slow plugin...") : juce::String(); }
// (4) the plugins done, ticking up
inline juce::String betweenPluginsWords() { return "Starting the next plugin..."; }
inline juce::String pluginsDoneWords (int n) { return juce::String (n) + (n == 1 ? " plugin done" : " plugins done"); }
// the live wave: 5 soft bars, heights from a slow sine (t in ms) - flat when not live
inline float waveHeight (int bar, double ms, bool live) { return live ? (float) (5.0 + 4.0 * std::sin (ms / 1000.0 * 2.0 * juce::MathConstants<double>::pi * 0.7 + bar * 0.9)) : 2.0f; }
// GETTING READY HAS ITS OWN MOVING BAR (Kathy, 15 Oct: "Getting ready" with an empty bar for a while): before a pass measures, the
// measuring run re-checks the Mac (the engine's Scanning / Checking - its stage counts) and then demo / sound-checks that pass's plugins
// (progress.json demo_checks) - each shows its own n of N; the % returns when measuring does
struct ReadyBar { bool on = false; double value = 0.0; juce::String left, right; };
inline ReadyBar readyBar (const juce::var& p, const juce::String& current)
{
    ReadyBar b; const auto stage = p.getProperty ("stage", "").toString();
    const auto lk = p.getProperty ("looking", {}); const int lt = (int) lk.getProperty ("total", 0), ld = (int) lk.getProperty ("done", 0);
    if ((int) p.getProperty ("stage_index", 0) <= 0 && lt > 0 && ld < lt)   // 18 Oct: the measuring run's own look at the VST3 bundles
    { b.on = true; b.value = (double) ld / lt; b.left = "Getting ready - looking for plugins"; b.right = juce::String (ld) + " of " + juce::String (lt); return b; }
    if (stage == "Scanning" || stage == "Checking")
    {
        const auto sc = p.getProperty ("stage_counts", {}); const int d = (int) sc.getProperty ("done", 0), t = (int) sc.getProperty ("total", 0);
        b.on = true; b.value = t > 0 ? (double) d / t : 0.0; b.left = "Getting ready - re-checking your plugins"; b.right = t > 0 ? juce::String (d) + " of " + juce::String (t) : juce::String(); return b;
    }
    const auto dc = p.getProperty ("demo_checks", {});
    if (stage == "Measuring" && (bool) dc.getProperty ("running", false))
    {
        const int d = (int) dc.getProperty ("done", 0), t = (int) dc.getProperty ("total", 0);
        b.on = true; b.value = t > 0 ? (double) d / t : 0.0; b.left = current.isNotEmpty() ? "Getting ready - checking " + current : juce::String ("Getting ready - checking plugins");
        b.right = juce::String (d) + " of " + juce::String (t) + " checked"; return b;
    }
    return b;
}
// THE PAUSE SAYS THE BAR'S % (Kathy, 15 Oct: the pause said "25 measured so far" while the checkpoint said 107 of 509 with a result - two
// different counts): "Paused at 43%" on the screen, "43% done." in the close question - the same number as the bar, never a count
// SHARING THE MAC (Kathy, 15 Oct): a DAW open -> the wizard pauses by itself and carries on by itself when it closes; the DAW is never touched.
// (17 Oct: identified by bundle id + main executable, below - the 15 Oct rule took any process inside the .app, helpers too)
// THE DAW CHECK BY BUNDLE ID (Kathy, 17 Oct, "DAW PAUSE": the wizard paused for a Logic with no window). A process is a DAW only when it
// is the MAIN executable of an app whose bundle id is a DAW's - a helper inside the app (an XPC service, a plug-in host, an agent) has
// another path and is never a DAW. The pid, path and bundle id go in the log, so a pause always says what it saw
inline juce::String dawFromBundleId (const juce::String& bundleId)
{
    struct D { const char* id; const char* name; };
    static const D k[] { { "com.apple.logic10", "Logic" }, { "com.apple.logic.pro", "Logic" }, { "com.avid.ProTools", "Pro Tools" }, { "com.ableton.live", "Ableton Live" },
                         { "com.steinberg.cubase", "Cubase" }, { "com.steinberg.nuendo", "Nuendo" }, { "com.presonus.studioone", "Studio One" }, { "com.cockos.reaper", "Reaper" },
                         { "com.image-line.flstudio", "FL Studio" }, { "com.bitwig.BitwigStudio", "Bitwig" }, { "com.motu.DigitalPerformer", "Digital Performer" },
                         { "com.uaudio.luna", "LUNA" } };
    const auto b = bundleId.trim();
    for (const auto& d : k)
    {
        const juce::String p (d.id);   // the id itself, or a versioned form of it ("com.presonus.studioone6", "com.steinberg.cubase13") - never a sub-id
        if (b.equalsIgnoreCase (p) || (b.startsWithIgnoreCase (p) && b.length() > p.length() && b.substring (p.length()).containsOnly ("0123456789"))) return d.name;
    }
    return {};
}
// the outermost .app a process runs from ("/Applications/Logic Pro.app/Contents/XPCServices/x.xpc/..." -> "/Applications/Logic Pro.app")
inline juce::String outermostApp (const juce::String& execPath)
{
    const int i = execPath.indexOfIgnoreCase (".app/"); return i < 0 ? juce::String() : execPath.substring (0, i + 4);
}
// is this process the app's own main executable (<app>/Contents/MacOS/<CFBundleExecutable>)?
inline bool isMainExecutable (const juce::String& execPath, const juce::String& appPath, const juce::String& bundleExecutable)
{
    return appPath.isNotEmpty() && bundleExecutable.isNotEmpty() && execPath == appPath + "/Contents/MacOS/" + bundleExecutable;
}
struct DawHit { juce::String name, bundleId, path; int pid = 0; bool windows = false; bool any() const { return name.isNotEmpty(); } };
inline juce::String dawLogLine (const DawHit& h)
{
    return h.name + " (pid " + juce::String (h.pid) + ", " + h.bundleId + ", " + h.path + ", " + (h.windows ? "a window open" : "no window open") + ")";
}
inline juce::String dawPausedText (const juce::String& daw, bool windows = true)
{
    if (! windows) return daw + " is still running in the background (even with no window open) - quit it with Cmd-Q or from the Dock and the wizard carries on by itself.";
    return "Paused while " + daw + " is open - it carries on by itself when you close it.";
}
inline juce::String carryOnAnywayWarning (const juce::String& daw) { return "Measuring while " + daw + " runs can slow both down and change the results - only if you're not using it."; }
inline constexpr const char* kCarryOnAnyway = "Carry on anyway";
inline constexpr double kDawQuietS = 10.0;   // closed for this long before carrying on (a DAW restarting is not a close)
inline juce::String pausedTitle (int pct) { return "Paused at " + percentWords (pct); }
inline juce::String percentDoneWords (int pct) { return percentWords (pct) + " done"; }
// the "it's working" shimmer (Kathy, 15 Oct): only while measuring runs - never paused, never while pausing or finishing
inline bool shimmerOn (Screen s, bool pausing, bool finishing) { return s == Screen::measuring && ! pausing && ! finishing; }
// where the soft band is at time t (ms): 0..1 across the bar, one sweep every 2.4 s, then a 0.8 s rest
inline double shimmerPhase (double ms) { const double period = 3200.0, sweep = 2400.0; const double k = std::fmod (ms, period); return k < sweep ? k / sweep : -1.0; }
inline juce::String pausedText (Mode m, bool keptOffline)
{
    if (m == Mode::beta) return keptOffline ? "It carries on where it stopped - open the wizard any time. You're offline: the results so far are kept on this Mac and sent when you're back online."
                                            : "It carries on where it stopped - open the wizard any time. Every result so far has been sent to EchoJay.";
    return keptOffline ? "It carries on where it stopped - open the wizard any time. You're offline: what's measured so far is saved and will be sent when you're back online. The free month comes when the full run is done."
                       : "It carries on where it stopped - open the wizard any time. What's measured so far has been sent; the free month comes when the full run is done.";
}
// a user-facing string that tells a time (the 14 Oct rule's check): a time unit (hour, minute, min, sec, second, day), or "about" / "left"
// next to a number. "best left running overnight" tells none
inline bool tellsATime (const juce::String& s)
{
    auto w = juce::StringArray::fromTokens (s.toLowerCase(), " -.,;:()/", ""); w.removeEmptyStrings();
    static const juce::StringArray units { "hour", "hours", "h", "minute", "minutes", "min", "mins", "sec", "secs", "second", "seconds", "day", "days" };
    auto isNum = [] (const juce::String& x) { return x.isNotEmpty() && x.containsOnly ("0123456789"); };
    for (int i = 0; i < w.size(); ++i)
    {
        if (units.contains (w[i])) return true;
        if (w[i] == "about" && i + 1 < w.size() && isNum (w[i + 1])) return true;
        if (w[i] == "left" && i > 0 && (isNum (w[i - 1]) || units.contains (w[i - 1]))) return true;
    }
    return false;
}
// after measuring: the upload (normal) or the saved file (beta: no upload, no promo)
inline Screen afterMeasure (Mode m) { (void) m; return Screen::upload; }   // 21 Oct: the beta sends too (its last results, then the run's end)
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
// NEVER LOOKS FROZEN (Kathy, 18 Oct): ONE progress system on every stage - a step line on top, a bar per step that moves per sub-step and
// never goes back inside its step, its % with one decimal, a "now" line (plugin + what it is doing), a ticking counter, the live wave and
// the 2 min / 15 min lines. The scan screen is steps 1 and 2; the measuring screen (its getting ready too) is step 3
inline juce::String stepLine (bool measuringScreen, int stageIndex)
{
    if (measuringScreen) return "Step 3 of 3 - Measuring";
    return stageIndex <= 0 ? juce::String ("Step 1 of 3 - Looking for plugins") : juce::String ("Step 2 of 3 - Checking your plugins");
}
// the scan screen's bar, PER STEP: step 1 the VST3 bundles looked at (n of N), step 2 the engine's own unrounded share of the checking
// (every control read, every map, every signature read - stage_share; an older engine: its n of N)
inline double scanBarValue (const juce::var& progress)
{
    const int idx = (int) progress.getProperty ("stage_index", 0);
    if (idx >= 2) return 1.0;
    if (idx <= 0)
    {   const auto lk = progress.getProperty ("looking", {}); const int lt = (int) lk.getProperty ("total", 0), ld = (int) lk.getProperty ("done", 0);
        return lt > 0 ? juce::jlimit (0.0, 1.0, (double) ld / (double) lt) : 0.0; }
    // 28 Oct (the STAGING 4 run: the bar sat at 98 % while "979 plugins checked" of 1,204 kept rising - the engine's stage_share weights
    // the step by guessed seconds per kind and leaves out the short sound check). The bar is the real work: plugins checked of the total
    // to check, each plugin being checked now counted by its own fraction (jobs_now), never past the whole; the share only without counts
    const auto sc = progress.getProperty ("stage_counts", {});
    const int total = (int) sc.getProperty ("total", 0), done = (int) sc.getProperty ("done", 0);
    if (total > 0)
    {   double partial = 0.0;
        if (const auto* jobs = progress.getProperty ("jobs_now", {}).getArray())
            for (const auto& j : *jobs) partial += juce::jlimit (0.0, 1.0, (double) j.getProperty ("fraction", 0.0));
        return juce::jlimit (0.0, 1.0, ((double) done + partial) / (double) total); }
    if (progress.hasProperty ("stage_share")) return juce::jlimit (0.0, 1.0, (double) progress.getProperty ("stage_share", 0.0));
    return 0.0;
}
// a step's bar never goes back: the larger of what was shown in THIS step and the new value (a new step starts from its own 0)
struct StepBar { int step = 0; double shown = 0.0; double next (int s, double v) { if (s != step) { step = s; shown = 0.0; } shown = juce::jmax (shown, juce::jlimit (0.0, 1.0, v)); return shown; } };
inline juce::String barPercentWords (double fraction) { return percentWordsTenths (std::floor (juce::jlimit (0.0, 1.0, fraction) * 1000.0 + 1.0e-9) / 10.0); }
// the words left of the bar: step 1 "Looking for plugins - 312 of 859" (Kathy, 18 Oct, LOOKING FOR PLUGINS), step 2 "Checking your plugins"
inline juce::String scanLeftWords (const juce::var& progress, const juce::String&)
{
    const auto lk = progress.getProperty ("looking", {});
    const int lt = (int) lk.getProperty ("total", 0), ld = (int) lk.getProperty ("done", 0);
    if ((int) progress.getProperty ("stage_index", 0) <= 0)
        return lt > 0 && ld < lt ? "Looking for plugins - " + juce::String (ld) + " of " + juce::String (lt) : juce::String ("Looking for plugins");
    return "Checking your plugins";
}
// the now line: the engine's own (plugin + what it is doing, "LinEQ Lowband - checking controls 12 of 40")
inline juce::String nowWords (const juce::var& progress) { return progress.getProperty ("now", {}).getProperty ("line", "").toString(); }
// THE PACKING STAGE (Kathy, 23 Oct; the 22 Oct log's last 4 s: "0 of 1 done" under a full bar): the engine counts its packing as a one-job
// stage of its own - the screen never shows that count; while packing (and once done) the counter says what is happening
inline juce::String packingWords() { return "Packing results..."; }
inline bool packingStage (const juce::String& stage) { return stage == "Packing" || stage == "Done"; }
inline juce::String counterWords (const juce::var& progress, bool measuringScreen);
inline juce::String measuringCounterWords (const juce::String& stage, const juce::var& progress)
{ return packingStage (stage) ? packingWords() : stage == "Measuring" ? counterWords (progress, true) : juce::String(); }
// the ticking counter, per stage
inline juce::String counterWords (const juce::var& progress, bool measuringScreen)
{
    const int idx = (int) progress.getProperty ("stage_index", 0); const auto sc = progress.getProperty ("stage_counts", {}); const int done = (int) sc.getProperty ("done", 0);
    auto n = [] (int k, const char* one, const char* many) { return juce::String (k) + " " + (k == 1 ? one : many); };
    if (! measuringScreen)
    {
        if (idx <= 0) { const auto lk = progress.getProperty ("looking", {}); return (int) lk.getProperty ("total", 0) > 0 ? n ((int) lk.getProperty ("done", 0), "plugin looked at", "plugins looked at") : juce::String(); }
        return n (done, "plugin checked", "plugins checked");
    }
    if (idx <= 1) return n (done, "plugin re-checked", "plugins re-checked");   // the measuring run's getting ready: the Mac re-checked
    const auto dc = progress.getProperty ("demo_checks", {});
    if ((bool) dc.getProperty ("running", false)) return n ((int) dc.getProperty ("done", 0), "plugin sound-checked", "plugins sound-checked");
    if (const auto mp = progress.getProperty ("measure_progress", {}); mp.isObject())   // 18 Oct (BAR % WRONG): "19 of 389 done - 83 skipped"
    {   const int sk = (int) mp.getProperty ("skipped", 0);
        return juce::String ((int) mp.getProperty ("done", 0)) + " of " + juce::String ((int) mp.getProperty ("total", 0)) + " done" + (sk > 0 ? " - " + juce::String (sk) + " skipped" : juce::String()); }
    return progress.hasProperty ("plugins_done") ? pluginsDoneWords ((int) progress.getProperty ("plugins_done", 0)) : juce::String();
}
// THE PROGRESS LOG (Kathy, 20 Oct): what the tester SAW, written to app.log every kProgressLogEveryS and at every stage change, so a log proves
// the bar never froze and never disagreed with its counter: the screen, the engine stage, the step line, the bar's % (what is drawn), the %
// text shown beside it, the counter as shown and as its own %, the engine's share, and the skipped count
inline constexpr double kProgressLogEveryS = 30.0;
struct ProgressLogClock { juce::String lastKey; double lastMs = -1.0; };
inline bool progressLogDue (ProgressLogClock& c, const juce::String& stageKey, double nowMs)
{
    if (stageKey == c.lastKey && c.lastMs >= 0.0 && nowMs - c.lastMs < kProgressLogEveryS * 1000.0) return false;
    c.lastKey = stageKey; c.lastMs = nowMs; return true;
}
// THE BATTERY (Kathy, 23 Oct): the 22 Oct run ran on battery and macOS forced a Low Power Sleep at 3% - a keep-awake cannot stop that.
// On battery: a notice on every screen ("Please plug in your Mac - the run takes several hours"), the run goes on. Below
// kBatteryPausePercent ON BATTERY while scanning or measuring: the same clean pause as the button, saying why; plugged in again, it
// carries on by itself (a pause the battery caused, never the user's). Every change goes to the log, and each progress line says the power
struct Power { bool known = false, onBattery = false; int percent = -1; };
// 28 Oct (the STAGING 4 run: "Please plug in your Mac" on AC at 100 %, charged / not charging): macOS can name the battery as the providing
// source while the adapter is attached and the battery full. On battery only when the battery provides AND no power source reports AC
inline bool onBatteryFrom (bool providingIsBattery, bool anySourceOnAC) { return providingIsBattery && ! anySourceOnAC; }   // known: macOS answered; a Mac with no battery is never on it
inline constexpr int kBatteryPausePercent = 20;
inline juce::String plugInNotice() { return "Please plug in your Mac - the run takes several hours"; }
inline juce::String powerWords (const Power& p) { return ! p.known ? juce::String() : p.onBattery ? "battery " + juce::String (p.percent) + "%" : juce::String ("AC"); }
struct BatteryStep { bool notice = false, pause = false, resume = false; juce::String why; };
// THE RUN IS OVER (Kathy, staging run 1, 26 Oct: the notice still asked to plug in on the free-month screen): once measuring is done -
// sending, the end screens - sleep can spoil nothing (an upload waits and carries on), so the notice is not shown there
inline bool runIsOver (Screen s) { return s == Screen::upload || s == Screen::reward || s == Screen::noCode || s == Screen::betaSaved || s == Screen::alreadyDone; }
inline BatteryStep batteryStep (const Power& p, bool running, bool pausedForBattery, bool runOver = false)
{
    BatteryStep s; if (! p.known) return s;
    s.notice = p.onBattery && ! runOver;
    if (running && ! pausedForBattery && p.onBattery && p.percent >= 0 && p.percent < kBatteryPausePercent)
    { s.pause = true; s.why = "the battery is at " + juce::String (p.percent) + "%"; }
    if (pausedForBattery && ! p.onBattery) s.resume = true;
    return s;
}
// CARRY ON ON A LOW BATTERY (Kathy, 24 Oct; the 23 Oct run: Carry on at 19% paused again at 18%, then macOS slept at 1%): below
// kBatteryPausePercent on battery, Carry on does not carry on - it says why and the run stays paused until the Mac is plugged in, then
// carries on by itself (the user asked to carry on)
// THE HIDDEN SIGN OUT (Kathy, 26 Oct; testing only, for switching test accounts without Terminal): Option-click on the title, on a screen
// where no run is going (before it starts, or after it ends). Never while scanning, measuring, paused or sending, nor mid sign-in (the
// browser's approval would land after it)
inline bool signOutAllowed (Screen s)
{ return s == Screen::welcome || s == Screen::consent || s == Screen::reward || s == Screen::noCode || s == Screen::betaSaved || s == Screen::alreadyDone; }
// A FINISHED RUN NEVER RUNS AGAIN (Kathy, 27 Oct; STAGING 3 run 3: a Sign out on run 1's end screen, then a new sign-in and Start, began a
// second run under run 1's finished run id). At Start: a folder whose run is finished - measured, saved, sent, ended, or signed out of since
// it began - gets a FRESH run id and fresh run state; the old run's files move to previous_runs/<its run id>/, kept for the record
inline bool runNeedsFreshStart (const juce::var& st, bool endFileExists)
{
    if (st.getProperty ("run_id", "").toString().isEmpty()) return false;   // no run yet: nothing to start afresh
    return endFileExists || st.hasProperty ("signed_out_at") || st.hasProperty ("server_completed") || (bool) st.getProperty ("uploaded", false) || (bool) st.getProperty ("saved", false)
        || (bool) st.getProperty ("measure_done", false);
}
inline const juce::StringArray& runStateNames()   // what belongs to one run (app.log stays: one log for the folder, it says where each run went)
{ static const juce::StringArray k { "app_state.json", "reward.json", "receipt.json", "eligibility.json", "catalogue_request.json", "catalogue_answers.json",
                                     "engine_output.txt", "engine", "outbox", "store" }; return k; }
inline constexpr const char* kPreviousRuns = "previous_runs";
// THE CATALOGUE CHECK'S WORDS (27 Oct): while the server is asked, the step says so (the engine's last plugin line is gone); a check
// with no answer says "EchoJay is taking a while to answer" and when it tries again - "offline" only when the network failed at once
inline constexpr const char* kCheckingStep = "Step 2 of 3 - Checking your plugins with EchoJay";
inline juce::String catalogueWaitLine (int answered, int chunks)
{ return chunks <= 1 ? juce::String ("Asking EchoJay which of your plugins it already knows")
                     : "Asking EchoJay which of your plugins it already knows - " + juce::String (answered) + " of " + juce::String (chunks) + " parts answered"; }
inline juce::String catalogueRetryText (const juce::String& words, int inS)
{ return words.trim() + (inS > 0 ? " Trying again in " + juce::String (inS) + " s." : juce::String (" Trying again now.")); }
inline constexpr const char* kSignedOutText = "Signed out of EchoJay on this Mac. The next run asks you to sign in.";
inline constexpr const char* kPlugInFirst = "Plug in first - the Mac will sleep at low battery";
inline bool carryOnBlocked (const Power& p) { return p.known && p.onBattery && p.percent >= 0 && p.percent < kBatteryPausePercent; }
inline juce::String batteryPausedText (int percent)
{ return "The battery is at " + juce::String (percent) + "%, so the wizard paused safely - nothing is lost. Plug in your Mac and it carries on by itself."; }
inline juce::String progressLogLine (const juce::String& screen, const juce::var& progress, const juce::String& stepText, double barValue, const juce::String& barRight, const juce::String& counter,
                                     const juce::String& power = {})
{
    const auto mp = progress.getProperty ("measure_progress", {});
    juce::String l = "progress | screen " + screen + " | stage " + progress.getProperty ("stage", "").toString() + " | step " + (stepText.isEmpty() ? juce::String ("-") : stepText)
                   + " | bar " + barPercentWords (barValue) + " | shown '" + barRight + "' | counter '" + counter + "'";
    if (mp.isObject())
    {   const int done = (int) mp.getProperty ("done", 0), total = (int) mp.getProperty ("total", 0);
        if (total > 0) l << " (" << juce::String (100.0 * done / total, 1) << "%)";
        l << " | share " << juce::String (juce::jlimit (0.0, 1.0, (double) mp.getProperty ("share", 0.0)) * 100.0, 1) << "% | skipped " << (int) mp.getProperty ("skipped", 0); }
    else l << " | skipped " << (int) progress.getProperty ("skipped", 0);
    // 22 Oct (Kathy: Maag MAGNUM-K held the bar for 28 min): the now line (the plugin + what it is doing) and each running job with its
    // own steps, so a long single plugin is visibly moving in the log too
    const auto nw = nowWords (progress); if (nw.isNotEmpty()) l << " | now '" << nw << "'";
    juce::StringArray js; for (const auto& j : jobLines (progress)) js.add (j.name + (j.fraction >= 0.0 ? " " + juce::String (juce::roundToInt (100.0 * juce::jlimit (0.0, 1.0, j.fraction))) + "%" : juce::String (" checking")));
    if (! js.isEmpty()) l << " | jobs '" << js.joinIntoString ("; ") << "'";
    if (power.isNotEmpty()) l << " | power " << power;   // 23 Oct: the battery
    return l;
}
// THE STOP (Kathy, 13 Oct, beta item 1): the same words whether it stopped while checking or while measuring
// FINISH NOW (Kathy, 13 Oct): the app writes this file in the engine's folder, then stops it as a pause does; the engine, seeing it after
// the stop, packs what landed instead of ending as stopped. A disk stop never finishes (there may be no room to pack)
inline constexpr const char* kFinishRequestFile = "finish_requested";
inline bool finishAfterStop (int phasebCode, bool finishRequested, bool diskStopped) { return phasebCode == 130 && finishRequested && ! diskStopped; }
// "N measured so far" (the Measuring screen): what landed without a problem
inline int measuredSoFar (const juce::var& progress)
{
    if (progress.getProperty ("stage", "").toString() != "Measuring") return 0;
    if (progress.hasProperty ("measured_done")) return (int) progress.getProperty ("measured_done", 0);   // 14 Oct: never a demo check
    const auto sc = progress.getProperty ("stage_counts", {});
    return juce::jmax (0, (int) sc.getProperty ("done", 0) - (int) progress.getProperty ("problems", 0));
}
inline juce::String measuredSoFarWords (int n) { return n == 1 ? juce::String ("1 measured so far") : juce::String (n) + " measured so far"; }
// the Send-what's-done button is live only once the engine is measuring (a finish while it still lists / reads signatures would pack nothing)
inline bool finishAvailable (const juce::var& progress) { return progress.getProperty ("stage", "").toString() == "Measuring"; }
// THE QUIET CHECKPOINT (Kathy, 13 Oct): every ~60 min while measuring, what is done is packed (release: uploaded; beta: the Desktop file)
// a checkpoint the app has not handled yet: its time stamp differs from the last one handled (kept in app_state across launches)
inline bool isNewCheckpoint (const juce::String& at, const juce::String& lastHandledAt) { return at.isNotEmpty() && at != lastHandledAt; }
inline bool checkpointDue (double lastMs, double nowMs, double minutes) { return minutes > 0.0 && nowMs - lastMs >= minutes * 60000.0; }
// THE MEASURING CHOICES (Kathy, 13 Oct): on the Measuring screen and when the window is closed - pause (the default) or send what's done.
// The release says what sending early costs; the beta (no reward) says the Desktop file is always there
struct Choices { juce::String pause, send, line; };
inline Choices measuringChoices (Mode m)
{
    if (m == Mode::beta) return { "Pause - carry on later", "Finish and send what's done", "Each result is sent to EchoJay as soon as it's measured." };
    return { "Pause - carry on later", "Send what's done (no free month)", "The free month needs the full run - you can carry on later instead." };
}
// JOBS BY MAC (Kathy, 13 Oct): 2 by default; 4 on a Mac with 8+ performance cores AND enough free memory at launch (the serial lane is
// unchanged: UAD / licence / PACE / history rows still run one at a time). The estimates use the chosen number
// THE MEMORY, MEASURED (Kathy, 14 Oct; the 4 GB guess replaced): one job = the measuring child + its probe + the macOS AU bridge host
// for an x86_64-only plugin. Peak footprint on this Mac (top, every ~9 s, four plugins both ways): one-shot 29-89 MB, the warm probe
// 39-146 MB (H-Reverb: the keeper and the measured instance both live in the bridge host; townhouse native 36-39 MB). A job is reserved
// at twice the largest, 300 MB; 4 jobs need the Phase B pool's own low-memory floor (1.5 GB, EjmapJobs.h kMinFreeBytes - it still holds
// new jobs back whenever free memory falls under it) plus 4 x 300 MB: 2.7 GB free at launch
inline constexpr double kJobPeakBytes = 300.0 * 1024.0 * 1024.0;
inline constexpr double kPoolFloorBytes = 1.5 * 1024.0 * 1024.0 * 1024.0;   // = jobs::kMinFreeBytes
inline constexpr double kJobs4MinFreeBytes = kPoolFloorBytes + 4.0 * kJobPeakBytes;
inline int jobsFor (int performanceCores, double freeBytes) { return performanceCores >= 8 && freeBytes >= kJobs4MinFreeBytes ? 4 : 2; }
// THE OUTBOX (Kathy, 13 Oct: offline) - an upload that could not be sent is kept in <work>/outbox (zip + manifest, numbered) and sent on the
// next launch or the next try, oldest first; a send that fails stops the drain (the order is kept)
inline juce::File outboxDir (const juce::File& work) { return work.getChildFile ("outbox"); }
inline juce::File outboxPut (const juce::File& work, const juce::File& zip, const juce::File& manifest, const juce::String& kind)
{
    const auto d = outboxDir (work); d.createDirectory();
    int n = 1; for (const auto& f : d.findChildFiles (juce::File::findFiles, false, "*.zip")) n = juce::jmax (n, f.getFileNameWithoutExtension().upToFirstOccurrenceOf ("_", false, false).getIntValue() + 1);
    const auto stem = juce::String (n).paddedLeft ('0', 4) + "_" + kind;
    zip.copyFileTo (d.getChildFile (stem + ".zip")); manifest.copyFileTo (d.getChildFile (stem + ".manifest.json"));
    return d.getChildFile (stem + ".zip");
}
// send(zip, manifest) -> true when the server answered; returns how many were sent
inline int outboxDrain (const juce::File& work, const std::function<bool (const juce::File&, const juce::File&)>& send)
{
    auto files = outboxDir (work).findChildFiles (juce::File::findFiles, false, "*.zip");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
    int sent = 0;
    for (const auto& z : files)
    {
        const auto m = z.getSiblingFile (z.getFileNameWithoutExtension() + ".manifest.json");
        if (! send (z, m)) break;
        z.deleteFile(); m.deleteFile(); ++sent;
    }
    return sent;
}
// THE BETA'S ONE FILE (Kathy, 13 Oct): the Desktop results file is REWRITTEN on every pause / close / hourly checkpoint / finish - the same
// file each time (its name chosen once, kept in app_state "saved_file"), so a tester who stopped early always has one file to email
inline juce::File betaResultsTarget (const juce::String& savedPath, const juce::File& desk, const juce::Time& now)
{
    if (savedPath.isNotEmpty()) return juce::File (savedPath);
    auto f = desk.getChildFile (resultsFileName (now));
    for (int k = 2; f.existsAsFile(); ++k) f = desk.getChildFile ("EchoJay-Wizard-results-" + now.formatted ("%Y-%m-%d") + "-" + juce::String (k) + ".zip");
    return f;
}
inline bool rewriteResultsFile (const juce::File& src, const juce::File& target)
{
    if (! src.existsAsFile()) return false;
    const auto tmp = target.getSiblingFile ("." + target.getFileName() + ".tmp");
    return src.copyFileTo (tmp) && tmp.moveFileTo (target);
}
inline juce::String stoppedNote() { return "Stopped - it carries on next time"; }
// THE PACK SAYS SO AT ONCE (Kathy, 29 Oct): the engine's pack_refused.json (a pack it could not make) and privacy_withheld.json (files it
// set aside) in plain words for the measuring screen; "" when neither has anything to say
inline juce::String packNoteWords (const juce::var& refused, const juce::var& privacy)
{
    juce::StringArray parts;
    if (refused.isObject() && refused.getProperty ("why", "").toString().isNotEmpty())
        parts.add ("The latest results couldn't be packed to send (" + refused.getProperty ("why", "").toString() + ") - they're kept on this Mac");
    juce::StringArray products;
    if (const auto* w = privacy.getProperty ("withheld", {}).getArray()) for (const auto& e : *w) { const auto pid = e.getProperty ("plugin_id", "").toString(); products.addIfNotAlreadyThere (pid.isNotEmpty() ? pid : e.getProperty ("file", "").toString()); }   // one per plugin
    if (const int n = products.size(); n > 0) parts.add (juce::String (n) + (n == 1 ? " result is" : " results are") + " kept on this Mac, not sent (privacy check)");
    return parts.joinIntoString (". ");
}
// THE STOPPED SCREEN (Kathy, 29 Oct: "Stopped" kept Pause and Finish and send, which did nothing once the engine had ended): one button,
// Quit, and the reason when the engine gave one - a low disk in its own words, a pack it could not make in the pack's
inline Copy stoppedCopy (const juce::String& engineSummary, const juce::String& packNote)
{
    Copy c; c.title = "Stopped"; c.primary = "Quit";
    c.text = engineSummary.containsIgnoreCase ("disk space") ? engineSummary
           : (packNote.isNotEmpty() ? packNote + ". " : juce::String()) + stoppedNote() + " - open the wizard again to carry on.";
    return c;
}
// what the window does when the engine ends: a scan that ran to its end -> its results; a measuring run that did -> the upload; a
// STOPPED one (130, or any other end) never counts as finished - the scan is not marked done, and the next launch carries on
// A RUN THAT REACHED THE END WITH PLUGINS LEFT (Kathy, 15 Oct: 774fe72e's run ended "The free month needs the full run: 351 of 509" - the 158
// UAD rows it had left unrun): the engine never uploads that as finished. It sends what is done as a checkpoint, writes engine/left.json
// (each plugin left and why) and ends with kLeftExit; the app says what is left and offers Carry on
inline constexpr int kLeftExit = 9;
enum class AfterEngine { scanDone, upload, stopped, left };
inline AfterEngine afterEngine (Screen s, int code)
{
    if (code == 0) return s == Screen::scan ? AfterEngine::scanDone : AfterEngine::upload;
    if (code == kLeftExit && s == Screen::measuring) return AfterEngine::left;
    return AfterEngine::stopped;
}
inline juce::String leftWords (const juce::var& left)
{
    const auto* a = left.getArray(); const int n = a != nullptr ? a->size() : 0;
    if (n == 0) return {};
    juce::StringArray names, whys; for (const auto& e : *a) { if (names.size() < 3) names.add (e.getProperty ("product", "").toString()); whys.addIfNotAlreadyThere (e.getProperty ("why", "").toString()); }
    whys.removeEmptyStrings();
    const auto who = names.joinIntoString (", ") + (n > 3 ? " and " + juce::String (n - 3) + " more" : juce::String());
    return juce::String (n) + (n == 1 ? " plugin still needs" : " plugins still need") + " measuring: " + who + (whys.isEmpty() ? juce::String() : " (" + whys[0] + ")") + ". Carry on to finish the run.";
}
inline Screen openOn (const Where& w, Mode m = Mode::normal)
{
    if (m == Mode::normal && ! w.signedIn) return Screen::welcome;
    if (m == Mode::beta && ! w.signedIn) return Screen::welcome;   // 21 Oct: the beta signs in (in the browser) first
    if (w.serverCompleted) return Screen::consent;   // 27 Oct (B: 409 run_completed): that run is over on EchoJay - the next Start is a new run (freshRunIfNeeded)
    if (! w.consented && ! w.scanned && ! w.measureStarted) return Screen::consent;   // 13 Oct: consent before anything is scanned
    if (w.rewarded) return Screen::reward;
    if (w.crashed > 0) return Screen::crashRecovered;             // a crash is said before anything carries on
    if (m == Mode::beta && w.saved && ! w.uploaded) return Screen::upload;   // 21 Oct: a run that ended but whose end was not reported yet
    if (w.uploaded) return Screen::reward;
    if (w.measureDone) return afterMeasure (m);
    if (w.measureStarted) return Screen::measuring;                // closed or stopped mid-measure: carry on
    if (w.scanned) return Screen::scanDone;
    return Screen::scan;
}
// 15 Oct: an app reopened after a finished scan carries on the same way - straight into measuring when there is something to measure
inline Screen openOn (const Where& w, Mode m, const Eligibility& e) { const auto s = openOn (w, m); return s == Screen::scanDone ? afterScan (e, m) : s; }
} // namespace ejmap::wizapp
