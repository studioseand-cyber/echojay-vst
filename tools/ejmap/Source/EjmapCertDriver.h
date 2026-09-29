/*
  EjmapCertDriver.h

  EJ MAP AS THE DRIVER (certification plan step 5). The signed probe MEASURES; this
  file orchestrates it and DERIVES from what it prints (decision D2). Its first job is
  regenerating the pushed compressor-profile fixtures from live probe runs, and
  proving it by comparing field by field. Design requirements and the acceptance
  criterion are in docs/EJMAP_CERT_DRIVER.md; the parts that shape this code:

    - SIGN ONCE AND FAIL LOUDLY (doc section 1). The probe's signature is checked
      before the first product, under a deadline, and an unsigned or unverifiable
      probe aborts the run before any plugin is touched. With --sign-identity the
      driver signs ONCE first, also under a deadline, because a locked keychain
      makes codesign BLOCK rather than fail.
    - THE DRIVER OWNS EVERY TIMEOUT (doc section 2). Every probe run has a
      deadline enforced from outside, and the child is killed on expiry. The wait
      status is read with waitpid, never through juce::ChildProcess, whose
      getExitCode returns 0 for a child killed by a signal - a crash would read as a
      clean exit.
    - THE RETRY RULE (spec section 6, the Neve 33609 case). A run that is killed or
      dies abnormally is re-run ONCE, alone. A second failure is recorded with its
      reason. Two timeouts mark a hang. Every attempt is written to run.jsonl, and a
      retried product is named in the report.
    - UNLICENSED IS NOT BROKEN (doc section 3). PACE-wrapped products are HELD, not
      run, while step 2 is open (--include-pace overrides). A held product is
      reported as held, never as a failure.
    - COVERAGE FIRST (doc section 0). The report opens with what was reachable and
      what was not, and the out-of-reach products are listed by name.

  Two modes, both headless (Main.cpp runHeadlessCli):
    --cert-rederive <fixturesDir>
        Re-derives range, direction and unit from each fixture's OWN displayAt. No
        probe, no plugin, so it covers all 74. It proves the derivation code.
    --cert-defaults --fixtures <dir> --probe <EchoJayProbe> --out <dir>
                    [--timeout-s N] [--sign-identity ID --entitlements FILE] [--include-pace]
        Regenerates every reachable fixture through the probe and compares it.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <CoreFoundation/CoreFoundation.h>
#include "EchoJayAuRegistry.h"
#include "EjmapFixtureUnit.h"
#include "EjmapFixtureRange.h"
#include "EjmapFixtureReadout.h"

#include <CoreGraphics/CoreGraphics.h>
#include <libproc.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <map>
#include <set>
#include <vector>
#include <iostream>

extern char** environ;

namespace ejmap::cert
{

//==============================================================================
// A CHILD PROCESS WITH A DEADLINE, AND ITS EXACT WAIT STATUS.
struct ChildResult
{
    // uiShown: a window belonging to the child's process tree came on screen, and
    // the driver killed the run at once. The probe never shows a window of its own
    // (LSBackgroundOnly, no editor), so any window in its tree is a plugin's or a
    // licence wrapper's - e.g. PACE's "Software Activation" (PACEEdenExperience),
    // a child of the probe, seen 28 Sep on Eiosis E2Deesser.
    enum class Kind { exited, signaled, timedOut, uiShown, spawnFailed } kind = Kind::spawnFailed;
    int code = 0;             // exit code when exited
    int signal = 0;           // terminating signal when signaled
    juce::String out;         // stdout and stderr, interleaved
    juce::StringArray windowsInTree;   // owner names of on-screen windows the child's tree showed
    double ms = 0.0;

    bool cleanExit (int wanted = 0) const { return kind == Kind::exited && code == wanted; }
    juce::String describe() const
    {
        switch (kind)
        {
            case Kind::exited:      return "exit " + juce::String (code);
            case Kind::signaled:    return "killed by signal " + juce::String (signal)
                                           + (signal == SIGTERM ? " (SIGTERM: a caller's timeout, not a refusal)" : "");
            case Kind::timedOut:    return "timed out after " + juce::String (ms / 1000.0, 1) + " s (killed by the driver)";
            case Kind::uiShown:     return "SHOWED A WINDOW (" + windowsInTree.joinIntoString (", ")
                                           + ") after " + juce::String (ms / 1000.0, 1) + " s; killed by the driver";
            case Kind::spawnFailed: return "could not be started";
        }
        return "?";
    }
};

//==============================================================================
// WINDOW WATCHING (docs/EJMAP_CERT_DRIVER.md section 3: a precondition before any
// PACE subject joins). An activation dialog blocks instantiation, so without this
// it can only ever surface as a timeout, indistinguishable from a hang.
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

// Owner names of windows owned by `root` or any descendant. onScreenOnly is the
// production setting: a dialog is a window the user can SEE. The option is a
// parameter so the attribution and the kill path can be tested with an off-screen
// window, without flashing anything on the user's desktop.
inline juce::StringArray windowsOwnedByTree (pid_t root, bool onScreenOnly = true)
{
    juce::StringArray owners;
    const CGWindowListOption opt = onScreenOnly ? kCGWindowListOptionOnScreenOnly : kCGWindowListOptionAll;
    CFArrayRef list = CGWindowListCopyWindowInfo (opt, kCGNullWindowID);
    if (list == nullptr) return owners;
    for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
    {
        auto w = (CFDictionaryRef) CFArrayGetValueAtIndex (list, i);
        auto pidRef = (CFNumberRef) CFDictionaryGetValue (w, kCGWindowOwnerPID);
        int pid = 0;
        if (pidRef == nullptr || ! CFNumberGetValue (pidRef, kCFNumberIntType, &pid)) continue;
        if (! inTreeOf ((pid_t) pid, root)) continue;
        auto name = (CFStringRef) CFDictionaryGetValue (w, kCGWindowOwnerName);
        owners.addIfNotAlreadyThere ((name != nullptr ? juce::String::fromCFString (name) : juce::String ("?"))
                                     + " [pid " + juce::String (pid) + "]");
    }
    CFRelease (list);
    return owners;
}

struct WatchOptions { bool watchWindows = true; bool onScreenOnly = true; int pollMs = 250; };

inline ChildResult runChild (const juce::StringArray& args, int timeoutMs, WatchOptions watch = {})
{
    ChildResult r;
    int fds[2];
    if (args.isEmpty() || pipe (fds) != 0) return r;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init (&fa);
    posix_spawn_file_actions_adddup2 (&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2 (&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose (&fa, fds[0]);

    std::vector<std::string> store;
    for (const auto& a : args) store.push_back (a.toStdString());
    std::vector<char*> argv;
    for (auto& s : store) argv.push_back (s.data());
    argv.push_back (nullptr);

    pid_t pid = 0;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const int rc = posix_spawn (&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy (&fa);
    close (fds[1]);
    if (rc != 0) { close (fds[0]); return r; }

    fcntl (fds[0], F_SETFL, O_NONBLOCK);
    juce::MemoryOutputStream collected;
    int status = 0;
    bool reaped = false, killed = false, uiKilled = false;
    double lastWindowPoll = 0.0;
    char buf[8192];
    auto drain = [&] { for (;;) { const ssize_t n = read (fds[0], buf, sizeof buf); if (n > 0) collected.write (buf, (size_t) n); else break; } };
    for (;;)
    {
        pollfd p { fds[0], POLLIN, 0 };
        poll (&p, 1, 20);
        drain();
        if (! reaped && waitpid (pid, &status, WNOHANG) == pid) reaped = true;
        if (reaped) { drain(); break; }
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (watch.watchWindows && now - lastWindowPoll >= watch.pollMs)
        {
            lastWindowPoll = now;
            const auto shown = windowsOwnedByTree (pid, watch.onScreenOnly);
            if (! shown.isEmpty())
            {
                r.windowsInTree = shown;
                // The whole tree goes: the wrapper's UI process is a child of the probe.
                for (int k = 0; k < 16; ++k)
                {
                    bool any = false;
                    int n = proc_listchildpids (pid, nullptr, 0);
                    std::vector<pid_t> kids ((size_t) juce::jmax (0, n) + 8);
                    n = proc_listchildpids (pid, kids.data(), (int) (kids.size() * sizeof (pid_t)));
                    for (int c = 0; c < n; ++c) if (kids[(size_t) c] > 1) { kill (kids[(size_t) c], SIGKILL); any = true; }
                    if (! any) break;
                }
                kill (pid, SIGKILL);
                waitpid (pid, &status, 0);
                uiKilled = reaped = true;
                drain();
                break;
            }
        }
        if (now - t0 > timeoutMs)
        {
            kill (pid, SIGKILL);
            waitpid (pid, &status, 0);
            killed = reaped = true;
            drain();
            break;
        }
    }
    close (fds[0]);
    r.ms  = juce::Time::getMillisecondCounterHiRes() - t0;
    r.out = collected.toString();
    if (uiKilled)                r.kind = ChildResult::Kind::uiShown;
    else if (killed)             r.kind = ChildResult::Kind::timedOut;
    else if (WIFEXITED (status))   { r.kind = ChildResult::Kind::exited;   r.code = WEXITSTATUS (status); }
    else if (WIFSIGNALED (status)) { r.kind = ChildResult::Kind::signaled; r.signal = WTERMSIG (status); }
    return r;
}

//==============================================================================
// THE SIGNATURE GATE. Sign once (optional) under a deadline, then verify.
struct ProbeIdentity { bool ok = false; juce::String why, team, cdhash; };

inline ProbeIdentity checkProbe (const juce::File& probe, const juce::String& signIdentity,
                                 const juce::File& entitlements)
{
    ProbeIdentity id;
    if (! probe.existsAsFile()) { id.why = "no probe at " + probe.getFullPathName(); return id; }
    if (signIdentity.isNotEmpty())
    {
        auto s = runChild ({ "/usr/bin/codesign", "-f", "-s", signIdentity, "--timestamp", "--options", "runtime",
                             "--entitlements", entitlements.getFullPathName(), probe.getFullPathName() }, 20000);
        if (s.kind == ChildResult::Kind::timedOut)
        { id.why = "SIGNING BLOCKED: codesign did not return within 20 s. A keychain prompt is the likely cause "
                   "(locked keychain, or a key not set to Always Allow). Nothing was run."; return id; }
        if (! s.cleanExit()) { id.why = "signing failed (" + s.describe() + "): " + s.out.trim(); return id; }
    }
    auto v = runChild ({ "/usr/bin/codesign", "--verify", "--strict", probe.getFullPathName() }, 20000);
    if (! v.cleanExit()) { id.why = "the probe is not validly signed (" + v.describe() + "): " + v.out.trim()
                                    + ". PACE refuses an unsigned probe, so nothing was run."; return id; }
    auto d = runChild ({ "/usr/bin/codesign", "-dvvv", probe.getFullPathName() }, 20000);
    for (auto line : juce::StringArray::fromLines (d.out))
    {
        line = line.trim();
        if (line.startsWith ("TeamIdentifier=")) id.team = line.fromFirstOccurrenceOf ("=", false, false);
        if (line.startsWith ("CDHash="))         id.cdhash = line.fromFirstOccurrenceOf ("=", false, false);
    }
    if (id.team.isEmpty() || id.team == "not set")
    { id.why = "the probe verifies but has no Team Identifier (ad-hoc signed). PACE needs a Developer ID "
               "signature, so nothing was run."; return id; }
    id.ok = true;
    return id;
}

//==============================================================================
// PACE, by the probe line's own rule (merge/kathy-2026-09-06 Source/EJPaceCheck.h
// isPaceWrapped, at 971e4c1). Ported, not included: that header is not on this
// branch. If the rule changes there, it must change here.
inline bool isPaceWrapped (const juce::File& bundle)
{
    const auto contents = bundle.getChildFile ("Contents");
    if (contents.getChildFile ("__Pace_Eden.bundle").exists()
        || contents.getChildFile ("Resources").getChildFile ("__Pace_Eden.bundle").exists())
        return true;
    for (const auto& f : contents.getChildFile ("Frameworks").findChildFiles (juce::File::findFilesAndDirectories, false))
        if (f.getFileName().containsIgnoreCase ("pace") || f.getFileName().containsIgnoreCase ("eden"))
            return true;
    for (const auto& bin : contents.getChildFile ("MacOS").findChildFiles (juce::File::findFiles, false))
    {
        juce::MemoryBlock mb;
        if (! bin.loadFileAsData (mb)) continue;
        for (const char* needle : { "PACEAntiPiracy", "com.paceap", "PaceAP", "__Pace_Eden" })
        {
            const auto* data = static_cast<const char*> (mb.getData());
            const size_t n = mb.getSize(), k = std::strlen (needle);
            for (size_t i = 0; i + k <= n; ++i)
                if (data[i] == needle[0] && std::memcmp (data + i, needle, k) == 0)
                    return true;
        }
    }
    return false;
}

// "type,subtype,manufacturer" -> the .component bundle that registers it, from each
// bundle's own Info.plist. Built-in system components have no bundle here.
inline std::map<juce::String, juce::File> componentBundles()
{
    std::map<juce::String, juce::File> out;
    for (const auto& dir : { juce::File ("/Library/Audio/Plug-Ins/Components"),
                             juce::File ("~/Library/Audio/Plug-Ins/Components") })
        for (const auto& b : dir.findChildFiles (juce::File::findDirectories, false, "*.component"))
        {
            const auto path = b.getFullPathName();
            auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(),
                                                                (CFIndex) path.getNumBytesAsUTF8(), true);
            if (url == nullptr) continue;
            if (auto bundle = CFBundleCreate (nullptr, url))
            {
                if (auto comps = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (bundle, CFSTR ("AudioComponents")))
                    if (CFGetTypeID (comps) == CFArrayGetTypeID())
                        for (CFIndex i = 0; i < CFArrayGetCount (comps); ++i)
                            if (auto d = (CFDictionaryRef) CFArrayGetValueAtIndex (comps, i))
                                if (CFGetTypeID (d) == CFDictionaryGetTypeID())
                                {
                                    auto s = [d] (CFStringRef k) {
                                        auto v = (CFStringRef) CFDictionaryGetValue (d, k);
                                        return v != nullptr && CFGetTypeID (v) == CFStringGetTypeID()
                                                 ? juce::String::fromCFString (v) : juce::String(); };
                                    const auto key = s (CFSTR ("type")) + "," + s (CFSTR ("subtype")) + "," + s (CFSTR ("manufacturer"));
                                    out.emplace (key, b);
                                }
                CFRelease (bundle);
            }
            CFRelease (url);
        }
    return out;
}

//==============================================================================
// REACHABILITY of one fixture on this machine.
struct Subject
{
    juce::File fixtureFile;
    juce::var pushed;
    juce::String product, uid, version;
    enum class Reach { reachable, reachableNoVersion, heldPace, versionMismatch, notInstalled, ambiguous } reach
        = Reach::notInstalled;
    juce::String detail;                 // installed version(s), the ambiguity, ...
    juce::PluginDescription desc;        // resolved component, when installed
};

inline juce::String reachName (Subject::Reach r)
{
    switch (r)
    {
        case Subject::Reach::reachable:          return "reachable";
        case Subject::Reach::reachableNoVersion: return "reachable (fixture records no version)";
        case Subject::Reach::heldPace:           return "held: PACE-wrapped or unverifiable, waits for step 2";
        case Subject::Reach::versionMismatch:    return "out of reach: installed at another version";
        case Subject::Reach::notInstalled:       return "out of reach: not installed";
        case Subject::Reach::ambiguous:          return "out of reach: uid matches more than one component";
    }
    return "?";
}

inline std::vector<Subject> loadFixtures (const juce::File& dir)
{
    std::vector<Subject> out;
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.json");
    files.sort();
    for (const auto& f : files)
    {
        Subject s;
        s.fixtureFile = f;
        s.pushed = juce::JSON::parse (f.loadFileAsString());
        if (! s.pushed.isObject()) continue;
        s.product = s.pushed.getProperty ("product", "").toString();
        s.uid     = s.pushed.getProperty ("uid", "").toString().toLowerCase();
        s.version = s.pushed.getProperty ("version", "").toString();
        out.push_back (s);
    }
    return out;
}

inline void classify (std::vector<Subject>& subjects, bool includePace)
{
    const auto bundles = componentBundles();
    std::multimap<juce::String, juce::PluginDescription> byUid;      // registry, resolved once
    const auto census = echojay::auregistry::buildCensus();
    for (const auto& t : census.targets)
    {
        auto d = echojay::auregistry::describeFromRegistry (t.identifier);
        if (d.fileOrIdentifier.isNotEmpty())
            byUid.emplace (juce::String::toHexString (d.uniqueId).toLowerCase(), d);
    }
    // THE HOLD FAILS SAFE. First version: "no bundle found" meant "not PACE", and the
    // two McDSP APB compressors ran while step 2 was open - PACE-wrapped (Eden bundle
    // and PACE bytes both present), but registered without an AudioComponents key, so
    // no plist scan finds them. A component whose PACE state cannot be CHECKED is
    // held, and says so. Apple's own built-ins have no bundle here and are exempt.
    auto paceState = [&bundles] (const juce::PluginDescription& d, juce::String& why) {
        const auto code = d.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false);
        auto it = bundles.find (code);
        if (it != bundles.end())
        {
            if (isPaceWrapped (it->second)) { why = "PACE-wrapped (" + it->second.getFileName() + ")"; return true; }
            return false;
        }
        if (code.endsWith (",appl")) return false;      // macOS built-in, no bundle to check
        why = "no bundle found for " + code + ", so its PACE state cannot be checked: held, not assumed clear";
        return true; };
    auto isPace = [&paceState] (const juce::PluginDescription& d, juce::String& why) { return paceState (d, why); };

    for (auto& s : subjects)
    {
        // The three fixtures whose `version` holds the component code instead of a
        // version: resolve that exact component (Apple's are outside the census).
        if (s.version.containsChar (','))
        {
            auto d = echojay::auregistry::describeFromRegistry ("AudioUnit:Effects/" + s.version);
            if (d.fileOrIdentifier.isEmpty() || d.name.isEmpty() || d.name == "<Unknown>")
            { s.reach = Subject::Reach::notInstalled; s.detail = "component " + s.version + " is not registered"; continue; }
            s.desc = d;
            juce::String why;
            const bool held = ! includePace && isPace (d, why);
            s.detail = held ? why : "installed " + d.version + "; the fixture records no version to check it against";
            s.reach = held ? Subject::Reach::heldPace : Subject::Reach::reachableNoVersion;
            continue;
        }
        juce::Array<juce::PluginDescription> atVersion;
        juce::StringArray versions;
        for (auto [it, end] = byUid.equal_range (s.uid); it != end; ++it)
        {
            versions.addIfNotAlreadyThere (it->second.version);
            if (it->second.version == s.version) atVersion.add (it->second);
        }
        if (versions.isEmpty()) { s.reach = Subject::Reach::notInstalled; continue; }
        if (atVersion.isEmpty())
        { s.reach = Subject::Reach::versionMismatch; s.detail = "fixture " + s.version + ", installed " + versions.joinIntoString (" / "); continue; }
        if (atVersion.size() > 1)
        {
            s.reach = Subject::Reach::ambiguous;
            for (const auto& d : atVersion) s.detail << d.fileOrIdentifier << " ";
            continue;                                   // never pick one
        }
        s.desc = atVersion.getReference (0);
        juce::String why;
        const bool held = ! includePace && isPace (s.desc, why);
        if (held) s.detail = why;
        s.reach = held ? Subject::Reach::heldPace : Subject::Reach::reachable;
    }
}

//==============================================================================
// THE PROBE'S OUTPUT, parsed. Lines are tab-separated; see au_instantiate_probe.cpp.
struct ListRow { juce::String name, label; int numSteps = 0; bool discrete = false, automatable = false, meta = false; };
struct TextAtRow { int index = -1; juce::String name, label, defText; double defNorm = 0, declared = 0;
                   std::map<juce::String, juce::String> at; };

inline std::map<int, ListRow> parseListParams (const juce::String& out)
{
    std::map<int, ListRow> rows;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() != 7 || ! f[0].containsOnly ("0123456789") || f[0].isEmpty()) continue;
        ListRow r { f[1], f[2], f[3].getIntValue(), f[4] == "1", f[5] == "1", f[6] == "1" };
        rows[f[0].getIntValue()] = r;
    }
    return rows;
}

inline std::vector<TextAtRow> parseTextAt (const juce::String& out)
{
    std::vector<TextAtRow> rows;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() >= 5 && f[0] == "param" && f[3] == "unit")
            rows.push_back ({ f[1].getIntValue(), f[2], f[4] });
        else if (f.size() >= 5 && f[0] == "def" && ! rows.empty())
        { rows.back().defNorm = f[1].getDoubleValue(); rows.back().defText = f[2]; rows.back().declared = f[4].getDoubleValue(); }
        else if (f.size() >= 2 && f[0] == "at" && ! rows.empty())
            rows.back().at[f[1]] = f.size() >= 3 ? f[2] : juce::String();
    }
    return rows;
}

//==============================================================================
// THE FIXTURE, composed from probe output through the committed derivations.
inline const char* kProfileNote  = "PROFILE ONLY - nothing certified, nothing written to the plugin.";
inline const char* kDefaultsNote = "the value the plugin instantiated with; declaredDefault is what the plugin calls "
                                   "its default - they are not always the same";

inline juce::var composeFixture (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                 int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date)
{
    // IDENTITY FROM THE RESOLVED COMPONENT, never copied from the fixture: copying
    // would make these fields match by construction and the comparison circular.
    // The three version-less fixtures carry the component code in `version`, so for
    // them the code is what the component is asked for.
    const auto uid = juce::String::toHexString (s.desc.uniqueId).toLowerCase();
    const auto version = s.reach == Subject::Reach::reachableNoVersion
                           ? s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)
                           : s.desc.version;
    auto* o = new juce::DynamicObject();
    o->setProperty ("identity", "AudioUnit|" + uid + "|" + version);
    o->setProperty ("product", s.desc.name);
    o->setProperty ("format", s.desc.pluginFormatName.isNotEmpty() ? s.desc.pluginFormatName : juce::String ("AudioUnit"));
    o->setProperty ("uid", uid);
    o->setProperty ("version", version);
    o->setProperty ("sampledAt", date);
    o->setProperty ("certified", false);
    o->setProperty ("note", kProfileNote);
    o->setProperty ("probe", probeLabel);
    o->setProperty ("listParamsRc", listRc);
    o->setProperty ("textAtRc", textAtRc);
    juce::Array<juce::var> controls;
    for (const auto& t : textAt)
    {
        auto* c = new juce::DynamicObject();
        const auto a0 = t.at.count ("0.000") ? t.at.at ("0.000") : juce::String();
        const auto a5 = t.at.count ("0.500") ? t.at.at ("0.500") : juce::String();
        const auto a1 = t.at.count ("1.000") ? t.at.at ("1.000") : juce::String();
        c->setProperty ("index", t.index);
        c->setProperty ("name", t.name);
        const auto unit = fixtureunit::unitFor (t.label, a0, a1);
        c->setProperty ("unit", unit.isEmpty() ? juce::var() : juce::var (unit));
        auto* at = new juce::DynamicObject();
        at->setProperty ("0.000", a0); at->setProperty ("0.500", a5); at->setProperty ("1.000", a1);
        c->setProperty ("displayAt", juce::var (at));
        auto lr = list.find (t.index);
        c->setProperty ("numSteps", lr != list.end() ? lr->second.numSteps : 0);
        c->setProperty ("discrete", lr != list.end() && lr->second.discrete);
        const auto dr = fixturerange::derive (a0, a5, a1);
        c->setProperty ("range", dr.range);
        c->setProperty ("direction", dr.direction);
        auto* d = new juce::DynamicObject();
        d->setProperty ("normalised", t.defNorm);
        d->setProperty ("display", t.defText);
        d->setProperty ("declaredDefault", t.declared);
        d->setProperty ("note", kDefaultsNote);
        c->setProperty ("defaultOnInstantiate", juce::var (d));
        controls.add (juce::var (c));
    }
    o->setProperty ("controls", controls);
    o->setProperty ("defaultsSampledAt", date);
    o->setProperty ("defaultsProbe", probeLabel);
    return juce::var (o);
}

//==============================================================================
// FIELD-BY-FIELD COMPARISON. Numbers compare as doubles (JSON has one number type);
// everything else by kind and value. Returns one line per differing path.
inline void diffVar (const juce::var& a, const juce::var& b, const juce::String& path, juce::StringArray& out)
{
    auto isNum = [] (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); };
    if (isNum (a) && isNum (b)) { if ((double) a != (double) b) out.add (path + ": " + a.toString() + " vs " + b.toString()); return; }
    if (a.isVoid() || b.isVoid()) { if (! (a.isVoid() && b.isVoid())) out.add (path + ": " + (a.isVoid() ? "null" : a.toString())
                                                                             + " vs " + (b.isVoid() ? "null" : b.toString())); return; }
    if (a.isBool() || b.isBool()) { if (! (a.isBool() && b.isBool() && (bool) a == (bool) b)) out.add (path + ": " + a.toString() + " vs " + b.toString()); return; }
    if (a.isString() || b.isString()) { if (! (a.isString() && b.isString() && a.toString() == b.toString()))
                                            out.add (path + ": \"" + a.toString() + "\" vs \"" + b.toString() + "\""); return; }
    if (auto* aa = a.getArray())
    {
        auto* ba = b.getArray();
        if (ba == nullptr) { out.add (path + ": array vs non-array"); return; }
        if (aa->size() != ba->size()) out.add (path + ": " + juce::String (aa->size()) + " vs " + juce::String (ba->size()) + " entries");
        for (int i = 0; i < juce::jmin (aa->size(), ba->size()); ++i) diffVar ((*aa)[i], (*ba)[i], path + "[" + juce::String (i) + "]", out);
        return;
    }
    auto* ao = a.getDynamicObject(); auto* bo = b.getDynamicObject();
    if (ao == nullptr || bo == nullptr) { if (a.toString() != b.toString()) out.add (path + ": differ"); return; }
    for (auto& kv : ao->getProperties())
        if (! bo->hasProperty (kv.name)) out.add (path + "." + kv.name.toString() + ": only in the regenerated fixture");
        else diffVar (kv.value, bo->getProperty (kv.name), path + "." + kv.name.toString(), out);
    for (auto& kv : bo->getProperties())
        if (! ao->hasProperty (kv.name)) out.add (path + "." + kv.name.toString() + ": only in the pushed fixture");
}

inline juce::StringArray compareFixtures (const juce::var& regenerated, const juce::var& pushed)
{
    static const juce::StringArray provenance { "sampledAt", "probe", "defaultsSampledAt", "defaultsProbe" };
    // A deep copy, NOT a JSON round trip: re-printing a double could move it and
    // manufacture a difference that no measurement made.
    auto strip = [] (const juce::var& v) {
        auto copy = v.clone();
        if (auto* o = copy.getDynamicObject()) for (const auto& k : provenance) o->removeProperty (k);
        return copy; };
    juce::StringArray out;
    diffVar (strip (regenerated), strip (pushed), "", out);
    return out;
}

//==============================================================================
// MODE 1: --cert-rederive. Every control of every fixture, re-derived from its own
// displayAt. range and direction must match exactly. unit can only be checked one
// way: a control whose fixture has NO unit must derive none from its text (a label
// can supply a unit, never remove one). The rest are counted, not claimed.
inline int runRederive (const juce::File& dir)
{
    auto subjects = loadFixtures (dir);
    int controls = 0, rangeBad = 0, dirBad = 0, unitContradicts = 0, unitFromText = 0, unitNeedsLabel = 0, unitNone = 0;
    juce::StringArray bad;
    for (const auto& s : subjects)
        if (auto* arr = s.pushed.getProperty ("controls", juce::var()).getArray())
            for (const auto& c : *arr)
            {
                ++controls;
                const auto da = c.getProperty ("displayAt", juce::var());
                const auto a0 = da.getProperty ("0.000", "").toString(), a5 = da.getProperty ("0.500", "").toString(),
                           a1 = da.getProperty ("1.000", "").toString();
                const auto d = fixturerange::derive (a0, a5, a1);
                juce::StringArray rd; diffVar (d.range, c.getProperty ("range", juce::var()), "range", rd);
                if (! rd.isEmpty()) { ++rangeBad; bad.add (s.product + " / " + c.getProperty ("name", "").toString() + ": " + rd.joinIntoString ("; ")); }
                if (d.direction != c.getProperty ("direction", "").toString())
                { ++dirBad; bad.add (s.product + " / " + c.getProperty ("name", "").toString() + ": direction " + d.direction
                                     + " vs " + c.getProperty ("direction", "").toString()); }
                const auto fromText = fixtureunit::unitFor ({}, a0, a1);
                const auto u = c.getProperty ("unit", juce::var());
                if (u.isVoid())                    { ++unitNone; if (fromText.isNotEmpty()) { ++unitContradicts;
                                                        bad.add (s.product + " / " + c.getProperty ("name", "").toString()
                                                                 + ": text gives unit '" + fromText + "' but the fixture has none"); } }
                else if (u.toString() == fromText) ++unitFromText;
                else                               ++unitNeedsLabel;
            }
    std::cout << "CERT REDERIVE: " << subjects.size() << " fixtures, " << controls << " controls, from each fixture's own displayAt\n"
              << "  range      " << (controls - rangeBad) << " / " << controls << " reproduced\n"
              << "  direction  " << (controls - dirBad) << " / " << controls << " reproduced\n"
              << "  unit       " << unitNone << " with no unit, of which " << unitContradicts << " contradicted by the text;\n"
              << "             " << unitFromText << " explained by the text, " << unitNeedsLabel
              << " that must come from a parameter label (not checkable without the plugin)\n";
    for (const auto& b : bad) std::cout << "  DIFF " << b << "\n";
    std::cout << (rangeBad + dirBad + unitContradicts == 0 ? "CERT REDERIVE: GREEN" : "CERT REDERIVE: RED") << std::endl;
    return rangeBad + dirBad + unitContradicts == 0 ? 0 : 1;
}

//==============================================================================
// MODE 2: --cert-defaults. Regenerate every reachable fixture and compare it.
struct Options
{
    juce::File fixtures, probe, out, entitlements;
    juce::String signIdentity;
    int timeoutMs = 90000;
    bool includePace = false;
};

inline int runCertDefaults (const Options& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    if (! opt.fixtures.isDirectory()) { say ("CERT: no fixtures directory at " + opt.fixtures.getFullPathName()); return 2; }
    if (! opt.out.createDirectory()) { say ("CERT: cannot create " + opt.out.getFullPathName()); return 2; }

    // 1. THE SIGNATURE, once, before anything is touched.
    const auto id = checkProbe (opt.probe, opt.signIdentity, opt.entitlements);
    if (! id.ok) { say ("CERT: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const juce::String probeLabel = "signed EchoJayProbe (feat/ejmap-cert), team " + id.team + ", cdhash " + id.cdhash;
    const juce::String date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");

    // 2. THE WORKLIST and its reachability.
    auto subjects = loadFixtures (opt.fixtures);
    classify (subjects, opt.includePace);

    auto ledger = opt.out.getChildFile ("run.jsonl");
    ledger.deleteFile();
    auto logAttempt = [&] (const Subject& s, const juce::String& mode, int attempt, const ChildResult& r)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("product", s.product); o->setProperty ("fixture", s.fixtureFile.getFileName());
        o->setProperty ("mode", mode); o->setProperty ("attempt", attempt);
        o->setProperty ("outcome", r.describe()); o->setProperty ("ms", r.ms);
        ledger.appendText (juce::JSON::toString (juce::var (o), true) + "\n");
    };
    // THE RETRY RULE: anything but a clean exit is re-run ONCE, alone. The second
    // result stands. Two timeouts mark a hang.
    auto runProbe = [&] (const Subject& s, const juce::String& mode, const juce::StringArray& extra, bool& retried)
    {
        juce::StringArray args { opt.probe.getFullPathName(), s.desc.name, s.desc.fileOrIdentifier,
                                 juce::String::toHexString (s.desc.uniqueId) };
        args.addArray (extra);
        // The probe's raw output is kept for every attempt, so a difference can be
        // traced to what the plugin printed without re-running it.
        auto keep = [&] (int attempt, const ChildResult& r) {
            opt.out.getChildFile ("raw").getChildFile (s.fixtureFile.getFileNameWithoutExtension() + "."
                + mode.retainCharacters ("abcdefghijklmnopqrstuvwxyz-") + "." + juce::String (attempt) + ".txt")
               .replaceWithText (r.out); };
        opt.out.getChildFile ("raw").createDirectory();
        auto r = runChild (args, opt.timeoutMs);
        logAttempt (s, mode, 1, r); keep (1, r);
        // Not retried: a refusal (exit 3) is the plugin's answer, and a window is a
        // licence or activation event - a retry would only put it on the desktop again.
        if (! r.cleanExit() && ! (r.kind == ChildResult::Kind::exited && r.code == 3)
            && r.kind != ChildResult::Kind::uiShown)
        {
            retried = true;
            r = runChild (args, opt.timeoutMs);
            logAttempt (s, mode, 2, r); keep (2, r);
        }
        return r;
    };

    // READOUT DETECTION (schema change, item 12: shape AGREED 28 Sep with a null default -
    // a meter has no instantiate value, so null is the truthful encoding - but NOT yet
    // emitted by composeFixture; that is its own change).
    // A control whose value differs between two FRESH instances is a readout, and its
    // defaultOnInstantiate is meaningless: one sample of something moving. Found on
    // Shadow Hills Class A's "VU Meter R" (five instances, five values). It is a
    // derivation from two measurements, so it lives here and the probe is unchanged.
    // Sufficient, not necessary: a meter that happens to read the same twice (a gain
    // reduction meter sitting at 0 in silence) is NOT caught.
    struct Readout { int index; juce::String name; double a, b; juce::String ta, tb; };
    struct Row { const Subject* s; juce::String outcome; juce::StringArray diffs, diffsOnReadouts, emissionFails;
                 bool reproduced = false, retried = false, readoutChecked = false, emitted = false;
                 int captured = 0, onList = 0; std::vector<Readout> readouts; juce::String readoutNote; };
    std::vector<Row> rows;
    for (const auto& s : subjects)
    {
        if (s.reach != Subject::Reach::reachable && s.reach != Subject::Reach::reachableNoVersion) continue;
        Row row { &s };
        std::cout << "  probing " << s.product << " ..." << std::flush;
        auto lp = runProbe (s, "--list-params", { "--list-params" }, row.retried);
        auto ta = lp.cleanExit() ? runProbe (s, "--text-at all", { "--text-at", "all" }, row.retried) : ChildResult();
        if (! lp.cleanExit()) row.outcome = "NOT REPRODUCED: --list-params " + lp.describe();
        else if (! ta.cleanExit()) row.outcome = "NOT REPRODUCED: --text-at all " + ta.describe();
        else
        {
            const auto list = parseListParams (lp.out);
            const auto text = parseTextAt (ta.out);
            for (const auto& kv : list) if (kv.second.automatable && ! kv.second.meta) ++row.onList;
            row.captured = (int) text.size();

            // THE SECOND INSTANCE. One extra instantiation per product.
            auto tb = runProbe (s, "--text-at all (second instance)", { "--text-at", "all" }, row.retried);
            std::map<int, int> positionOf;                      // control index -> position in controls[]
            for (size_t i = 0; i < text.size(); ++i) positionOf[text[i].index] = (int) i;
            std::set<int> readoutPositions;
            if (! tb.cleanExit()) row.readoutNote = "readout check incomplete: second instance " + tb.describe();
            else
            {
                const auto second = parseTextAt (tb.out);
                std::map<int, const TextAtRow*> byIndex;
                for (const auto& t : second) byIndex[t.index] = &t;
                row.readoutChecked = true;
                for (const auto& t : text)
                {
                    auto it = byIndex.find (t.index);
                    if (it == byIndex.end()) { row.readoutNote = "second instance lacks control " + juce::String (t.index); row.readoutChecked = false; break; }
                    if (it->second->defNorm != t.defNorm || it->second->defText != t.defText)
                    {
                        row.readouts.push_back ({ t.index, t.name, t.defNorm, it->second->defNorm, t.defText, it->second->defText });
                        readoutPositions.insert (positionOf[t.index]);
                    }
                }
            }

            // TWO FORMS, TWO QUESTIONS. `regen` is the MEASURED form, composed exactly as
            // before item 12; it is what the reproduction score compares, so that score stays
            // comparable to the 14 of 15 measured before the schema change and never contains
            // the new fields. `emittedFixture` is item 12's shape; it is what gets written, and
            // it is checked separately by checkEmission. Never merge the two.
            const auto regen = composeFixture (s, list, text, lp.code, ta.code, probeLabel, date);
            std::vector<fixturereadout::Moved> moved;
            for (const auto& ro : row.readouts) moved.push_back ({ ro.index, ro.a, ro.b, ro.ta, ro.tb });
            const auto emittedFixture = fixturereadout::applySchema (regen, moved, row.readoutChecked);
            opt.out.getChildFile (s.fixtureFile.getFileName()).replaceWithText (juce::JSON::toString (emittedFixture, false));
            row.emissionFails = fixturereadout::checkEmission (emittedFixture, regen, moved, row.readoutChecked);
            row.emitted = row.readoutChecked && row.emissionFails.isEmpty();
            for (const auto& d : compareFixtures (regen, s.pushed))
            {
                // A difference in a DETECTED readout's defaultOnInstantiate is the known,
                // recorded condition, reported apart from real differences.
                const int pos = d.startsWith (".controls[") ? d.fromFirstOccurrenceOf ("[", false, false).getIntValue() : -1;
                const bool onReadoutDefault = pos >= 0 && readoutPositions.count (pos) > 0
                                              && d.contains ("].defaultOnInstantiate.");
                (onReadoutDefault ? row.diffsOnReadouts : row.diffs).add (d);
            }
            row.reproduced = row.diffs.isEmpty() && row.diffsOnReadouts.isEmpty();
            row.outcome = row.reproduced ? "REPRODUCED"
                        : row.diffs.isEmpty() ? "DIFFERS ONLY on " + juce::String (row.diffsOnReadouts.size())
                                                + " detected-readout default(s)"
                                              : "DIFFERS in " + juce::String (row.diffs.size()) + " field(s)";
        }
        std::cout << " " << row.outcome << std::endl;
        rows.push_back (row);
    }

    // The readouts, in a SIDECAR. Not written into any fixture until the schema field
    // is agreed (it is item 12 on the mismatch list).
    {
        juce::Array<juce::var> side;
        for (const auto& r : rows)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("fixture", r.s->fixtureFile.getFileName());
            o->setProperty ("product", r.s->product);
            o->setProperty ("readoutCheck", r.readoutChecked ? juce::var ("instantiate_twice") : juce::var());
            if (r.readoutNote.isNotEmpty()) o->setProperty ("note", r.readoutNote);
            juce::Array<juce::var> list;
            for (const auto& ro : r.readouts)
            {
                auto* x = new juce::DynamicObject();
                x->setProperty ("index", ro.index); x->setProperty ("name", ro.name);
                x->setProperty ("samples", juce::Array<juce::var> { ro.a, ro.b });
                x->setProperty ("displays", juce::Array<juce::var> { ro.ta, ro.tb });
                list.add (juce::var (x));
            }
            o->setProperty ("readouts", list);
            side.add (juce::var (o));
        }
        opt.out.getChildFile ("readouts.json").replaceWithText (juce::JSON::toString (juce::var (side), false));
    }

    // 3. THE REPORT. Coverage first, always.
    std::map<Subject::Reach, juce::StringArray> byReach;
    for (const auto& s : subjects) byReach[s.reach].add (s.product + (s.detail.isNotEmpty() ? "  (" + s.detail + ")" : ""));
    const int reachable = (int) rows.size();
    int reproduced = 0, differs = 0, onlyReadouts = 0, failed = 0, readoutControls = 0, readoutChecked = 0, emitted = 0;
    for (const auto& r : rows)
    {
        if (r.reproduced) ++reproduced;
        else if (! r.diffs.isEmpty()) ++differs;
        else if (! r.diffsOnReadouts.isEmpty()) ++onlyReadouts;
        else ++failed;
        readoutControls += (int) r.readouts.size();
        if (r.readoutChecked) ++readoutChecked;
        if (r.emitted) ++emitted;
    }

    juce::String rep;
    rep << "EJ MAP CERT DRIVER - regenerate compressor-profile fixtures - " << juce::Time::getCurrentTime().toISO8601 (true) << "\n"
        << "probe     " << probeLabel << "\n"
        << "fixtures  " << opt.fixtures.getFullPathName() << "\n\n"
        << "COVERAGE (read this first)\n"
        << "  " << juce::String ("fixtures").paddedRight (' ', 58) << (int) subjects.size() << "\n";
    for (auto r : { Subject::Reach::reachable, Subject::Reach::reachableNoVersion, Subject::Reach::heldPace,
                    Subject::Reach::versionMismatch, Subject::Reach::notInstalled, Subject::Reach::ambiguous })
        rep << "  " << reachName (r).paddedRight (' ', 58) << byReach[r].size() << "\n";
    // TWO LINES, NEVER ONE: they answer different questions.
    rep << "\nREPRODUCTION (the MEASURED form vs the pushed fixture; item-12 fields are not in it): reproduced "
        << reproduced << " of " << reachable << " reachable, of " << (int) subjects.size() << " fixtures"
        << "  |  differ only on detected readouts " << onlyReadouts << "  |  differ " << differs
        << "  |  not reproduced (named) " << failed << "\n"
        << "SCHEMA EMISSION (item 12, checked separately, C1-C5): emitted the readout fields correctly on "
        << emitted << " of " << reachable << " reachable\n"
        << "READOUTS: checked on " << readoutChecked << " of " << reachable << " (two fresh instances each); "
        << readoutControls << " control(s) moved between instances (also in readouts.json)\n\n"
        << "PER REACHABLE PRODUCT (reproduction | emission)\n";
    for (const auto& r : rows)
    {
        rep << "  " << r.s->product.paddedRight (' ', 38) << r.outcome
            << "  |  emission " << (r.emitted ? juce::String ("OK")
                                              : r.readoutChecked ? "FAILED" : "not checked: " + r.readoutNote)
            << (r.onList > 0 ? "   controls " + juce::String (r.captured) + " / " + juce::String (r.onList) : juce::String())
            << (r.retried ? "   RETRIED" : "") << "\n";
        for (const auto& f : r.emissionFails) rep << "      EMISSION " << f << "\n";
        for (const auto& d : r.diffs) rep << "      " << d << "\n";
        for (const auto& d : r.diffsOnReadouts) rep << "      " << d << "   (on a detected readout)\n";
        for (const auto& ro : r.readouts)
            rep << "      readout [" << ro.index << "] " << ro.name << ": " << juce::String (ro.a, 6) << " then "
                << juce::String (ro.b, 6) << " on instantiate\n";
        if (r.readoutNote.isNotEmpty()) rep << "      " << r.readoutNote << "\n";
    }
    rep << "\nOUT OF REACH OR HELD, BY NAME\n";
    for (auto r : { Subject::Reach::heldPace, Subject::Reach::versionMismatch, Subject::Reach::notInstalled, Subject::Reach::ambiguous })
        if (! byReach[r].isEmpty())
        {
            rep << "  " << reachName (r) << " (" << byReach[r].size() << ")\n";
            for (const auto& n : byReach[r]) rep << "      " << n << "\n";
        }
    rep << "\nThe acceptance criterion is docs/EJMAP_CERT_DRIVER.md section 0. A product that did not answer counts as NOT\n"
           "reproduced and is never a pass. Every attempt is in run.jsonl beside this report.\n";
    opt.out.getChildFile ("report.txt").replaceWithText (rep);
    std::cout << "\n" << rep << std::endl;
    return (differs == 0 && onlyReadouts == 0 && failed == 0 && emitted == reachable) ? 0 : 1;
}

//==============================================================================
// ONE PROBE INVOCATION through the shipped machinery (--cert-probe-once): the same
// signature gate, the same runChild, the default PRODUCTION window watch, and NO
// retry. It exists for the step-2 question, which is one plugin and one mode, not a
// fixture. A window is a result here, exactly as in --cert-defaults.
inline int runProbeOnce (const juce::File& probe, const juce::StringArray& probeArgs, int timeoutMs)
{
    const auto id = checkProbe (probe, {}, {});
    if (! id.ok) { std::cout << "PROBE-ONCE: ABORTED BEFORE ANY PLUGIN - " << id.why << std::endl; return 3; }
    std::cout << "probe: team " << id.team << ", cdhash " << id.cdhash << std::endl;
    juce::StringArray args { probe.getFullPathName() };
    args.addArray (probeArgs);
    const auto r = runChild (args, timeoutMs);
    const auto rows = parseListParams (r.out);
    std::cout << "outcome: " << r.describe() << "\n"
              << "windows in the probe's tree: " << (r.windowsInTree.isEmpty() ? juce::String ("none") : r.windowsInTree.joinIntoString (", ")) << "\n"
              << "parameter rows: " << (int) rows.size() << "\n"
              << "---- probe output ----\n" << r.out << "---- end ----" << std::endl;
    return r.cleanExit() ? 0 : 1;
}

//==============================================================================
// SELF-TEST OF THE WINDOW WATCH (--cert-watch-selftest <helper>). The helper orders
// in one 10x10 borderless window 20,000 px off every display and waits 20 s. It is
// invisible to the user, but CoreGraphics lists it as ON SCREEN (measured 28 Sep),
// so the test runs the PRODUCTION setting. Three cases:
//   - the helper as the direct child: caught, killed at once, not a timeout
//   - the helper as a GRANDCHILD (via /bin/sh, forced to fork): PACE's activation
//     UI is a child of the probe, one level below what the driver spawns
//   - a child that shows no window: must exit cleanly, untouched
inline int runWatchSelfTest (const juce::File& helper)
{
    auto direct = runChild ({ helper.getFullPathName() }, 15000);
    std::cout << "direct child:     " << direct.describe() << std::endl;
    auto grand = runChild ({ "/bin/sh", "-c", "'" + helper.getFullPathName() + "'; true" }, 15000);
    std::cout << "grandchild:       " << grand.describe() << std::endl;
    auto quiet = runChild ({ "/bin/sleep", "1" }, 15000);
    std::cout << "no window (ctrl): " << quiet.describe() << std::endl;
    const bool ok = direct.kind == ChildResult::Kind::uiShown && direct.ms < 5000.0
                 && grand.kind == ChildResult::Kind::uiShown && grand.ms < 5000.0 && quiet.cleanExit();
    std::cout << (ok ? "WATCH SELFTEST: GREEN" : "WATCH SELFTEST: RED") << std::endl;
    return ok ? 0 : 1;
}

} // namespace ejmap::cert
