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
#include "EjmapCertOutcome.h"
#include "EjmapSweep.h"
#include "EjmapPitch.h"
#include "EjmapProfileExport.h"
#include "EchoJayParamMaps.h"   // fingerprintForDescription: the join key, one function for all three sides

#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach/mach_time.h>
#include <functional>
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
    enum class Kind { exited, signaled, timedOut, uiShown, spawnFailed, sleptTwice } kind = Kind::spawnFailed;
    int code = 0;             // exit code when exited
    int signal = 0;           // terminating signal when signaled
    juce::String out;         // stdout and stderr, interleaved
    juce::StringArray windowsInTree;   // owner names of on-screen windows the child's tree showed
    double ms = 0.0;                   // AWAKE time the child ran for (the clock every timeout is measured on)
    double sleptMs = 0.0;              // continuous minus awake time over the run: > 0 means the Mac slept during it

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
            case Kind::sleptTwice:  return "the Mac slept during both attempts: refused (a measurement across a wake is not trusted)";
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

//==============================================================================
// SLEEP (ruled 29 Sep; spec section 6: unattended runs happen on laptops).
//
// THE CLOCK. Every timeout is measured on AWAKE time, so a Mac that sleeps mid-run never turns slept time into a
// "hang". Measured 29 Sep on this Mac: mach_absolute_time (JUCE's getMillisecondCounterHiRes) read 518,794 s against
// 1,031,532 s of wall time since boot - it excludes the 5.9 days slept. runChild already timed out on that clock, so
// batch 2's sleeps could not fake a timeout; this makes it explicit, injectable and PINNED instead of a property of
// JUCE's implementation. A second clock that DOES count sleep (mach_continuous_time: not moved by NTP or a user
// changing the time, unlike wall time) is read beside it to measure how long the Mac slept during each process.
inline double continuousMs()
{
    static const double ratio = [] { mach_timebase_info_data_t tb; mach_timebase_info (&tb); return (double) tb.numer / (double) tb.denom / 1.0e6; }();
    return (double) mach_continuous_time() * ratio;
}
struct Clock
{
    std::function<double()> awakeMs     = [] { return juce::Time::getMillisecondCounterHiRes(); };
    std::function<double()> withSleepMs = [] { return continuousMs(); };
};

// A process that slept longer than this during its run is not trusted (ruled 29 Sep): a measurement across a wake
// can be corrupt AND plausible - RCompressor (s) position 5 read its ratio as 0 through a bridge that died across a
// dark wake, and rendered 2.1 M no-op blocks inside 500 ms. It is re-run ONCE; slept again, it is refused, never looped.
inline constexpr double kSleptMs = 1000.0;

// THE ASSERTION. Held for the whole of a run: no system sleep (honoured on AC power, including with the lid closed) and
// no idle sleep. It CANNOT stop a lid-close on battery - batch 2's sleep (29 Sep, 18:36) was exactly that, "Clamshell
// Sleep" on 9% battery, with idle sleep already prevented system-wide - which is why the awake clock above matters.
struct SleepGuard
{
    IOPMAssertionID system = 0, idle = 0;
    bool systemHeld = false, idleHeld = false;
    explicit SleepGuard (const juce::String& why)
    {
        const auto reason = CFStringCreateWithCString (kCFAllocatorDefault, why.toRawUTF8(), kCFStringEncodingUTF8);
        systemHeld = IOPMAssertionCreateWithName (kIOPMAssertionTypePreventSystemSleep, kIOPMAssertionLevelOn, reason, &system) == kIOReturnSuccess;
        idleHeld   = IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn, reason, &idle) == kIOReturnSuccess;
        CFRelease (reason);
    }
    ~SleepGuard()
    {
        if (systemHeld) IOPMAssertionRelease (system);
        if (idleHeld) IOPMAssertionRelease (idle);
    }
    juce::String describe() const
    {
        return juce::String ("sleep assertions: system ") + (systemHeld ? "held" : "REFUSED") + ", idle " + (idleHeld ? "held" : "REFUSED")
               + " (a lid-close on battery sleeps regardless; timeouts count awake time only)";
    }
};

// THE TIMEOUT ARITHMETIC, alone so it can be pinned: elapsed is AWAKE time.
inline bool timeoutPassed (double startAwakeMs, double nowAwakeMs, int timeoutMs) { return nowAwakeMs - startAwakeMs > timeoutMs; }

inline ChildResult runChild (const juce::StringArray& args, int timeoutMs, WatchOptions watch = {}, const Clock& clock = {})
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
    const double t0 = clock.awakeMs(), w0 = clock.withSleepMs();
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
        const double now = clock.awakeMs();
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
        if (timeoutPassed (t0, now, timeoutMs))
        {
            kill (pid, SIGKILL);
            waitpid (pid, &status, 0);
            killed = reaped = true;
            drain();
            break;
        }
    }
    close (fds[0]);
    r.ms  = clock.awakeMs() - t0;
    r.sleptMs = juce::jmax (0.0, (clock.withSleepMs() - w0) - r.ms);
    r.out = collected.toString();
    if (uiKilled)                r.kind = ChildResult::Kind::uiShown;
    else if (killed)             r.kind = ChildResult::Kind::timedOut;
    else if (WIFEXITED (status))   { r.kind = ChildResult::Kind::exited;   r.code = WEXITSTATUS (status); }
    else if (WIFSIGNALED (status)) { r.kind = ChildResult::Kind::signaled; r.signal = WTERMSIG (status); }
    return r;
}

//==============================================================================
// THE SIGNATURE GATE. Sign once (optional) under a deadline, then verify.
// THE RETRY RULE, ONE IMPLEMENTATION for every probe the driver runs (--cert-defaults and --cert-sweep alike).
//   - a refusal (exit 3) and a window are ANSWERS: never retried;
//   - anything else unclean is re-run ONCE (the SIGTERM rule: a killed probe is not a refusal);
//   - a clean run during which the Mac SLEPT is re-run ONCE too (ruled 29 Sep), because its numbers may be corrupt and
//     still look right; slept AGAIN, it is refused (`sleptTwice`) - recorded, never looped.
// `onAttempt` sees every attempt, so every attempt's output is kept.
struct Attempted { ChildResult r; int attempts = 0; bool sleptTwice = false; };
inline bool isAnswer (const ChildResult& r) { return (r.kind == ChildResult::Kind::exited && r.code == 3) || r.kind == ChildResult::Kind::uiShown; }
inline Attempted runWithRetry (const juce::StringArray& args, int timeoutMs, const std::function<void (int, const ChildResult&)>& onAttempt,
                               const Clock& clock = {}, WatchOptions watch = {})
{
    Attempted a;
    for (int attempt = 1; attempt <= 2; ++attempt)
    {
        a.r = runChild (args, timeoutMs, watch, clock);
        a.attempts = attempt;
        if (onAttempt) onAttempt (attempt, a.r);
        const bool slept = a.r.sleptMs > kSleptMs;
        if (isAnswer (a.r) || (a.r.cleanExit() && ! slept)) return a;
        if (attempt == 2) a.sleptTwice = slept;
    }
    return a;
}

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
    juce::String category = "compressor";   // "compressor" | "pitch": which certification the subject gets (ONE STORE, ruled 1 Oct)
    enum class Reach { reachable, reachableNoVersion, heldPace, requiresHardware, versionMismatch, notInstalled, ambiguous, unfixtured } reach
        = Reach::notInstalled;
    juce::String detail;                 // installed version(s), the ambiguity, ...
    juce::PluginDescription desc;        // resolved component, when installed
    bool licenceBound = false;           // PACE-wrapped, or its PACE state could not be checked
    // versionMismatch only: the ONE installed version, resolved for MEASUREMENT (see measurable()).
    bool installedUnique = false, hardware = false;
};

// THE VERSION GUARD BELONGS ON COMPARISON, NOT ON MEASUREMENT (ruled 29 Sep). A fixture made at another
// version cannot be REPRODUCED here, so --cert-defaults still skips it; but a fresh measurement at the
// installed version is valid and writes a NEW identity. Section 6 runs EJ Map on other people's Macs, where
// a version nobody has sampled is the normal case, not a workaround. Measurable when:
//   - reachable, with or without a recorded version; or
//   - installed at another version, exactly ONE version installed (several: never pick one), not a
//     hardware product, and not licence-bound unless PACE is included.
inline bool measurable (const Subject& s, bool includePace)
{
    if (s.reach == Subject::Reach::reachable || s.reach == Subject::Reach::reachableNoVersion) return true;
    if (s.reach == Subject::Reach::unfixtured) return ! s.hardware && (! s.licenceBound || includePace);
    return s.reach == Subject::Reach::versionMismatch && s.installedUnique && ! s.hardware
           && (! s.licenceBound || includePace);
}

// PLUGINS THAT NEED EXTERNAL HARDWARE (logged 29 Sep, docs/EJMAP_CERT_DRIVER.md
// section 3): they may enumerate, but they render nothing without the hardware, so they
// are RECORDED, never measured and never scored. The only known members so far are McDSP's
// APB plugins, which need the Analog Processing Box (per Kathy). The rule is by
// manufacturer code and product name, not by guess. Add to it only on evidence.
//
// UAD-2 joins it (ruled 29 Sep): every UAD-2 product (manufacturer code "!UAD") opened its own dialog at the FIRST RENDER
// on this Mac, which has no UAD hardware attached; defaults sampling, which never renders, was clean. Consistent with
// needing the hardware, not proven - the window watch cannot read titles. All 215 UAD components installed here are
// com.uaudio.effects with "!UAD"; none is a UADx (native) build, which would not need hardware.
// The category is CONDITIONAL, not uncertifiable: attaching the hardware makes it one re-run.
inline juce::String externalHardwareNeeded (const juce::String& product, const juce::String& componentCode)
{
    if (componentCode.endsWith (",McDP") && product.startsWith ("APB ")) return "McDSP APB hardware";
    if (componentCode.endsWith (",!UAD")) return "UAD hardware (UAD-2)";
    return {};
}
inline bool requiresExternalHardware (const juce::String& product, const juce::String& componentCode)
{
    return externalHardwareNeeded (product, componentCode).isNotEmpty();
}

inline juce::String reachName (Subject::Reach r)
{
    switch (r)
    {
        case Subject::Reach::reachable:          return "reachable";
        case Subject::Reach::reachableNoVersion: return "reachable (fixture records no version)";
        case Subject::Reach::heldPace:           return "held: PACE-wrapped or unverifiable (run with --include-pace)";
        case Subject::Reach::requiresHardware:   return "not measured: requires external hardware";
        case Subject::Reach::versionMismatch:    return "out of reach: installed at another version";
        case Subject::Reach::notInstalled:       return "out of reach: not installed";
        case Subject::Reach::ambiguous:          return "out of reach: uid matches more than one component";
        case Subject::Reach::unfixtured:         return "discovered: mapped, no fixture at its installed version";
    }
    return "?";
}

// WHAT COUNTS AS A RECORD (ruled 30 Sep). A fixture leaves the worklist when it records what happened to the product:
// a threshold sweep, every candidate's sweep (no thresholdSweep by design, a human picks), or a REFUSAL - the product
// was stopped before a usable measurement and the fixture says at which stage and why. Before this rule only a
// thresholdSweep counted, so a candidates fixture went straight back on the list, and a product stopped in the defaults
// phase wrote nothing and was rediscovered on every batch, forever.
// ONE STORE (ruled 1 Oct): compressor and tuner records live in the same directory, discriminated by the record's own
// `schema` field - the store is where records live, the schema is what a record says. Sean's ej_comp_profile/1 is the
// server-side projection and is unchanged by this; these are the certification records it is built from.
using sweep::kSchemaCompressor;
using sweep::kSchemaTuner;
inline bool sweepRecorded (const juce::var& fixture)
{
    return fixture.getProperty ("thresholdSweep", {}).isObject()
        || fixture.getProperty ("thresholdCandidates", {}).isArray()
        || fixture.getProperty ("pitchCandidates", {}).isArray()
        || fixture.getProperty ("thresholdRefusal", {}).isObject();
}
inline bool refusalRecorded (const juce::var& fixture) { return fixture.getProperty ("thresholdRefusal", {}).isObject(); }

// TRANSIENT OR PERMANENT (ruled 30 Sep). A refusal at these stages is a fact about the PRODUCT and a re-run cannot change
// it: the plan found no threshold role, or the ratio never reads 4:1. Everything else is a fact about the RUN - a hang,
// a window (the iLok was out), the budget, a sleep, a silent reference - and a re-run after fixing it is the point.
// --retry-refused re-runs the transient ones; retrying the permanent ones indiscriminately would re-run the
// uncertifiable on every batch, the very jam the record exists to escape. --retry-refused-all overrides.
inline bool refusalIsPermanent (const juce::String& stage) { return stage == "plan" || stage == "ratio_none" || stage == "ara_only"; }

// A RETRIED REFUSAL MAY HAVE NO CONTROLS (found 30 Sep on SSL G3's first --retry-refused): a record written in the
// defaults phase is the discovered identity and nothing else. Planned from as a fixture, it reads "0 controls hold
// the threshold role" - a permanent refusal manufactured from an empty record. So defaults are sampled first for an
// unseen version, a discovered product, AND any subject whose record carries no controls.
inline bool needsDefaultsFirst (const Subject& s)
{
    return s.reach == Subject::Reach::versionMismatch || s.reach == Subject::Reach::unfixtured
        || ! s.pushed.getProperty ("controls", {}).isArray();
}
inline bool refusalPermanent (const juce::var& fixture)
{
    return refusalRecorded (fixture) && refusalIsPermanent (fixture.getProperty ("thresholdRefusal", {}).getProperty ("stage", "").toString());
}

inline std::vector<Subject> loadFixtures (const juce::File& dir)
{
    std::vector<Subject> out;
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.json");
    files.sort();
    for (const auto& f : files)
    {
        // The defaults SIDECAR (<stem>.defaults.json) is the sample a sweep was planned on, kept beside the fixture; it is
        // not a subject. Loaded as one it carried the same uid with no sweep, and re-entered the worklist every run.
        if (f.getFileName().endsWith (".defaults.json")) continue;
        Subject s;
        s.fixtureFile = f;
        s.pushed = juce::JSON::parse (f.loadFileAsString());
        if (! s.pushed.isObject()) continue;
        s.product = s.pushed.getProperty ("product", "").toString();
        s.uid     = s.pushed.getProperty ("uid", "").toString().toLowerCase();
        s.version = s.pushed.getProperty ("version", "").toString();
        if (s.pushed.getProperty ("category", "").toString() == "pitch" || s.pushed.getProperty ("schema", "").toString() == kSchemaTuner) s.category = "pitch";
        out.push_back (s);
    }
    return out;
}

// THE HOLD FAILS SAFE. First version: "no bundle found" meant "not PACE", and the two McDSP APB compressors ran while
// step 2 was open - PACE-wrapped (Eden bundle and PACE bytes both present), but registered without an AudioComponents
// key, so no plist scan finds them. A component whose PACE state cannot be CHECKED is held, and says so. Apple's own
// built-ins have no bundle here and are exempt. One rule, used by classify and by discovery.
inline bool paceHeld (const juce::PluginDescription& d, const std::map<juce::String, juce::File>& bundles, juce::String& why)
{
    const auto code = d.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false);
    auto it = bundles.find (code);
    if (it != bundles.end())
    {
        if (isPaceWrapped (it->second)) { why = "PACE-wrapped (" + it->second.getFileName() + ")"; return true; }
        return false;
    }
    if (code.endsWith (",appl")) return false;      // macOS built-in, no bundle to check
    why = "no bundle found for " + code + ", so its PACE state cannot be checked: held, not assumed clear";
    return true;
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
    auto isPace = [&bundles] (const juce::PluginDescription& d, juce::String& why) { return paceHeld (d, bundles, why); };

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
            if (requiresExternalHardware (s.product, s.version))
            { s.reach = Subject::Reach::requiresHardware; s.detail = "needs " + externalHardwareNeeded (s.product, s.version) + " present; recorded, not measured - attaching it makes this one re-run"; continue; }
            juce::String why;
            s.licenceBound = isPace (d, why);
            const bool held = ! includePace && s.licenceBound;
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
        {
            s.reach = Subject::Reach::versionMismatch;
            s.detail = "fixture " + s.version + ", installed " + versions.joinIntoString (" / ");
            juce::Array<juce::PluginDescription> installed;
            for (auto [it, end] = byUid.equal_range (s.uid); it != end; ++it) installed.add (it->second);
            if (installed.size() == 1)
            {
                s.desc = installed.getReference (0);
                s.installedUnique = true;
                s.hardware = requiresExternalHardware (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false));
                if (s.hardware) s.detail << "; needs " << externalHardwareNeeded (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)) << " present";
                juce::String why;
                s.licenceBound = isPace (s.desc, why);
            }
            continue;
        }
        if (atVersion.size() > 1)
        {
            s.reach = Subject::Reach::ambiguous;
            for (const auto& d : atVersion) s.detail << d.fileOrIdentifier << " ";
            continue;                                   // never pick one
        }
        s.desc = atVersion.getReference (0);
        if (requiresExternalHardware (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)))
        { s.reach = Subject::Reach::requiresHardware; s.detail = "needs " + externalHardwareNeeded (s.product, s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)) + " present; recorded, not measured - attaching it makes this one re-run"; continue; }
        juce::String why;
        s.licenceBound = isPace (s.desc, why);
        const bool held = ! includePace && s.licenceBound;
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

// READOUT DETECTION: a control whose value differs between two FRESH instances is a readout (item 12).
// One function, used by --cert-defaults and by the sweep's defaults-first pass for an unseen version.
struct ReadoutHit { int index; juce::String name; double a, b; juce::String ta, tb; };
struct ReadoutScan { bool checked = false; juce::String note; std::vector<ReadoutHit> hits; std::set<int> positions; };
inline ReadoutScan scanReadouts (const std::vector<TextAtRow>& text, const ChildResult& second)
{
    ReadoutScan r;
    std::map<int, int> positionOf;                      // control index -> position in controls[]
    for (size_t i = 0; i < text.size(); ++i) positionOf[text[i].index] = (int) i;
    if (! second.cleanExit()) { r.note = "readout check incomplete: second instance " + second.describe(); return r; }
    const auto again = parseTextAt (second.out);
    std::map<int, const TextAtRow*> byIndex;
    for (const auto& t : again) byIndex[t.index] = &t;
    r.checked = true;
    for (const auto& t : text)
    {
        auto it = byIndex.find (t.index);
        if (it == byIndex.end()) { r.note = "second instance lacks control " + juce::String (t.index); r.checked = false; break; }
        if (it->second->defNorm != t.defNorm || it->second->defText != t.defText)
        {
            r.hits.push_back ({ t.index, t.name, t.defNorm, it->second->defNorm, t.defText, it->second->defText });
            r.positions.insert (positionOf[t.index]);
        }
    }
    return r;
}

//==============================================================================
// THE FIXTURE, composed from probe output through the committed derivations.
inline const char* kProfileNote  = "PROFILE ONLY - nothing certified, nothing written to the plugin.";
inline const char* kDefaultsNote = "the value the plugin instantiated with; declaredDefault is what the plugin calls "
                                   "its default - they are not always the same";

inline juce::var composeFixture (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                 int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date);
inline juce::var composeFixtureImpl (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                     int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date);
inline juce::var composeFixture (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
                                 int listRc, int textAtRc, const juce::String& probeLabel, const juce::String& date)
{
    auto f = composeFixtureImpl (s, list, textAt, listRc, textAtRc, probeLabel, date);
    if (auto* o = f.getDynamicObject()) o->setProperty ("schema", s.category == "pitch" ? kSchemaTuner : kSchemaCompressor);
    return f;
}
inline juce::var composeFixtureImpl (const Subject& s, const std::map<int, ListRow>& list, const std::vector<TextAtRow>& textAt,
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
    // THE JOIN KEY (ruled 1 Oct, the highest-stakes small fix): map_fp = SHA-256(format|uidHex|version|param_count) by the
    // ONE shared function EchoJay and EJ Map's mapper use, with param_count from the probe's --list-params rows - the
    // instance's getParameters().size() - and NEVER from controls.length: `controls` is built from the text-at rows and a
    // parameter the text pass skipped is not a control (NEOLD U2A: 13 rows, 11 controls; 5 of 92 fixtures hashed wrong
    // and would have failed the join SILENTLY). Reproduced against every local map: pin M1.
    o->setProperty ("param_count", (int) list.size());
    o->setProperty ("map_fp", echojay::fingerprintForDescription (s.desc, (int) list.size()));
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

    const SleepGuard sleepGuard ("EJ Map certification defaults");
    std::cout << sleepGuard.describe() << std::endl;
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
        o->setProperty ("slept_ms", std::round (r.sleptMs));
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
        // Not retried: a refusal (exit 3) is the plugin's answer, and a window is a
        // licence or activation event - a retry would only put it on the desktop again.
        // Re-run once: anything else unclean, or a clean run the Mac slept through.
        const auto a = runWithRetry (args, opt.timeoutMs, [&] (int attempt, const ChildResult& r) { logAttempt (s, mode, attempt, r); keep (attempt, r); });
        if (a.attempts == 2) retried = true;
        auto r = a.r;
        if (a.sleptTwice) r.kind = ChildResult::Kind::sleptTwice;
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
    using Readout = ReadoutHit;
    struct Row { const Subject* s; juce::String outcome; juce::StringArray diffs, diffsOnReadouts, emissionFails;
                 bool reproduced = false, retried = false, readoutChecked = false, emitted = false, unlicensed = false;
                 int captured = 0, onList = 0; std::vector<Readout> readouts; juce::String readoutNote; };
    std::vector<Row> rows;
    for (const auto& s : subjects)
    {
        if (s.reach != Subject::Reach::reachable && s.reach != Subject::Reach::reachableNoVersion) continue;
        Row row { &s };
        std::cout << "  probing " << s.product << " ..." << std::flush;
        auto lp = runProbe (s, "--list-params", { "--list-params" }, row.retried);
        auto ta = lp.cleanExit() ? runProbe (s, "--text-at all", { "--text-at", "all" }, row.retried) : ChildResult();
        // UNLICENSED IS NOT BROKEN (doc section 3, spec section 6). A licence-bound product
        // that refuses, hangs or shows a window is recorded as unlicensed_on_host, with the
        // shape it failed in. It is not a pass and not a defect, and it is counted apart.
        // Licence-bound by BUNDLE, or by BEHAVIOUR: PACE's UI appearing in the probe's
        // tree outranks a bundle scan that found nothing (kHs Compressor, 29 Sep).
        auto failedWith = [&] (const juce::String& f, const ChildResult& r) {
            const bool byBehaviour = ! s.licenceBound && certoutcome::isPaceUi (r.windowsInTree);
            row.unlicensed = certoutcome::classifyFailure (s.licenceBound, r.windowsInTree)
                               == certoutcome::Failure::unlicensedOnHost;
            if (! row.unlicensed) return "NOT REPRODUCED: " + f;
            return juce::String (byBehaviour ? "UNLICENSED ON HOST (licence-bound BY BEHAVIOUR: PACE UI appeared, "
                                                 "though its bundle carries no PACE markers; "
                                             : "UNLICENSED ON HOST (licence-bound; ") + f + ")"; };
        if (! lp.cleanExit()) row.outcome = failedWith ("--list-params " + lp.describe(), lp);
        else if (! ta.cleanExit()) row.outcome = failedWith ("--text-at all " + ta.describe(), ta);
        else
        {
            const auto list = parseListParams (lp.out);
            const auto text = parseTextAt (ta.out);
            for (const auto& kv : list) if (kv.second.automatable && ! kv.second.meta) ++row.onList;
            row.captured = (int) text.size();

            // THE SECOND INSTANCE. One extra instantiation per product.
            auto tb = runProbe (s, "--text-at all (second instance)", { "--text-at", "all" }, row.retried);
            const auto scan = scanReadouts (text, tb);
            row.readoutChecked = scan.checked;
            if (scan.note.isNotEmpty()) row.readoutNote = scan.note;
            row.readouts = scan.hits;
            const auto& readoutPositions = scan.positions;

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
    int reproduced = 0, differs = 0, onlyReadouts = 0, failed = 0, unlicensed = 0, readoutControls = 0, readoutChecked = 0, emitted = 0;
    for (const auto& r : rows)
    {
        if (r.reproduced) ++reproduced;
        else if (! r.diffs.isEmpty()) ++differs;
        else if (! r.diffsOnReadouts.isEmpty()) ++onlyReadouts;
        else if (r.unlicensed) ++unlicensed;
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
                    Subject::Reach::requiresHardware, Subject::Reach::versionMismatch, Subject::Reach::notInstalled,
                    Subject::Reach::ambiguous })
        rep << "  " << reachName (r).paddedRight (' ', 58) << byReach[r].size() << "\n";
    // TWO LINES, NEVER ONE: they answer different questions.
    rep << "\nREPRODUCTION (the MEASURED form vs the pushed fixture; item-12 fields are not in it): reproduced "
        << reproduced << " of " << reachable << " reachable, of " << (int) subjects.size() << " fixtures"
        << "  |  differ only on detected readouts " << onlyReadouts << "  |  differ " << differs
        << "  |  unlicensed_on_host " << unlicensed << "  |  not reproduced (named) " << failed << "\n"
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
    for (auto r : { Subject::Reach::heldPace, Subject::Reach::requiresHardware, Subject::Reach::versionMismatch,
                    Subject::Reach::notInstalled, Subject::Reach::ambiguous })
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
// DISCOVERY (ruled 29 Sep). THE DRIVER USED TO READ ITS WORKLIST FROM ITS OWN OUTPUT: every mode took its subjects from
// a directory of fixtures, so a product nobody had certified could never enter it - on a fresh Mac, nothing ever would.
// A fixture is the RECORD of what is certified, not the list of what to certify; a hand-written stub fixture unblocked
// everything, which is the proof. The worklist is now keyed on MAPS, as the coverage state machine already says:
//
//   a CANDIDATE is installed, MAPPED at its installed build, in the compressor category, with no fixture at its
//   installed identity and version.
//
//   MAPPED means EJ Map's ledger says so: a local map for that identity (maps/*.json), or the server's map state for it
//   (map-state.json: 1 local only, 2 submitted by this machine, 3 submitted by another). The second matters most: on a
//   fresh Mac almost everything is mapped by SOMEONE ELSE and has no local map at all (elysia mpressor, 29 Sep). A map
//   for a DIFFERENT build (4) is not mapped here.
//   THE CATEGORY is the local map's own, else categories.json's for that identity (indexed by format|uid, its
//   mark_keys). Both come from the server's categorisation, a documented PREREQUISITE (docs section 10): on a fresh Mac
//   the categories and the map state arrive in the same connected EJ Map run, so this one round-trip opens both stops.
//   "pitch" (tuners) is listed, not swept: tuner certification (section 5) is not built.
//
// Everything here reads the ledger; nothing writes it.
struct DiscoveryInputs
{
    std::map<juce::String, int> mapState;                   // identity key -> MapState (0 unmapped .. 5 unknown)
    std::map<juce::String, juce::String> localMapCategory;  // identity key -> category, from a local map
    std::map<juce::String, juce::String> categoryByUid;     // "AudioUnit|uid" -> category, from categories.json
    // "AudioUnit|uid" -> disposition, from categories.json, when it is anything but "sweep" - and its `why`. THE MAPPER'S
    // ESCAPE HATCH REACHES CERT (ruled 30 Sep): the runbook excludes a plugin that hangs by writing operator_excluded here,
    // and the certification batch must not open what the mapping sweep was told to leave alone.
    std::map<juce::String, juce::String> dispositionByUid;
    juce::StringArray notes;                                // what was read, for the report
};

inline DiscoveryInputs loadDiscoveryInputs (const juce::File& ledgerRoot)
{
    DiscoveryInputs in;
    if (! ledgerRoot.isDirectory()) { in.notes.add ("no EJ Map ledger at " + ledgerRoot.getFullPathName()); return in; }
    int maps = 0;
    for (const auto& f : ledgerRoot.getChildFile ("maps").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto m = juce::JSON::parse (f.loadFileAsString());
        const auto id = m.getProperty ("identity", {});
        if (! id.isObject()) continue;
        const auto key = id.getProperty ("format", "").toString() + "|" + id.getProperty ("uid", "").toString().toLowerCase()
                         + "|" + id.getProperty ("version", "").toString();
        in.localMapCategory[key] = m.getProperty ("category", "").toString();
        ++maps;
    }
    const auto ms = juce::JSON::parse (ledgerRoot.getChildFile ("map-state.json").loadFileAsString());
    if (auto* ids = ms.getProperty ("identities", {}).getDynamicObject())
        for (const auto& p : ids->getProperties())
            in.mapState[p.name.toString()] = (int) p.value.getProperty ("state", 5);
    const auto cats = juce::JSON::parse (ledgerRoot.getChildFile ("categories.json").loadFileAsString());
    if (auto* prods = cats.getProperty ("products", {}).getDynamicObject())
        for (const auto& p : prods->getProperties())
            if (const auto* mk = p.value.getProperty ("mark_keys", {}).getArray())
                for (const auto& k : *mk)
                {
                    const auto uidKey = k.toString().toLowerCase().replace ("audiounit|", "AudioUnit|").replace ("vst3|", "VST3|");
                    in.categoryByUid[uidKey] = p.value.getProperty ("category", "").toString();
                    const auto disp = p.value.getProperty ("disposition", "").toString().trim();
                    if (disp.isNotEmpty() && disp != "sweep")
                        in.dispositionByUid[uidKey] = disp + (p.value.hasProperty ("why") ? " (" + p.value.getProperty ("why", "").toString() + ")" : juce::String());
                }
    in.notes.add (juce::String (maps) + " local map(s), " + juce::String ((int) in.mapState.size()) + " map-state row(s)"
                  + (ms.getProperty ("fetched_at", "").toString().isNotEmpty() ? " (fetched " + ms.getProperty ("fetched_at", "").toString() + ")" : juce::String (" (never fetched)"))
                  + ", " + juce::String ((int) in.categoryByUid.size()) + " categorised identities"
                  + (in.dispositionByUid.empty() ? juce::String() : ", " + juce::String ((int) in.dispositionByUid.size()) + " with a disposition other than sweep (honoured)"));
    return in;
}

struct InstalledRecord { juce::PluginDescription desc; juce::String identityKey, uidKey; };

inline std::vector<InstalledRecord> installedAudioUnits()
{
    std::vector<InstalledRecord> out;
    for (const auto& t : echojay::auregistry::buildCensus().targets)
    {
        auto d = echojay::auregistry::describeFromRegistry (t.identifier);
        if (d.fileOrIdentifier.isEmpty()) continue;
        out.push_back ({ d, echojay::identityKeyForDescription (d), "AudioUnit|" + juce::String::toHexString (d.uniqueId).toLowerCase() });
    }
    return out;
}

// THE PURE CORE: which installed products are candidates, and why the rest are not. `fixtureKeys` holds "uid|version"
// (lowercase uid) for every fixture in the store.
struct Candidate { InstalledRecord inst; juce::String category, mappedBy; };
struct Discovery { std::vector<Candidate> candidates; std::map<juce::String, int> excluded; juce::StringArray tuners, excludedByDisposition; };

inline Discovery discoverCandidates (const DiscoveryInputs& in, const std::vector<InstalledRecord>& installed,
                                     const std::set<juce::String>& fixtureKeys)
{
    Discovery d;
    for (const auto& r : installed)
    {
        const auto local = in.localMapCategory.find (r.identityKey);
        const auto st = in.mapState.find (r.identityKey);
        const bool serverMapped = st != in.mapState.end() && st->second >= 1 && st->second <= 3;
        if (local == in.localMapCategory.end() && ! serverMapped) { ++d.excluded["not mapped at this build"]; continue; }
        juce::String category = local != in.localMapCategory.end() ? local->second : juce::String();
        if (category.isEmpty()) if (auto c = in.categoryByUid.find (r.uidKey); c != in.categoryByUid.end()) category = c->second;
        const auto fxKey = juce::String::toHexString (r.desc.uniqueId).toLowerCase() + "|" + r.desc.version;
        if (fixtureKeys.count (fxKey)) { ++d.excluded["fixture present at this version"]; continue; }
        if (auto disp = in.dispositionByUid.find (r.uidKey); disp != in.dispositionByUid.end())
        { ++d.excluded["disposition " + disp->second.upToFirstOccurrenceOf (" (", false, false) + " in categories.json"]; d.excludedByDisposition.add (r.desc.name + ": " + disp->second); continue; }
        // TUNERS ARE CANDIDATES (ruled 1 Oct, one store): category pitch gets the tuner certification, in the same worklist.
        if (category == "pitch") d.tuners.add (r.desc.name);
        else if (category != "compressor") { ++d.excluded[category.isEmpty() ? juce::String ("no category") : "category " + category]; continue; }
        d.candidates.push_back ({ r, category, local != in.localMapCategory.end() ? juce::String ("local map")
                                                                                  : "server map state " + juce::String (st->second) });
    }
    return d;
}

// THE WORKLIST: the fixture store's subjects (what has a record), then every discovered candidate (what has only a map),
// in COVERAGE ORDER - a record at the installed version that has not been swept first (the sweep alone is missing),
// then those that need defaults first (another version's record, or none). A fixture that already records a sweep is
// certified and leaves the list.
// THE STORE SIDE OF THE WORKLIST, pure: a fixture that records a sweep, every candidate's sweep, or a refusal stays out;
// the rest go on. `retryRefused` puts the refusals back (the iLok is in now, the hardware is attached): the record
// stays in the fixture until the re-run replaces it.
struct StorePartition { std::vector<Subject> toSweep; int recorded = 0, refused = 0, permanent = 0; };
inline StorePartition partitionStore (const std::vector<Subject>& fromStore, bool retryRefused, bool retryAll = false)
{
    StorePartition p;
    for (const auto& s : fromStore)
    {
        const bool refusal = refusalRecorded (s.pushed), permanent = refusalPermanent (s.pushed);
        if (refusal) ++p.refused;
        if (permanent) ++p.permanent;
        const bool retry = retryRefused && refusal && (retryAll || ! permanent);
        if (sweepRecorded (s.pushed) && ! retry) ++p.recorded;
        else p.toSweep.push_back (s);
    }
    return p;
}

inline std::vector<Subject> buildWorklist (const juce::File& fixturesDir, const juce::File& ledgerRoot, bool includePace,
                                           juce::StringArray& report, bool retryRefused = false, bool retryAll = false)
{
    std::vector<Subject> fromStore;
    if (fixturesDir.isDirectory()) fromStore = loadFixtures (fixturesDir);
    classify (fromStore, includePace);
    std::set<juce::String> fixtureKeys, storeUids;
    for (const auto& s : fromStore)
    {
        fixtureKeys.insert (s.uid + "|" + s.version);
        storeUids.insert (s.uid);
    }
    const auto part = partitionStore (fromStore, retryRefused, retryAll);
    std::vector<Subject> out = part.toSweep;
    const int certified = part.recorded;

    const auto in = loadDiscoveryInputs (ledgerRoot);
    const auto installed = installedAudioUnits();
    const auto disc = discoverCandidates (in, installed, fixtureKeys);
    const auto bundles = componentBundles();
    int added = 0;
    for (const auto& c : disc.candidates)
    {
        const auto uid = juce::String::toHexString (c.inst.desc.uniqueId).toLowerCase();
        if (storeUids.count (uid)) continue;                 // the store's own subject for this product already covers it
        Subject s;
        s.product = c.inst.desc.name; s.uid = uid; s.version = c.inst.desc.version;
        auto* o = new juce::DynamicObject();
        o->setProperty ("product", s.product); o->setProperty ("uid", uid); o->setProperty ("version", s.version);
        o->setProperty ("format", "AudioUnit");
        o->setProperty ("discovered", "mapped (" + c.mappedBy + "), category " + c.category + ", no fixture: defaults are sampled first");
        o->setProperty ("category", c.category);
        s.pushed = juce::var (o);
        s.category = c.category == "pitch" ? "pitch" : "compressor";
        s.desc = c.inst.desc;
        s.reach = Subject::Reach::unfixtured;
        s.installedUnique = true;
        s.hardware = requiresExternalHardware (s.product, c.inst.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false));
        juce::String why;
        s.licenceBound = paceHeld (c.inst.desc, bundles, why);
        s.detail = "mapped (" + c.mappedBy + ")" + (s.hardware ? "; needs " + externalHardwareNeeded (s.product, c.inst.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)) + " present" : juce::String());
        out.push_back (s);
        ++added;
    }
    // COVERAGE ORDER: the sweep alone is missing, then defaults-then-sweep; by name within each.
    auto rank = [] (const Subject& s) { return (s.reach == Subject::Reach::versionMismatch || s.reach == Subject::Reach::unfixtured) ? 1 : 0; };
    std::stable_sort (out.begin(), out.end(), [&] (const Subject& a, const Subject& b) {
        return rank (a) != rank (b) ? rank (a) < rank (b) : a.product.compareIgnoreCase (b.product) < 0; });

    report.add ("worklist: " + juce::String ((int) fromStore.size()) + " fixture(s) in the store (" + juce::String (certified)
                + " already record a sweep, candidates or a refusal; " + juce::String (part.refused) + " refusal(s), "
                + juce::String (part.permanent) + " of them permanent"
                + (retryRefused ? juce::String (retryAll ? "; ALL RE-RUN this time (--retry-refused-all)" : "; the transient ones RE-RUN this time (--retry-refused)") : juce::String()) + "), "
                + juce::String (added) + " discovered from the ledger at " + ledgerRoot.getFullPathName());
    report.addArray (in.notes);
    juce::StringArray ex;
    for (const auto& [why, n] : disc.excluded) ex.add (juce::String (n) + " " + why);
    report.add ("installed AUs not discovered: " + ex.joinIntoString (", "));
    if (! disc.excludedByDisposition.isEmpty())
        report.add ("left alone by categories.json disposition: " + disc.excludedByDisposition.joinIntoString ("; "));
    if (! disc.tuners.isEmpty()) report.add ("pitch category, on the worklist for tuner certification: " + disc.tuners.joinIntoString (", "));
    return out;
}

// The EJ Map ledger EJ Map itself uses when none is given.
inline juce::File defaultEjmapLedger() { return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap"); }

//==============================================================================
// THE THRESHOLD SWEEP, one product (spec section 4; --cert-sweep). The probe measures, EJ Map
// plans and derives (EjmapSweep.h). Same gates as --cert-defaults: the signature once, the
// reachability classification, the retry rule, the window watch, and every probe output kept.
// ONE PROCESS PER POSITION (ruled 29 Sep): a reference-only process at the default threshold
// (spec 4.7's unlicensed test only), then one process per position, levels quiet to loud.
// The fixture is written to <out>/fixtures only - never into the server tree, never over a
// pushed fixture - with LF line ends. processes.json records every process, so the fixture
// can be re-derived from the raw files alone (RoundTripTest does exactly that).
struct SweepOptions
{
    juce::File fixtures, probe, out, ledger = defaultEjmapLedger();
    juce::String product, hostVersion, armLabel;
    std::vector<std::pair<int, float>> extraSets;    // a DIAGNOSTIC arm: a non-swept control moved on purpose
    int timeoutMs = 120000;                          // per process
    bool includePace = false, resetPerHold = false, retryRefused = false, retryAll = false;
    bool profile = false;                            // the profile sweep (31 levels, 2.5 s, quiet reference everywhere)
};

// WHERE CERTIFICATION LANDS BY DEFAULT (ruled 30 Sep). The runbook hands a machine's work over as `zip -rq
// ~/Library/ejmap`, so anything written outside it never comes home: the sweep used to require --out with no default,
// and an operator's choice of directory decided whether the work survived. Now:
//   --out       defaults to ~/Library/ejmap/cert/            (traces, processes, reports, run.jsonl)
//   --fixtures  defaults to <out>/fixtures/                  THE STORE IS THE OUTPUT DIRECTORY
//   --probe     defaults to EchoJayProbe beside the running ejmap executable (inside ejmap.app once it ships there)
// The second line is the one that matters: the worklist prunes against --fixtures and the sweep writes to
// <out>/fixtures, so "a second run skips what the first certified" was only ever true when the two pointed at the
// same place. With the defaults they do. Explicit flags keep working for repo-store runs.
inline constexpr int kUncleanBudget = 2;     // unclean processes (after their once-retry) before a product is refused
inline juce::File defaultCertRoot() { return defaultEjmapLedger().getChildFile ("cert"); }
inline juce::File defaultProbeBeside (const juce::File& executable) { return executable.getSiblingFile ("EchoJayProbe"); }
template <typename Opts>
inline void resolveCertPaths (Opts& o, const juce::File& executable)
{
    if (o.out == juce::File())      o.out = defaultCertRoot();
    if (o.fixtures == juce::File()) o.fixtures = o.out.getChildFile ("fixtures");
    if (o.probe == juce::File())    o.probe = defaultProbeBeside (executable);
}

// The architecture the plugin RUNS in (ruling item 4): the bundle's own slices against this
// host's. A bundle with no slice for the host runs bridged, out of process, under Rosetta.
inline juce::String bundleArchs (const juce::File& bundle)
{
    auto plist = juce::parseXML (bundle.getChildFile ("Contents/Info.plist"));
    juce::String exeName;
    if (plist != nullptr)
        if (auto* dict = plist->getChildByName ("dict"))
            for (auto* k = dict->getFirstChildElement(); k != nullptr; k = k->getNextElement())
                if (k->hasTagName ("key") && k->getAllSubText() == "CFBundleExecutable" && k->getNextElement() != nullptr)
                    exeName = k->getNextElement()->getAllSubText();
    if (exeName.isEmpty()) return {};
    auto r = runChild ({ "/usr/bin/lipo", "-archs", bundle.getChildFile ("Contents/MacOS").getChildFile (exeName).getFullPathName() }, 20000);
    return r.cleanExit() ? r.out.trim() : juce::String();
}

inline juce::String fixtureFileName (const juce::var& f)
{
    return "AudioUnit_" + f.getProperty ("uid", "").toString() + "_" + f.getProperty ("version", "").toString() + ".json";
}

// DERIVE, COMPOSE, WRITE AND REPORT: shared by a live sweep and by a re-derivation from its traces, so the two can
// never disagree about what a trace means (decision D2: re-compute, never re-measure).
struct SweepRunInfo { juce::String headline, probeLabel, armLine; int processes = 0, retried = 0, sleptProcesses = 0; double sleptMs = 0; bool newIdentity = false; };

struct Derivation { sweep::Derived d; sweep::DisplayCheck dc; juce::var sweepVar; juce::String report; bool written = false; };

inline Derivation deriveOne (const sweep::Plan& plan, const sweep::Measured& m, const sweep::Provenance& pv, const SweepRunInfo& info,
                             const juce::String& fixtureName)
{
    Derivation out;
    const auto levels = plan.testLevels();
    const auto d = sweep::derive (m, levels, plan.ratioIndex, plan.quietReference);
    const auto dc = sweep::displayCheck (d, plan.thrUnit);
    const bool written = ! d.unlicensedSuspect;
    out.d = d; out.dc = dc; out.written = written;
    if (written) out.sweepVar = sweep::composeThresholdSweep (d, dc, plan, pv);

    int slicesTotal = 0, instack = 0, unl = 0, rer = 0, landed = 0, failedProc = 0; double confMax = 0;
    for (const auto& p : m.positions)
    {
        if (p.processFailed) { ++failedProc; continue; }
        if (p.unlanded) { ++unl; continue; }
        ++landed; slicesTotal += p.slices; if (p.instackMatch) ++instack; confMax = juce::jmax (confMax, p.confirmMs);
        if (p.rerendered) ++rer;
    }
    auto num = [] (std::optional<double> v, int dp = 2) { return v ? juce::String (*v, dp) : juce::String ("null"); };
    juce::String rep;
    rep << info.headline << "\n";
    if (info.probeLabel.isNotEmpty()) rep << "probe: " << info.probeLabel << "\n";
    rep << "host: " << pv.host << (pv.bridged ? " (BRIDGED)" : " (native)") << "\n";
    if (info.armLine.isNotEmpty()) rep << info.armLine << "\n";
    rep << "threshold: [" << plan.thr << "] " << plan.thrName << " unit '" << plan.thrUnit << "' (class " << plan.cls
        << (plan.thrFlags.isEmpty() ? "" : ", flags " + plan.thrFlags.joinIntoString (",")) << ") | positions " << (int) plan.norms.size()
        << (plan.stepped ? " (every step)" : " (16 evenly spaced)") << ", one process each, levels quiet to loud\n";
    if (plan.pickNote.isNotEmpty()) rep << "pick: " << plan.pickNote << "\n";
    if (! plan.linkStates.isEmpty()) rep << "links as instantiated (recorded, not written): " << plan.linkStates.joinIntoString (", ") << "\n";
    rep << "ratio: " << (plan.ratioIndex >= 0 ? "[" + juce::String (plan.ratioIndex) + "] ran at '" + d.ratioText + "' -> "
                                                  + (d.ratio ? juce::String (*d.ratio) : juce::String ("not numeric"))
                                                  + (plan.ratioRaise ? " (RAISED from '" + d.ratioInstantiated + "', value READ BACK after the write)" : juce::String())
                                                : plan.ratioNote)
        << " | auto make-up disabled: " << (plan.autoMakeupDisabled ? "yes" : "no") << "\n"
        << "default-threshold reference ('" << m.refText << "', spec 4.7 check only):";
    for (double L : levels) { const auto k = sweep::levelKey (L); if (m.refDb.count (k)) rep << "  " << k << " out-in " << juce::String (m.refDb.at (k) - m.inRmsDb.at (k), 2); }
    if (! d.defaultGain.empty()) rep << "  (default gain is information only)";
    if (d.quietReference)
    {
        rep << "\nlinear reference: each position's own gain at -48, checked against -54 (must differ by 6 dB within "
            << juce::String (sweep::kQuietTolDb, 1) << "):";
        for (size_t i = 0; i < d.quietCheckDb.size(); ++i)
            rep << " " << (d.quietCheckDb[i] ? juce::String (*d.quietCheckDb[i], 2) : juce::String ("--"));
    }
    else
    {
        rep << "\nlinear reference (soft end";
        if (d.softEnd) { rep << ", position " << *d.softEnd << "):"; for (double L : levels) rep << "  " << sweep::levelKey (L) << " " << juce::String (d.linearGain.at (sweep::levelKey (L)), 2); }
        else rep << "): none";
        if (d.softEndSpreadDb) rep << "  | spread " << juce::String (*d.softEndSpreadDb, 2) << " dB (unusable beyond " << juce::String (sweep::kLinearDb, 1) << ")";
    }
    rep << "\n";
    {
        std::map<juce::String, int> counts;
        for (const auto& x : d.landedBy) ++counts[x.isEmpty() ? "no_process" : x];
        rep << "writes landed by:";
        for (const auto& [k, v] : counts) rep << " " << k << " " << v;
        rep << "\n";
    }
    if (info.processes > 0) rep << "processes: " << info.processes << " run, " << info.retried << " retried, " << failedProc << " position(s) failed"
                                << (info.sleptProcesses > 0 ? ", " + juce::String (info.sleptProcesses) + " SLEPT DURING THEIR RUN (" + juce::String (info.sleptMs / 1000.0, 1)
                                                                + " s asleep; their readings are recorded as measured, not re-run)" : juce::String()) << "\n";
    rep << "write verify: " << landed << " landed, " << unl << " write_unlanded (skipped), in-stack read already matched on " << instack
        << " of " << landed << ", pump slices total " << slicesTotal << ", longest confirm " << juce::String (confMax, 1) << " ms, re-rendered " << rer << "\n";
    { juce::StringArray a; for (int i : d.holdDoubled) a.add (juce::String (i)); rep << "hold doubled at positions: " << (a.isEmpty() ? "none" : a.joinIntoString (",")) << "\n"; }
    { juce::StringArray a; for (int i : d.stillMoving) a.add (juce::String (i)); rep << "still moving AFTER doubling (reading not used): " << (a.isEmpty() ? "none" : a.joinIntoString (",")) << "\n"; }
    rep << "\npos   norm      display        red@-24  red@-12  red@-6    T(peak)   T-display\n";
    for (size_t i = 0; i < d.norms.size(); ++i)
    {
        auto cell = [&] (double L) {
            auto it = d.reduction.find (sweep::levelKey (L));
            std::optional<double> g = it != d.reduction.end() && i < it->second.size() ? it->second[i] : std::nullopt;
            return (g ? juce::String (*g, 2) : juce::String ("--")).paddedLeft (' ', 7); };
        const juce::var tv = i < d.tEquivalent.size() ? d.tEquivalent[i] : juce::var();
        juce::String t = tv.isVoid() ? "--" : tv.isObject() ? juce::JSON::toString (tv, true) : juce::String ((double) tv, 1);
        juce::String off;
        for (const auto& o : dc.offsets) if (o.first == (int) i) off = juce::String (o.second, 2);
        rep << juce::String ((int) i).paddedLeft (' ', 3) << "  " << juce::String (d.norms[i], 4).paddedRight (' ', 8) << "  "
            << d.texts[i].paddedRight (' ', 13) << cell (-24) << "  " << cell (-12) << "  " << cell (-6) << "   "
            << t.paddedLeft (' ', 9) << "   " << off.paddedLeft (' ', 7) << "\n";
    }
    rep << "\n1 MAP:             " << d.result << (d.reason.isNotEmpty() ? " (" + d.reason + ")" : juce::String())
        << " | sense " << (d.sense.isEmpty() ? "--" : d.sense) << " | engage";
    for (double L : levels) { auto e = d.engage.count (sweep::levelKey (L)) ? d.engage.at (sweep::levelKey (L)) : std::nullopt; rep << " " << sweep::levelKey (L) << ":" << (e ? juce::String (*e) : juce::String ("none")); }
    rep << "\n2 engage drift:    " << num (dc.engageDriftDb);
    for (const auto& [k, v] : dc.engage) rep << "  " << k << ":" << num (v);
    rep << (dc.engage.empty() ? "  (not a dB display)" : "") << "   (primary; displayLinear unset until the population decides)"
        << "\n   offset IQR:      " << (dc.positions > 0 ? juce::String (dc.iqrDb, 2) + " dB over " + juce::String (dc.positions) + " positions (min "
                                                         + juce::String (dc.minDb, 2) + ", max " + juce::String (dc.maxDb, 2) + ")" : juce::String ("null"))
        << "\n3 displayOffsetDb: " << num (dc.offsetDb) << (dc.offsetDb ? juce::String (std::abs (*dc.offsetDb) <= sweep::kDisplayDb ? "  (inside spec 7's 2 dB bar)" : "  (outside spec 7's 2 dB bar)") : juce::String())
        << "\n" << (written ? "fixture: " + fixtureName : "NOT LICENSED suspected (silent, non-finite or not the input's tone at default): " + d.referenceNote + "no thresholdSweep written") << "\n";
    out.report = rep;
    return out;
}

// A REFUSAL IS A RECORD (ruled 30 Sep). A product stopped before a usable measurement - its defaults would not sample,
// its plan found no threshold, its ratio search failed, a window appeared, its unclean-process budget ran out, or its
// reference was not the input's tone - used to write NOTHING, so discovery offered it again on every batch: four
// minutes of timeouts per batch per hanger, for ever. It now writes the fixture it has (the store's, or the discovered
// identity with no controls yet) with `thresholdRefusal` naming the stage and the reason, and NO thresholdSweep. The
// worklist treats that as recorded; --retry-refused, a new installed version, or deleting the file puts it back.
inline juce::File writeRefusalRecord (const juce::File& fixturesDir, const juce::var& base, const juce::String& stage, const juce::String& reason,
                                      int processes, int unclean, const juce::String& probeLabel, const juce::String& host)
{
    auto f = sweep::stripPrivate (juce::JSON::parse (juce::JSON::toString (base)));
    if (auto* o = f.getDynamicObject())
    {
        o->removeProperty ("thresholdSweep"); o->removeProperty ("thresholdCandidates"); o->removeProperty ("thresholdReview");
        auto* r = new juce::DynamicObject();
        r->setProperty ("stage", stage);
        r->setProperty ("retry", refusalIsPermanent (stage) ? "permanent: a fact about the product; --retry-refused skips it (--retry-refused-all does not)"
                                                             : "transient: a fact about the run; --retry-refused re-runs it once the reason is fixed");
        r->setProperty ("reason", reason);
        r->setProperty ("recordedAt", juce::Time::getCurrentTime().toISO8601 (false));
        r->setProperty ("host", host);
        r->setProperty ("probe", probeLabel);
        r->setProperty ("processes", processes);
        r->setProperty ("uncleanProcesses", unclean);
        r->setProperty ("note", "stopped before a usable measurement: no thresholdSweep. The worklist skips a recorded refusal; "
                                "re-run it with --retry-refused (after fixing what the reason names), or by deleting this file, "
                                "or it re-runs on its own at a new installed version");
        o->setProperty ("thresholdRefusal", juce::var (r));
    }
    sweep::stampSchema (f, kSchemaCompressor);
    fixturesDir.createDirectory();
    auto out = fixturesDir.getChildFile (fixtureFileName (base));
    out.replaceWithText (juce::JSON::toString (f) + "\n", false, false, "\n");
    return out;
}

// Returns whether a thresholdSweep was written; false is the licence-suspect case (silent, non-finite or off-tone at
// default), which the caller records as a refusal.
inline bool composeAndReport (const juce::var& base, const sweep::Plan& plan, const sweep::Measured& m, const sweep::Provenance& pv,
                              const juce::File& fixtureOut, const juce::File& reportOut, const SweepRunInfo& info, juce::String* whyNot = nullptr)
{
    const auto one = deriveOne (plan, m, pv, info, fixtureOut.getFileName());
    if (one.written)
        fixtureOut.replaceWithText (juce::JSON::toString (sweep::composeFixture (base, one.sweepVar)) + "\n", false, false, "\n");
    else if (whyNot != nullptr)
        *whyNot = "not licensed suspected (silent, non-finite or not the input's tone at default): " + one.d.referenceNote;
    reportOut.replaceWithText (one.report, false, false, "\n");
    std::cout << one.report << std::flush;
    return one.written;
}

// A LICENCE IS A PROPERTY OF THE PRODUCT, NOT OF A CANDIDATE (ruled 30 Sep, the third misfire of a licence inference
// after the PACE bundle markers and the 3 dB reference test). If ANY candidate produced the input's tone, the plugin is
// licensed, and a candidate whose reference was silent or off-tone at default is saying something about THAT STAGE (a
// gate closed on the tone, a processor that mutes) - never about licensing. Such a candidate keeps the result its data
// gave it and records the silent reference beside it. Only a product silent on every candidate stays licence-suspect.
inline void resolveLicenceAtProductLevel (std::vector<std::pair<sweep::Plan, Derivation>>& cands, const sweep::Provenance& pv)
{
    bool producedAudio = false;
    for (const auto& [q, one] : cands) producedAudio = producedAudio || ! one.d.unlicensedSuspect;
    if (! producedAudio) return;
    for (auto& [q, one] : cands)
    {
        if (! one.d.unlicensedSuspect) continue;
        one.written = true;
        one.sweepVar = sweep::composeThresholdSweep (one.d, one.dc, q, pv);
        if (auto* o = one.sweepVar.getDynamicObject())
            o->setProperty ("silentOrOffToneAtDefault", "this candidate's reference at default was not the input's tone (" + one.d.referenceNote.trim()
                                                        + "); another candidate of the same product produced it, so this is a fact about the stage, not the licence");
        one.report = one.report.replace ("NOT LICENSED suspected (silent, non-finite or not the input's tone at default): ",
                                         "reference silent or off-tone at default, NOT a licence question (another candidate produced the tone): ")
                               .replace ("no thresholdSweep written", "sweep written with its data's result '" + one.d.result + "'");
    }
}

// SEVERAL CANDIDATES (ruled 30 Sep): one fixture carrying every candidate's sweep, labelled, and NO thresholdSweep - a
// human reads the curves and picks. The fixture says so in `thresholdReview`.
inline void composeCandidatesAndReport (const juce::var& base, const sweep::Plan& plan, const std::vector<std::pair<sweep::Plan, Derivation>>& cands,
                                        const juce::File& fixtureOut, const juce::File& reportOut)
{
    auto f = sweep::stripPrivate (juce::JSON::parse (juce::JSON::toString (base)));
    juce::Array<juce::var> arr;
    juce::String rep;
    for (const auto& [q, one] : cands)
    {
        auto* c = new juce::DynamicObject();
        c->setProperty ("index", q.thr); c->setProperty ("name", q.thrName);
        juce::Array<juce::var> fl; for (auto& x : q.thrFlags) fl.add (x); c->setProperty ("flags", fl);
        c->setProperty ("thresholdSweep", one.written ? sweep::stripPrivate (one.sweepVar) : juce::var());
        // A FLAT CANDIDATE IS EVIDENCE, NOT A DEFECT (ruled 30 Sep): 997 Hz excites one band, so a band that does not
        // cover it shows nothing. Said on the candidate, so thresholdReview does not read as a dead end. A PASS-THROUGH
        // flat is NOT that evidence: the product did nothing as instantiated, so the tone says nothing about its bands.
        if (one.d.result == "flat" && one.d.passThroughAtDefaults)
            c->setProperty ("reading", "pass_through_at_defaults: output equals input at every reading, so the product does nothing at its instantiate "
                                       "defaults; not evidence about band coverage or this control (a precondition gap, not a band gap)");
        else if (one.d.result == "flat")
            c->setProperty ("reading", "no response at 997 Hz: this candidate's band does not cover the test tone, or the control is not a threshold at it; "
                                       "uncertified for want of an in-band tone (a per-band tone is a later feature)");
        else if (one.d.result == "certified")
            c->setProperty ("reading", "responds at 997 Hz: a curve to read");
        arr.add (juce::var (c));
        rep << one.report << "\n";
    }
    // ONE FIELD, NOT A SCAN (ruled 30 Sep): the responding candidates are NAMED here, index and name, and every candidate's
    // verdict sits beside them. Two consumers deriving the same thing from the array is how the Waves untick bug happened;
    // the server half reads thresholdReview.responding and nothing else.
    auto ref = [] (const sweep::Plan& q) { auto* o = new juce::DynamicObject(); o->setProperty ("index", q.thr); o->setProperty ("name", q.thrName); return juce::var (o); };
    juce::Array<juce::var> responding, verdicts, passThrough;
    int curves = 0, flats = 0, passCount = 0;
    for (const auto& [q, one] : cands)
    {
        const juce::String verdict = ! one.written ? "licence_suspect" : one.d.passThroughAtDefaults ? "pass_through_at_defaults" : one.d.result;
        auto v = ref (q); v.getDynamicObject()->setProperty ("result", verdict); verdicts.add (v);
        if (one.d.result == "certified") { ++curves; responding.add (ref (q)); }
        else if (one.d.result == "flat" && one.d.passThroughAtDefaults) { ++passCount; passThrough.add (ref (q)); }
        else if (one.d.result == "flat") ++flats;
    }
    if (auto* o = f.getDynamicObject())
    {
        auto* rv = new juce::DynamicObject();
        rv->setProperty ("class", plan.cls);
        rv->setProperty ("candidates", (int) cands.size());
        rv->setProperty ("note", "several controls hold the threshold role; each was swept with the others at their instantiate defaults. "
                                 "On a multiband, the band containing the 997 Hz test tone can certify and the rest are uncertified for want of an in-band tone. "
                                 "There is no thresholdSweep and no dB-equivalent map until a human picks from the curves; the map comes after the pick, when that band's own ratio can be read. "
                                 "`responding` names the candidate(s) that respond; `passThroughAtDefaults` names those where the product did nothing as instantiated (not band evidence); `verdicts` holds every candidate's result");
        rv->setProperty ("curves", curves);
        rv->setProperty ("flats", flats);                       // flats that ARE band-coverage evidence: pass-through is counted apart
        rv->setProperty ("responding", responding);             // THE FIELD: which candidate(s) respond at 997 Hz, by index and name
        auto* pt = new juce::DynamicObject();
        pt->setProperty ("count", passCount); pt->setProperty ("candidates", passThrough);
        rv->setProperty ("passThroughAtDefaults", juce::var (pt));
        rv->setProperty ("verdicts", verdicts);                 // every candidate's result in one place, same index/name keys
        o->setProperty ("thresholdReview", juce::var (rv));
        o->setProperty ("thresholdCandidates", arr);
    }
    sweep::stampSchema (f, kSchemaCompressor);
    fixtureOut.replaceWithText (juce::JSON::toString (f) + "\n", false, false, "\n");
    reportOut.replaceWithText (rep, false, false, "\n");
    std::cout << rep << std::flush;
}

inline int runCertSweep (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const SleepGuard sleepGuard ("EJ Map certification sweep");
    if (! opt.out.createDirectory()) { say ("SWEEP: cannot create " + opt.out.getFullPathName()); return 2; }
    const auto id = checkProbe (opt.probe, {}, {});
    if (! id.ok) { say ("SWEEP: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const juce::String probeLabel = "signed EchoJayProbe (feat/ejmap-cert), team " + id.team + ", cdhash " + id.cdhash;
    const juce::String date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");

    juce::StringArray wl;
    auto subjects = buildWorklist (opt.fixtures, opt.ledger, opt.includePace, wl, opt.retryRefused, opt.retryAll);
    const Subject* sp = nullptr;
    for (const auto& x : subjects) if (x.product == opt.product) { sp = &x; break; }
    if (sp == nullptr) { say ("SWEEP: '" + opt.product + "' is not on the worklist (" + wl.joinIntoString ("; ") + ")"); return 2; }
    const Subject& s = *sp;
    if (! measurable (s, opt.includePace))
    {
        juce::String why = reachName (s.reach) + (s.detail.isNotEmpty() ? " (" + s.detail + ")" : juce::String());
        if (s.reach == Subject::Reach::versionMismatch)
            why << (! s.installedUnique ? "; more than one version installed, and one is never picked"
                    : s.hardware ? "; needs its hardware" : "; licence-bound (run with --include-pace)");
        say ("SWEEP: " + s.product + " is not measurable here: " + why);
        return 4;
    }

    const juce::String armTag = opt.armLabel.isNotEmpty() ? ".arm-" + opt.armLabel : juce::String();
    auto raw = opt.out.getChildFile ("raw");
    raw.createDirectory();
    auto ledger = opt.out.getChildFile ("run.jsonl");
    juce::Array<juce::var> processes;
    bool windowSeen = false;
    juce::StringArray windows;
    int unclean = 0;
    juce::StringArray uncleanWhat;
    auto overBudget = [&] { return unclean >= kUncleanBudget; };
    juce::var base = s.pushed;               // the store's fixture, or the discovered identity; replaced by the sampled defaults below
    // The trace stem for THIS product, known before its defaults are: a refusal's processes.json lands under it too.
    const auto stem0 = "AudioUnit_" + juce::String::toHexString (s.desc.uniqueId).toLowerCase() + "_" + s.desc.version + ".sweep" + armTag;
    // THE RETRY RULE, as --cert-defaults: anything but a clean exit is re-run ONCE; a refusal (exit 3) and a
    // window are answers, not retried. Every attempt's output is kept.
    auto runProbe = [&] (const juce::String& stem, const juce::String& tag, const juce::StringArray& extra, float norm) -> ChildResult
    {
        juce::StringArray args { opt.probe.getFullPathName(), s.desc.name, s.desc.fileOrIdentifier,
                                 juce::String::toHexString (s.desc.uniqueId) };
        args.addArray (extra);
        const auto a = runWithRetry (args, opt.timeoutMs, [&] (int attempt, const ChildResult& r)
        {
            const auto file = stem + "." + tag + "." + juce::String (attempt) + ".txt";
            raw.getChildFile (file).replaceWithText (r.out, false, false, "\n");
            auto* o = new juce::DynamicObject();
            o->setProperty ("tag", tag); o->setProperty ("attempt", attempt); o->setProperty ("file", file);
            // A process that slept is recorded as NOT clean: a re-derivation must not trust it either.
            const bool slept = r.sleptMs > kSleptMs;
            o->setProperty ("outcome", slept && r.cleanExit() ? "SLEPT " + juce::String (r.sleptMs / 1000.0, 1) + " s during its run: not trusted"
                                                              : r.describe());
            o->setProperty ("clean", r.cleanExit() && ! slept); o->setProperty ("norm", norm);
            o->setProperty ("ms", r.ms);
            o->setProperty ("slept_ms", std::round (r.sleptMs));
            processes.add (juce::var (o));
            ledger.appendText (juce::JSON::toString (juce::var (o), true) + "\n");
            if (r.kind == ChildResult::Kind::uiShown) { windowSeen = true; windows.addArray (r.windowsInTree); }
        });
        auto r = a.r;
        if (a.sleptTwice) r.kind = ChildResult::Kind::sleptTwice;   // refused: unclean, and it says why
        if (! r.cleanExit()) { ++unclean; uncleanWhat.add (tag + ": " + r.describe()); }
        return r;
    };
    auto licenceLine = [&] (const juce::String& what, const ChildResult& r) {
        const auto f = certoutcome::classifyFailure (s.licenceBound, r.windowsInTree);
        return (f == certoutcome::Failure::unlicensedOnHost ? "UNLICENSED ON HOST: " : "ERROR: ") + what + " " + r.describe(); };
    // THE PER-PRODUCT BUDGET (ruled 30 Sep): the mapper's sweep bounds a plugin at one 90 s process; a certification is
    // ~17 processes per candidate, each with a once-retry, so a plugin that hangs on every load cost ~68 minutes before
    // its fixture existed. After kUncleanBudget processes end unclean (after their retry) the product stops, and the
    // refusal is written THEN. The defaults phase stops on its first unclean process, as before - within the budget.
    auto refuse = [&] (int rc, const juce::String& stage, const juce::String& reason) {
        say ("SWEEP: " + s.product + " - " + reason);
        opt.out.getChildFile (stem0 + ".processes.json").replaceWithText (juce::JSON::toString (juce::var (processes)) + "\n", false, false, "\n");
        const auto rec = writeRefusalRecord (opt.out.getChildFile ("fixtures"), base, stage, reason, processes.size(), unclean,
                                             probeLabel, "EJ Map " + opt.hostVersion);
        say ("SWEEP: refusal recorded at stage '" + stage + "' (" + (refusalIsPermanent (stage) ? "permanent" : "transient") + ") -> " + rec.getFileName()
             + " (skipped by the worklist until " + (refusalIsPermanent (stage) ? "--retry-refused-all" : "--retry-refused") + " or a new version)");
        return rc; };

    // AN UNSEEN VERSION: sample defaults first, then sweep, in one pass (ruled 29 Sep). The identity is the
    // INSTALLED component's, composed by the same composeFixture as --cert-defaults.
    bool newIdentity = false;
    if (needsDefaultsFirst (s))
    {
        const auto dstem = "AudioUnit_" + juce::String::toHexString (s.desc.uniqueId).toLowerCase() + "_" + s.desc.version + ".defaults";
        auto lp = runProbe (dstem, "list-params", { "--list-params" }, -1.0f);
        if (! lp.cleanExit()) return refuse (1, "defaults", licenceLine ("defaults --list-params", lp));
        auto ta = runProbe (dstem, "text-at", { "--text-at", "all" }, -1.0f);
        if (! ta.cleanExit()) return refuse (1, "defaults", licenceLine ("defaults --text-at all", ta));
        auto tb = runProbe (dstem, "text-at-2", { "--text-at", "all" }, -1.0f);
        const auto text = parseTextAt (ta.out);
        const auto scan = scanReadouts (text, tb);
        std::vector<fixturereadout::Moved> moved;
        for (const auto& h : scan.hits) moved.push_back ({ h.index, h.a, h.b, h.ta, h.tb });
        base = fixturereadout::applySchema (composeFixture (s, parseListParams (lp.out), text, lp.code, ta.code, probeLabel, date),
                                            moved, scan.checked);
        newIdentity = true;
    }

    auto plan = sweep::planFromFixture (base);
    if (! plan.ok) return refuse (4, "plan", plan.why);
    if (opt.profile) plan.makeProfile();
    for (auto x : opt.extraSets) plan.sets.push_back (x);
    const auto stem = fixtureFileName (base).upToLastOccurrenceOf (".json", false, false) + ".sweep" + armTag;

    // THE RATIO RAISE (spec 4.2): read the ratio's own texts on a grid, pick the smallest READ value at or above 4:1,
    // and write it in every process. The value used for R is read back after that write, in each process.
    if (plan.ratioRaise)
    {
        const auto rc = sweep::findControl (base, plan.ratioIndex);
        std::vector<float> grid;
        if (sweep::isSteppedControl (rc)) { const int n = (int) rc.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) grid.push_back ((float) k / (float) (n - 1)); }
        else for (int k = 0; k <= 64; ++k) grid.push_back ((float) k / 64.0f);
        juce::StringArray gs;
        for (float g : grid) gs.add (juce::String (g, 6));
        const auto r = runProbe (stem, "ratio-search", { "--text-at-norms", juce::String (plan.ratioIndex), gs.joinIntoString (",") }, -1.0f);
        if (windowSeen || ! r.cleanExit()) return refuse (1, "ratio_search", licenceLine ("ratio search", r));
        std::vector<sweep::GridPoint> pts;
        for (const auto& line : juce::StringArray::fromLines (r.out))
        {
            const auto f = juce::StringArray::fromTokens (line, "\t", "");
            if (f.size() >= 3 && f[0] == "at" && f[2] == "landed") pts.push_back ({ (float) f[1].getDoubleValue(), f[f.indexOf ("text") + 1] });
        }
        juce::String chosenText;
        const auto norm = sweep::chooseRatioRaise (pts, chosenText);
        if (! norm) return refuse (4, "ratio_none", "ratio instantiates at '" + plan.ratioDefaultText + "' and no grid position reads 4:1 or more; not swept");
        plan.sets.push_back ({ plan.ratioIndex, *norm }); plan.setRoles[plan.ratioIndex] = "ratio_raise";
        std::cout << "  ratio raise: [" << plan.ratioIndex << "] '" << plan.ratioDefaultText << "' -> norm " << *norm << " (grid read '" << chosenText << "')" << std::endl;
    }

    // THE NEUTRAL SET, profile sweeps only: mix / make-up by role, drive by name; each chosen on the control's own text grid
    // and written as a precondition. What could not be chosen is said. Never the threshold, the ratio, or an engage switch.
    juce::StringArray neutralNotes;
    if (opt.profile)
    {
        std::vector<roles::NamedControl> named;
        if (const auto* cs = base.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs) named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(), false });
        const auto cl = roles::classify (named, roles::Category::compressor);
        std::vector<std::pair<int, juce::String>> wanted;
        for (const auto& r : cl.controls)
        {
            if (r.index == plan.thr || r.index == plan.ratioIndex) continue;
            if (r.role == "mix") wanted.push_back ({ r.index, "mix_wet" });
            else if (r.role == "makeup") wanted.push_back ({ r.index, "makeup_zero" });
            else if (r.role.isEmpty() && sweep::driveNamed (r.name)) wanted.push_back ({ r.index, "drive_cleanest" });
        }
        for (const auto& [idx, role] : wanted)
        {
            if (windowSeen || overBudget()) break;
            bool already = false; for (auto [i, v] : plan.sets) already = already || i == idx;
            if (already) continue;
            const auto ctl = sweep::findControl (base, idx);
            std::vector<float> grid;
            if (sweep::isSteppedControl (ctl)) { const int n = (int) ctl.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) grid.push_back ((float) k / (float) juce::jmax (1, n - 1)); }
            else for (int k = 0; k <= 32; ++k) grid.push_back ((float) k / 32.0f);
            juce::StringArray gs; for (float g : grid) gs.add (juce::String (g, 6));
            const auto r = runProbe (stem, "neutral-" + juce::String (idx), { "--text-at-norms", juce::String (idx), gs.joinIntoString (",") }, -1.0f);
            if (! r.cleanExit()) { neutralNotes.add (role + " [" + juce::String (idx) + "]: grid read failed (" + r.describe() + "), not set"); continue; }
            std::vector<sweep::GridPoint> pts;
            for (const auto& line : juce::StringArray::fromLines (r.out))
            {
                const auto f = juce::StringArray::fromTokens (line, "\t", "");
                if (f.size() >= 3 && f[0] == "at" && f[2] == "landed") pts.push_back ({ (float) f[1].getDoubleValue(), f[f.indexOf ("text") + 1] });
            }
            juce::String chosen;
            const auto norm = sweep::chooseNeutral (pts, role, chosen);
            if (! norm) { neutralNotes.add (role + " [" + juce::String (idx) + "] " + ctl.getProperty ("name", "").toString() + ": no numeric text on its grid, not set"); continue; }
            plan.sets.push_back ({ idx, *norm }); plan.setRoles[idx] = role;
            std::cout << "  neutral: [" << idx << "] " << ctl.getProperty ("name", "").toString() << " -> norm " << *norm << " ('" << chosen << "') for " << role << std::endl;
        }
    }
    juce::StringArray sets;
    for (auto [i, v] : plan.sets) sets.add (juce::String (i) + ":" + juce::String (v, 6));
    // ONE SWEEP of one threshold control: a reference process, then one process per position. Called once for a single
    // threshold, once per candidate when several hold the role (tags prefixed "c<index>." so the traces stay apart).
    auto sweepFor = [&] (const sweep::Plan& q, const juce::String& tagPrefix, sweep::ProcessOut& refOut, std::vector<sweep::ProcessOut>& posOut)
    {
        juce::StringArray levelList;
        for (double L : q.probeLevels()) levelList.add (juce::String ((int) L));
        auto sweepArgs = [&] (const juce::String& norms, const juce::String& ref) {
            juce::StringArray a { "--sweep", "thr=" + juce::String (q.thr), "norms=" + norms, "levels=" + levelList.joinIntoString (","), "hz=997",
                                  "hold=" + juce::String (q.holdS, 2), "discard=" + juce::String (q.discardS, 2), "win=" + juce::String (q.winS, 2), "ref=" + ref, "moving_db=0.1",
                                  juce::String ("reset=") + (opt.resetPerHold ? "1" : "0") };
            juce::StringArray all = sets;                                     // the plan's preconditions (ratio raise, auto make-up off)
            for (const auto& w : q.engage) all.add (juce::String (w.index) + ":" + juce::String (w.norm, 6));   // plus the engage writes
            if (! all.isEmpty()) a.add ("set=" + all.joinIntoString (","));
            return a; };
        std::cout << "  sweeping " << s.product << (tagPrefix.isNotEmpty() ? " candidate [" + juce::String (q.thr) + "] " + q.thrName : juce::String())
                  << (newIdentity ? " (unseen version " + s.desc.version + ": defaults sampled first)" : juce::String())
                  << " - 1 reference + " << (int) q.norms.size() << " position processes" << std::endl;
        if (! windowSeen && ! overBudget())
        {
            const auto r = runProbe (stem, tagPrefix + "ref", sweepArgs ("", "2.0"), -1.0f);
            refOut = { r.out, r.cleanExit(), r.describe(), -1.0f };
        }
        posOut.clear();
        for (size_t k = 0; k < q.norms.size() && ! windowSeen && ! overBudget(); ++k)
        {
            const auto tag = tagPrefix + "pos" + juce::String ((int) k).paddedLeft ('0', 2);
            const auto r = runProbe (stem, tag, sweepArgs (juce::String (q.norms[k], 6), "0"), q.norms[k]);
            posOut.push_back ({ r.out, r.cleanExit(), r.describe(), q.norms[k] });
        }
    };
    sweep::ProcessOut refOut;
    std::vector<sweep::ProcessOut> posOut;
    std::vector<std::pair<sweep::Plan, std::pair<sweep::ProcessOut, std::vector<sweep::ProcessOut>>>> candRuns;
    // THE QUIET-LEVEL FALLBACK (ruled 30 Sep): a sweep whose soft end has no linear anchor is re-swept at once with the
    // per-position quiet reference (levels -54/-48 added), tagged "q." + the candidate's prefix so both runs stay in the
    // traces. The plan that comes back is the one to derive from. Same processes, same probe, one more pass.
    // THE ENGAGE SEARCH (spec section 3, built 1 Oct): a candidate that reads pass-through at the instantiate defaults
    // is tried with each engage candidate in turn - a QUICK probe of three positions with the write - and the first
    // that shows gain reduction is verified (GR with the write, pass-through without: the sweep just taken). The full
    // sweep is then re-run with the write as a precondition, tagged "e<idx>." + prefix. Every refused candidate is
    // recorded. If none shows GR the product stays pass-through, and the fixture lists what was tried.
    auto engageSearch = [&] (sweep::Plan q, const juce::String& prefix) -> sweep::Plan
    {
        const auto cands = sweep::engageCandidates (base, q.thrName);
        std::cout << "  pass-through at defaults: trying " << (int) cands.size() << " engage candidate(s)" << std::endl;
        for (const auto& w : cands)
        {
            if (windowSeen || overBudget()) break;
            sweep::Plan t = q; t.engage = { w };
            sweep::ProcessOut tr; std::vector<sweep::ProcessOut> tp;
            sweep::Plan quick = t; quick.norms = { q.norms.front(), q.norms[q.norms.size() / 2], q.norms.back() };
            if (quick.profile) { quick.profile = false; quick.quietReference = false; quick.holdS = 1.5; quick.discardS = 0.75; quick.winS = 0.25; }   // the quick probe stays quick
            sweepFor (quick, "eq" + juce::String (w.index) + "." + prefix, tr, tp);
            const auto d = sweep::derive (sweep::mergeProcesses (tr, tp), quick.testLevels(), q.ratioIndex, quick.quietReference);
            const bool hit = sweep::showsResponse (d);
            q.engageTried.add (w.name + " -> " + juce::String (w.norm, 1) + " (from '" + w.fromDisplay + "'): " + (hit ? "GAIN REDUCTION" : d.result + (d.reason.isNotEmpty() ? " - " + d.reason : juce::String())));
            std::cout << "    " << q.engageTried[q.engageTried.size() - 1] << std::endl;
            if (hit) { q.engage = { w }; return q; }
        }
        return q;
    };
    auto sweepWithFallback = [&] (sweep::Plan q, const juce::String& prefix, sweep::ProcessOut& r, std::vector<sweep::ProcessOut>& ps) -> sweep::Plan
    {
        sweepFor (q, prefix, r, ps);
        if (q.quietReference || windowSeen || overBudget()) return q;
        auto first = sweep::derive (sweep::mergeProcesses (r, ps), q.testLevels(), q.ratioIndex, q.quietReference);
        if (first.result == "flat" && first.passThroughAtDefaults && q.engage.empty())
        {
            q = engageSearch (q, prefix);
            if (q.engage.empty()) return q;                                      // stays pass-through; the tried list rides the fixture
            std::cout << "  engage verified: " << q.engage.front().name << " -> " << q.engage.front().norm << "; full sweep with the write" << std::endl;
            sweepFor (q, "e" + juce::String (q.engage.front().index) + "." + prefix, r, ps);
            if (windowSeen || overBudget()) return q;
            first = sweep::derive (sweep::mergeProcesses (r, ps), q.testLevels(), q.ratioIndex, q.quietReference);
        }
        if (! sweep::needsQuietFallback (first)) return q;
        std::cout << "  soft end has no linear anchor (" << first.reason << "): re-sweeping with the quiet-level reference" << std::endl;
        q.quietReference = true;
        q.referenceFallbackNote = "quiet-level reference used because the soft end had no linear anchor: " + first.reason;
        sweepFor (q, "q." + (q.engage.empty() ? juce::String() : "e" + juce::String (q.engage.front().index) + ".") + prefix, r, ps);
        return q;
    };
    if (plan.candidates.empty())
        plan = sweepWithFallback (plan, "", refOut, posOut);
    else
        for (const auto& c : plan.candidates)
        {
            if (windowSeen || overBudget()) break;
            auto q = plan.forCandidate (c);
            sweep::applyCandidateControl (q, base);
            sweep::ProcessOut cr; std::vector<sweep::ProcessOut> cp;
            q = sweepWithFallback (q, "c" + juce::String (c.index) + ".", cr, cp);
            candRuns.push_back ({ q, { cr, cp } });
        }
    opt.out.getChildFile (stem + ".processes.json").replaceWithText (juce::JSON::toString (juce::var (processes)) + "\n", false, false, "\n");
    auto fixturesDir = opt.out.getChildFile ("fixtures");
    fixturesDir.createDirectory();
    if (newIdentity)   // the defaults the sweep was planned on, kept whatever happens next
        fixturesDir.getChildFile (fixtureFileName (base).upToLastOccurrenceOf (".json", false, false) + ".defaults.json")
                   .replaceWithText (juce::JSON::toString (base) + "\n", false, false, "\n");
    if (windowSeen)
    {
        // A WINDOW IS RECORDED; ONLY PACE'S IS CALLED A LICENCE FACT (EjmapCertOutcome.h). Said with the stage the probe
        // had reached, so "at the first render" (UAD-2 on this Mac, 29 Sep) reads differently from "on load".
        juce::String stageSeen = "unknown";
        for (int i = processes.size(); --i >= 0;)
            if (processes[i].getProperty ("outcome", "").toString().startsWith ("SHOWED A WINDOW"))
            {
                for (const auto& line : juce::StringArray::fromLines (raw.getChildFile (processes[i].getProperty ("file", "").toString()).loadFileAsString()))
                    if (line.startsWith ("stage\t")) stageSeen = line.fromFirstOccurrenceOf ("\t", false, false);
                stageSeen << " (process " << processes[i].getProperty ("tag", "").toString() << ")";
                break;
            }
        const bool licence = certoutcome::classifyFailure (s.licenceBound, windows) == certoutcome::Failure::unlicensedOnHost;
        return refuse (licence ? 1 : 5, "window", juce::String (licence ? "UNLICENSED ON HOST" : "STOPPED: A WINDOW (recorded, not called a licence fact)")
             + ": a window appeared in the probe's tree (" + windows.joinIntoString (", ") + ") at stage " + stageSeen
             + "; the product was stopped at once, nothing derived");
    }
    if (overBudget())
        return refuse (6, "budget", "stopped after " + juce::String (unclean) + " unclean processes (budget " + juce::String (kUncleanBudget)
                       + ", each already re-run once): " + uncleanWhat.joinIntoString ("; ") + "; nothing derived");

    const auto m = plan.candidates.empty() ? sweep::mergeProcesses (refOut, posOut)
                                           : sweep::mergeProcesses (candRuns.front().second.first, candRuns.front().second.second);
    juce::String pluginArch = m.arch;
    bool bridged = false;
    const auto bundles = componentBundles();
    if (auto it = bundles.find (s.desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false)); it != bundles.end())
    {
        const auto archs = juce::StringArray::fromTokens (bundleArchs (it->second), " ", "");
        if (! archs.isEmpty() && ! archs.contains (m.arch)) { bridged = true; pluginArch = archs.contains ("x86_64") ? "x86_64" : archs[0]; }
    }
    sweep::Provenance pv;
    pv.measuredAt = juce::Time::getCurrentTime().toISO8601 (false);
    pv.host = "EJ Map " + opt.hostVersion + " / " + pluginArch + " / 48k";
    pv.bridged = bridged;
    SweepRunInfo info;
    if (! opt.extraSets.empty())
    {
        auto* arm = new juce::DynamicObject();
        arm->setProperty ("label", opt.armLabel);
        arm->setProperty ("note", "DIAGNOSTIC ARM - a non-swept control was moved on purpose; this is not the product's certification");
        juce::Array<juce::var> xs;
        for (auto [i, v] : opt.extraSets)
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("index", i); x->setProperty ("normalised", v);
            if (m.params.count (i)) x->setProperty ("name", m.params.at (i).first);
            if (m.setTexts.count (i)) x->setProperty ("readBack", m.setTexts.at (i));
            xs.add (juce::var (x));
        }
        arm->setProperty ("sets", xs);
        pv.diagnosticArm = juce::var (arm);
        info.armLine = "DIAGNOSTIC ARM '" + opt.armLabel + "' (not the product's certification)";
    }
    const auto outName = fixtureFileName (base).upToLastOccurrenceOf (".json", false, false) + armTag + ".json";
    info.headline = "EJ Map threshold sweep - " + s.product + " -> " + outName
                    + (s.reach == Subject::Reach::unfixtured ? "  (DISCOVERED: " + s.detail + "; no fixture existed)"
                       : newIdentity ? "  (NEW IDENTITY: installed " + s.desc.version + ", no fixture at that version)" : juce::String());
    info.probeLabel = probeLabel;
    info.processes = processes.size();
    for (const auto& pr : processes)
    {
        if ((int) pr.getProperty ("attempt", 1) == 2) ++info.retried;
        const double sl = (double) pr.getProperty ("slept_ms", 0.0);
        if (sl > 1000.0) { ++info.sleptProcesses; info.sleptMs += sl; }   // over a second: a sleep, not clock jitter
    }
    info.newIdentity = newIdentity;
    if (plan.candidates.empty())
    {
        juce::String whyNot;
        if (composeAndReport (base, plan, m, pv, fixturesDir.getChildFile (outName), opt.out.getChildFile (stem + ".report.txt"), info, &whyNot))
            return 0;
        return refuse (1, "reference", whyNot);
    }
    std::vector<std::pair<sweep::Plan, Derivation>> cands;
    for (const auto& [q, run] : candRuns)
    {
        SweepRunInfo ci = info;
        ci.headline = "CANDIDATE [" + juce::String (q.thr) + "] " + q.thrName + " (flags " + q.thrFlags.joinIntoString (",") + ") - " + s.product
                      + " (class " + plan.cls + ", " + juce::String ((int) plan.candidates.size()) + " candidates; the others at their instantiate defaults)";
        cands.push_back ({ q, deriveOne (q, sweep::mergeProcesses (run.first, run.second), pv, ci, outName) });
    }
    resolveLicenceAtProductLevel (cands, pv);
    composeCandidatesAndReport (base, plan, cands, fixturesDir.getChildFile (outName), opt.out.getChildFile (stem + ".report.txt"));
    return 0;
}

// RE-DERIVE A SWEEP FROM ITS TRACES (--cert-sweep-rederive): nothing is measured. The plan comes from the fixture's
// own controls, the provenance (when, where, bridged, the diagnostic arm) from its existing thresholdSweep, and the
// readings from processes.json and the raw files. A rule change is applied to every past sweep this way.
// WHICH RUN TO RE-DERIVE FROM (1 Oct): the traces may hold the first pass (prefix), an engaged full sweep ("e<idx>." +
// prefix), a quiet-level fallback ("q." + ...), or both. The latest in that order is the run the fixture was composed
// from, and the plan is restored to match: engage writes from the fixture's own `engage` record.
struct TraceRun { juce::String prefix; bool quiet = false; int engageIndex = -1; };
inline TraceRun resolveTraceRun (const juce::File& processesJson, const juce::String& prefix)
{
    std::set<juce::String> tags;
    const auto list = juce::JSON::parse (processesJson.loadFileAsString());   // held: getArray() on a temporary dangles
    if (const auto* a = list.getArray())
        for (const auto& p : *a) tags.insert (p.getProperty ("tag", "").toString());
    int eidx = -1;
    for (const auto& t : tags)
        if (t.startsWith ("e") && t.endsWith ("." + prefix + "ref") && ! t.startsWith ("eq"))
            eidx = t.substring (1).upToFirstOccurrenceOf (".", false, false).getIntValue();
    TraceRun r;
    const juce::String e = eidx >= 0 ? "e" + juce::String (eidx) + "." : juce::String();
    if (tags.count ("q." + e + prefix + "ref")) { r.prefix = "q." + e + prefix; r.quiet = true; }
    else r.prefix = e + prefix;
    r.engageIndex = eidx;
    return r;
}
inline void restoreEngage (sweep::Plan& q, const juce::var& oldSweep)
{
    const auto eg = oldSweep.getProperty ("engageWrites", {});
    if (const auto* ws = eg.getProperty ("writes", {}).getArray())
        for (const auto& w : *ws)
            q.engage.push_back ({ (int) w.getProperty ("index", -1), w.getProperty ("control", "").toString(),
                                  (float) (double) w.getProperty ("norm", 0.0), w.getProperty ("from", "").toString() });
    if (const auto* tr = eg.getProperty ("tried", {}).getArray())
        for (const auto& t : *tr) q.engageTried.add (t.toString());
}

inline int runSweepRederive (const juce::File& fixtureIn, const juce::File& processesJson, const juce::File& rawDir, const juce::File& fixtureOut)
{
    const auto fx = juce::JSON::parse (fixtureIn.loadFileAsString());
    auto old = fx.getProperty ("thresholdSweep", {});
    // Provenance comes from the first candidate that HAS a sweep: the first may be a licence-suspect with none
    // (MDynamicsMBLarge, whose Band 1 gate is silent at default).
    if (! old.isObject() && fx.getProperty ("thresholdCandidates", {}).isArray())
        for (const auto& c : *fx.getProperty ("thresholdCandidates", {}).getArray())
            if (c.getProperty ("thresholdSweep", {}).isObject()) { old = c.getProperty ("thresholdSweep", {}); break; }
    if (! old.isObject()) { std::cout << "REDERIVE: " << fixtureIn.getFileName() << " has no thresholdSweep" << std::endl; return 2; }
    auto plan = sweep::planFromFixture (fx);
    if (! plan.ok) { std::cout << "REDERIVE: " << plan.why << std::endl; return 4; }
    // The raise, as recorded: the fixture says it ran raised; the value itself is read back from the traces.
    if (old.getProperty ("ratioDuring", {}).hasProperty ("raisedFrom")) plan.ratioRaise = true;
    // A profile sweep is recognised by its 31-level grid, and re-derived as one.
    if (old.getProperty ("tone", {}).getProperty ("levels_dbfs", {}).size() > 3) plan.makeProfile();
    sweep::Provenance pv;
    pv.measuredAt = old.getProperty ("measuredAt", "").toString();
    pv.host = old.getProperty ("host", "").toString();
    pv.bridged = (bool) old.getProperty ("bridged", false);
    pv.diagnosticArm = old.getProperty ("diagnosticArm", {});
    auto base = juce::JSON::parse (juce::JSON::toString (fx));
    if (auto* o = base.getDynamicObject()) { o->removeProperty ("thresholdSweep"); o->removeProperty ("thresholdCandidates"); o->removeProperty ("thresholdReview"); }
    SweepRunInfo info;
    info.headline = "RE-DERIVED from traces (nothing measured) - " + fx.getProperty ("product", "").toString() + " -> " + fixtureOut.getFileName();
    if (! pv.diagnosticArm.isVoid()) info.armLine = "DIAGNOSTIC ARM '" + pv.diagnosticArm.getProperty ("label", "").toString() + "' (not the product's certification)";
    // A "q."-prefixed run in the traces is the quiet-level fallback (ruled 30 Sep) and is the run to derive from.
    auto fallbackNote = [&] (const juce::var& sweepVar) {
        const auto n = sweepVar.getProperty ("linearReference", {}).getProperty ("fallback", "").toString();
        return n.isNotEmpty() ? n : juce::String ("quiet-level reference used because the soft end had no linear anchor (re-derived from the q. traces)"); };
    if (plan.candidates.empty())
    {
        sweep::ProcessOut ref; std::vector<sweep::ProcessOut> pos;
        const auto run = resolveTraceRun (processesJson, "");
        if (run.quiet) { plan.quietReference = true; plan.referenceFallbackNote = fallbackNote (old); }
        if (run.engageIndex >= 0) restoreEngage (plan, old);
        if (! sweep::loadProcesses (processesJson, rawDir, ref, pos, run.prefix)) { std::cout << "REDERIVE: cannot load the traces" << std::endl; return 2; }
        composeAndReport (base, plan, sweep::mergeProcesses (ref, pos), pv, fixtureOut, fixtureOut.getSiblingFile (fixtureOut.getFileNameWithoutExtension() + ".report.txt"), info);
        return 0;
    }
    std::vector<std::pair<sweep::Plan, Derivation>> cands;
    for (const auto& c : plan.candidates)
    {
        auto q = plan.forCandidate (c);
        sweep::applyCandidateControl (q, base);
        sweep::ProcessOut ref; std::vector<sweep::ProcessOut> pos;
        juce::var oldC;
        for (const auto& x : *fx.getProperty ("thresholdCandidates", {}).getArray()) if ((int) x.getProperty ("index", -1) == c.index) oldC = x.getProperty ("thresholdSweep", {});
        const auto run = resolveTraceRun (processesJson, "c" + juce::String (c.index) + ".");
        if (run.quiet) { q.quietReference = true; q.referenceFallbackNote = fallbackNote (oldC); }
        if (run.engageIndex >= 0 || oldC.hasProperty ("engageWrites")) restoreEngage (q, oldC);
        if (! sweep::loadProcesses (processesJson, rawDir, ref, pos, run.prefix)) { std::cout << "REDERIVE: no traces for candidate " << c.index << std::endl; return 2; }
        SweepRunInfo ci = info;
        ci.headline = "CANDIDATE [" + juce::String (q.thr) + "] " + q.thrName + " - re-derived";
        cands.push_back ({ q, deriveOne (q, sweep::mergeProcesses (ref, pos), pv, ci, fixtureOut.getFileName()) });
    }
    resolveLicenceAtProductLevel (cands, pv);
    composeCandidatesAndReport (base, plan, cands, fixtureOut, fixtureOut.getSiblingFile (fixtureOut.getFileNameWithoutExtension() + ".report.txt"));
    return 0;
}

inline int runCertTuner (const SweepOptions& opt);   // defined below (tuner certification, one store)

// EVERY RUNNABLE PRODUCT (--cert-sweep-all): the census's runnable list, minus what is named in --skip, one
// runCertSweep each. A product's failure is reported and the batch goes on.
inline int runSweepAll (SweepOptions opt, const juce::StringArray& skip)
{
    juce::StringArray wl;
    auto subjects = buildWorklist (opt.fixtures, opt.ledger, opt.includePace, wl, opt.retryRefused, opt.retryAll);
    std::cout << wl.joinIntoString ("\n") << std::endl;
    juce::StringArray done, refused;
    int n = 0;
    const SleepGuard sleepGuard ("EJ Map certification batch");
    std::cout << sleepGuard.describe() << std::endl;
    for (const auto& s : subjects)
    {
        // A DISCOVERED product has no controls until its defaults are sampled, so its plan is decided after sampling,
        // not predicted; a fixture's subject is predicted from the fixture's own controls, as before.
        const bool planLater = s.reach == Subject::Reach::unfixtured;
        const bool tuner = s.category == "pitch";
        if (skip.contains (s.product) || ! measurable (s, opt.includePace) || (! tuner && ! planLater && ! sweep::planFromFixture (s.pushed).ok))
            continue;
        ++n;
        std::cout << "\n=== [" << n << "] " << s.product << (tuner ? " (tuner)" : "") << std::endl;
        opt.product = s.product;
        const int rc = tuner ? runCertTuner (opt) : runCertSweep (opt);
        (rc == 0 ? done : refused).add (s.product + (rc == 0 ? juce::String() : " (exit " + juce::String (rc) + ")"));
    }
    std::cout << "\nSWEEP-ALL: " << n << " attempted, " << done.size() << " swept to a fixture, " << refused.size() << " stopped\n";
    for (const auto& r : refused) std::cout << "  stopped: " << r << "\n";
    std::cout << std::flush;
    return refused.isEmpty() ? 0 : 1;
}

//==============================================================================
// TUNER CERTIFICATION (--cert-tuner, spec section 5, built 1 Oct). One product: resolve it in the AU registry, sample
// its defaults (the unseen-version path's probes), role its controls with the TUNER lexicon, then for every control
// holding the strength role run the probe's pitch sweep twice - static (strength) and square vibrato (speed) - and
// write <out>/tuners/<identity>.json with `pitchCandidates` (one record per candidate) and `pitchReview`. The probe
// hosts all positions of one generator in ONE process (no compression history to reset between positions). An
// ARA/offline-only tool is refused before any process by name.
inline bool araOnlyByName (const juce::String& product)
{
    return product.containsIgnoreCase ("melodyne") || product.containsIgnoreCase ("waves tune lt") || product.containsIgnoreCase ("waves tune (");
}

inline int runCertTuner (const SweepOptions& opt)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const SleepGuard sleepGuard ("EJ Map tuner certification");
    auto outDir = opt.out.getChildFile ("fixtures");                 // ONE STORE (ruled 1 Oct): tuner records beside the compressors'
    if (! outDir.createDirectory()) { say ("TUNER: cannot create " + outDir.getFullPathName()); return 2; }
    const auto id = checkProbe (opt.probe, {}, {});
    if (! id.ok) { say ("TUNER: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const juce::String probeLabel = "signed EchoJayProbe (feat/ejmap-cert), team " + id.team + ", cdhash " + id.cdhash;
    // Resolve the installed component by product name.
    std::vector<InstalledRecord> hits;
    for (const auto& r : installedAudioUnits()) if (r.desc.name == opt.product) hits.push_back (r);
    if (hits.empty()) { say ("TUNER: '" + opt.product + "' is not an installed AudioUnit"); return 2; }
    if (hits.size() > 1) { say ("TUNER: '" + opt.product + "' resolves to " + juce::String ((int) hits.size()) + " components; refused"); return 2; }
    Subject s; s.desc = hits[0].desc; s.product = s.desc.name; s.uid = juce::String::toHexString (s.desc.uniqueId).toLowerCase(); s.version = s.desc.version;
    s.reach = Subject::Reach::unfixtured; s.installedUnique = true; s.category = "pitch";
    // A refusal is a record here too: the identity with no controls, so the worklist stops offering it.
    auto identityOnly = [&] { auto* o = new juce::DynamicObject(); o->setProperty ("product", s.product); o->setProperty ("uid", s.uid);
                              o->setProperty ("version", s.version); o->setProperty ("format", "AudioUnit"); o->setProperty ("category", "pitch");
                              o->setProperty ("schema", kSchemaTuner); return juce::var (o); };
    if (araOnlyByName (opt.product))
    {
        say ("TUNER: " + opt.product + " is an ARA/offline tool with no real-time pitch path: uncertifiable by any harness, refused before any process");
        writeRefusalRecord (outDir, identityOnly(), "ara_only", "ARA/offline-only pitch tool: no real-time path, uncertifiable by any harness", 0, 0, probeLabel, "EJ Map " + opt.hostVersion);
        return 4;
    }
    const auto stem = "AudioUnit_" + s.uid + "_" + s.version + ".tuner";
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory();
    auto ledger = opt.out.getChildFile ("run.jsonl");
    juce::Array<juce::var> processes; bool windowSeen = false; juce::StringArray windows;
    auto runProbe = [&] (const juce::String& tag, const juce::StringArray& extra) -> ChildResult
    {
        juce::StringArray args { opt.probe.getFullPathName(), s.desc.name, s.desc.fileOrIdentifier, juce::String::toHexString (s.desc.uniqueId) };
        args.addArray (extra);
        const auto a = runWithRetry (args, opt.timeoutMs, [&] (int attempt, const ChildResult& r)
        {
            const auto file = stem + "." + tag + "." + juce::String (attempt) + ".txt";
            raw.getChildFile (file).replaceWithText (r.out, false, false, "\n");
            auto* o = new juce::DynamicObject();
            o->setProperty ("tag", tag); o->setProperty ("attempt", attempt); o->setProperty ("file", file);
            o->setProperty ("outcome", r.describe()); o->setProperty ("clean", r.cleanExit()); o->setProperty ("ms", r.ms);
            processes.add (juce::var (o));
            ledger.appendText (juce::JSON::toString (juce::var (o), true) + "\n");
            if (r.kind == ChildResult::Kind::uiShown) { windowSeen = true; windows.addArray (r.windowsInTree); }
        });
        auto r = a.r; if (a.sleptTwice) r.kind = ChildResult::Kind::sleptTwice; return r;
    };
    const juce::String date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    auto refuseT = [&] (int rc, const juce::String& stage, const juce::String& reason) {
        say ("TUNER: " + s.product + " - " + reason);
        const auto rec = writeRefusalRecord (outDir, identityOnly(), stage, reason, processes.size(), 1, probeLabel, "EJ Map " + opt.hostVersion);
        say ("TUNER: refusal recorded at stage '" + stage + "' -> " + rec.getFileName());
        return rc; };
    auto lp = runProbe ("list-params", { "--list-params" });
    if (! lp.cleanExit()) return refuseT (1, "defaults", (windowSeen ? "UNLICENSED ON HOST: " : "ERROR: ") + juce::String ("defaults --list-params ") + lp.describe());
    auto ta = runProbe ("text-at", { "--text-at", "all" });
    if (! ta.cleanExit()) return refuseT (1, "defaults", (windowSeen ? "UNLICENSED ON HOST: " : "ERROR: ") + juce::String ("defaults --text-at all ") + ta.describe());
    auto base = composeFixture (s, parseListParams (lp.out), parseTextAt (ta.out), lp.code, ta.code, probeLabel, date);
    if (auto* o = base.getDynamicObject()) { o->setProperty ("category", "pitch"); o->setProperty ("schema", kSchemaTuner); }

    // ROLES with the tuner lexicon: strength candidates, the key/scale control.
    std::vector<roles::NamedControl> named;
    if (const auto* cs = base.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs) named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(), false });
    const auto cl = roles::classify (named, roles::Category::tuner);
    std::vector<const roles::ControlRole*> strength; int keyIndex = -1; juce::String keyText;
    for (const auto& r : cl.controls)
    {
        if (r.role == "strength") strength.push_back (&r);
        if (r.role == "key" && keyIndex < 0) { keyIndex = r.index; keyText = sweep::findControl (base, r.index).getProperty ("defaultOnInstantiate", {}).getProperty ("display", "").toString(); }
    }
    say ("TUNER: " + s.product + " " + s.version + ": " + juce::String ((int) strength.size()) + " strength candidate(s)"
         + (keyIndex >= 0 ? ", key/scale [" + juce::String (keyIndex) + "] as instantiated '" + keyText + "'" : juce::String (", no key/scale control roled")));
    juce::Array<juce::var> cands; int measuredAny = 0;
    if (strength.empty()) say ("TUNER: no control holds the strength role: nothing to sweep (recorded)");
    for (const auto* c : strength)
    {
        if (windowSeen) break;
        const auto ctrl = sweep::findControl (base, c->index);
        juce::StringArray norms;
        if (sweep::isSteppedControl (ctrl)) { const int n = (int) ctrl.getProperty ("numSteps", 0); for (int k = 0; k < n; ++k) norms.add (juce::String ((float) k / (float) juce::jmax (1, n - 1), 6)); }
        else for (int k = 0; k < 8; ++k) norms.add (juce::String ((float) k / 7.0f, 6));
        const juce::String ctl = juce::String (c->index);
        auto st = runProbe ("c" + ctl + ".static",  { "--sweep-pitch", "ctl=" + ctl, "norms=" + norms.joinIntoString (","), "gen=static",  "note=220", "cents=30", "hold=4", "db=-18" });
        auto vb = runProbe ("c" + ctl + ".vibrato", { "--sweep-pitch", "ctl=" + ctl, "norms=" + norms.joinIntoString (","), "gen=vibrato", "shape=square", "rate=0.5", "note=220", "cents=30", "hold=6", "db=-18" });
        const auto ms = pitch::parsePitch (st.cleanExit() ? st.out : juce::String ("refused " + st.describe()));
        const auto mv = pitch::parsePitch (vb.cleanExit() ? vb.out : juce::String ("refused " + vb.describe()));
        auto rec = pitch::composePitchSweep (ms, mv, keyIndex, keyText);
        if (auto* o = rec.getDynamicObject())
        {
            o->setProperty ("index", c->index); o->setProperty ("name", c->name);
            if (! ms.ok) o->setProperty ("staticRefused", ms.refused);
            if (! mv.ok) o->setProperty ("vibratoRefused", mv.refused);
        }
        measuredAny += (int) rec.getProperty ("strengthMeasured", 0) + (int) rec.getProperty ("speedMeasured", 0);
        say ("  [" + ctl + "] " + c->name + ": strength measured at " + rec.getProperty ("strengthMeasured", 0).toString() + " position(s), speed at " + rec.getProperty ("speedMeasured", 0).toString());
        cands.add (rec);
    }
    opt.out.getChildFile (stem + ".processes.json").replaceWithText (juce::JSON::toString (juce::var (processes)) + "\n", false, false, "\n");
    auto f = sweep::stripPrivate (base);
    if (auto* o = f.getDynamicObject())
    {
        o->setProperty ("pitchCandidates", cands);
        auto* rv = new juce::DynamicObject();
        rv->setProperty ("candidates", cands.size()); rv->setProperty ("measured", measuredAny);
        rv->setProperty ("windowSeen", windowSeen);
        rv->setProperty ("note", "every control holding the strength role was swept with a static detuned note and a square vibrato; a control whose static residual moves across positions is a strength control, one whose transition duration moves is a speed control; the server half reads both");
        o->setProperty ("pitchReview", juce::var (rv));
    }
    auto outFile = outDir.getChildFile ("AudioUnit_" + s.uid + "_" + s.version + ".json");
    outFile.replaceWithText (juce::JSON::toString (f) + "\n", false, false, "\n");
    say ("TUNER: " + s.product + " -> " + outFile.getFileName() + (windowSeen ? "  (A WINDOW APPEARED: " + windows.joinIntoString (", ") + ")" : juce::String()));
    return windowSeen ? 5 : 0;
}

//==============================================================================
// EXPORT (--export-profile <record> <out.json> | --export-profiles <store> <outDir>): every record through the one
// exporter; a refusal is printed with its reason and nothing is written for it. The report is the per-product line Sean
// asked for: profile emitted / emitted with fit over 1.5 / not possible and why.
inline int runExportProfiles (const juce::File& in, const juce::File& out, bool whole)
{
    juce::Array<juce::File> files;
    if (whole) files = in.findChildFiles (juce::File::findFiles, false, "*.json"); else files.add (in);
    if (whole) out.createDirectory();
    int emitted = 0, over = 0, refused = 0;
    for (const auto& f : files)
    {
        if (f.getFileName().endsWith (".defaults.json")) continue;
        const auto rec = juce::JSON::parse (f.loadFileAsString());
        const auto e = profile::exportCompProfile (rec);
        const auto name = rec.getProperty ("product", f.getFileNameWithoutExtension()).toString();
        if (! e.ok) { ++refused; std::cout << "NOT POSSIBLE   " << name << ": " << e.refused << std::endl; continue; }
        const auto dst = whole ? out.getChildFile (f.getFileName()) : out;
        dst.replaceWithText (juce::JSON::toString (e.profile) + "\n", false, false, "\n");
        const bool big = e.fitMaxErrorDb > 1.5;
        if (big) ++over; else ++emitted;
        std::cout << (big ? "EMITTED, FIT OVER 1.5  " : "EMITTED        ") << name << ": " << e.points << " curve points, fit max error "
                  << juce::String (e.fitMaxErrorDb, 2) << " dB -> " << dst.getFileName() << std::endl;
    }
    std::cout << "EXPORT: " << emitted << " emitted, " << over << " emitted with fit over 1.5, " << refused << " not possible" << std::endl;
    return 0;
}

//==============================================================================
// HIS SECTION 8 TONE CHECK (v1.2), run here (--cert-tone-check): L = -18 dBFS RMS, g = 2. Pick the position exactly as
// his section 6 says from the EXPORTED profile, write it plus engage + neutral + the reference ratio, render 997 Hz at L
// through the signed probe in one fresh process (with -54/-48 for the position's own quiet reference), measure GR. Pass
// is within 0.5 dB of g. The ratio NORM is not in his profile (only the control name and the value), so it comes from
// our record's preconditions - said in the result, because it is a gap in the contract.
inline int runToneCheck (const SweepOptions& opt, const juce::File& profileFile, const juce::File& recordFile, double Lrms, double g)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {});
    if (! id.ok) { say ("TONE: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const auto profile = juce::JSON::parse (profileFile.loadFileAsString());
    const auto record  = juce::JSON::parse (recordFile.loadFileAsString());
    if (! profile.isObject() || ! record.isObject()) { say ("TONE: cannot read the profile or the record"); return 2; }
    const auto product = profile.getProperty ("plugin", {}).getProperty ("name", "").toString();
    std::vector<InstalledRecord> hits;
    for (const auto& r : installedAudioUnits()) if (r.desc.name == product) hits.push_back (r);
    if (hits.size() != 1) { say ("TONE: '" + product + "' resolves to " + juce::String ((int) hits.size()) + " component(s)"); return 2; }
    const auto& desc = hits[0].desc;
    const auto pick = profile::pickPosition (profile, Lrms, g);
    if (! pick.ok) { say ("TONE: " + product + " - section 6 picks nothing: " + pick.refused); return 4; }
    // The writes: engage + neutral by control NAME from the profile, resolved to indices through the record's controls.
    auto indexOf = [&] (const juce::String& name) { if (const auto* cs = record.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) if (c.getProperty ("name", "") == name) return (int) c.getProperty ("index", -1); return -1; };
    juce::StringArray sets; juce::Array<juce::var> writes;
    auto addWrite = [&] (const juce::String& name, int idx, double norm, const juce::String& why) {
        sets.add (juce::String (idx) + ":" + juce::String (norm, 6));
        auto* o = new juce::DynamicObject(); o->setProperty ("control", name); o->setProperty ("index", idx); o->setProperty ("norm", norm); o->setProperty ("why", why); writes.add (juce::var (o)); };
    for (const auto& e : *profile.getProperty ("engage", {}).getArray())   { const auto n = e.getProperty ("control", "").toString(); const int i = indexOf (n); if (i < 0) { say ("TONE: engage control '" + n + "' not in the record"); return 2; } addWrite (n, i, (double) e.getProperty ("norm", 0.0), "engage"); }
    for (const auto& e : *profile.getProperty ("neutral", {}).getArray())  { const auto n = e.getProperty ("control", "").toString(); const int i = indexOf (n); if (i < 0) { say ("TONE: neutral control '" + n + "' not in the record"); return 2; } addWrite (n, i, (double) e.getProperty ("norm", 0.0), "neutral"); }
    juce::String ratioNote = "no ratio precondition in the record (ratio as instantiated)";
    if (const auto* pre = record.getProperty ("thresholdSweep", {}).getProperty ("preconditions", {}).getArray())
        for (const auto& x : *pre)
            if (x.getProperty ("role", "").toString() == "ratio_raise")
            { addWrite ("ratio", (int) x.getProperty ("index", -1), (double) x.getProperty ("norm", 0.0), "reference ratio - NORM from our record; his profile carries only the value"); ratioNote = "ratio norm taken from the record's preconditions"; }
    const auto plan = sweep::planFromFixture (record);
    if (! plan.ok) { say ("TONE: the record has no plan: " + plan.why); return 4; }
    const double Lpeak = Lrms + profile::kPeakToSineRmsDb;
    juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId),
                             "--sweep", "thr=" + juce::String (plan.thr), "norms=" + juce::String (pick.norm, 6),
                             "levels=-54,-48," + juce::String (Lpeak, 4), "hz=997", "hold=2.5", "discard=2.2", "win=0.3", "ref=0", "moving_db=0.1", "reset=0" };
    if (! sets.isEmpty()) args.add ("set=" + sets.joinIntoString (","));
    say ("TONE: " + product + " - L " + juce::String (Lrms, 2) + " dBFS RMS (" + juce::String (Lpeak, 2) + " peak), g " + juce::String (g, 1)
         + "; section 6 picks norm " + juce::String (pick.norm, 4) + (pick.i1 >= 0 ? " between points " + juce::String (pick.i0) + " and " + juce::String (pick.i1) : " at point " + juce::String (pick.i0))
         + " (in_at_gr at g: " + juce::String (pick.inAtG0, 2) + (pick.i1 >= 0 ? " / " + juce::String (pick.inAtG1, 2) : juce::String()) + "); " + ratioNote);
    const auto r = runChild (args, opt.timeoutMs);
    auto raw = opt.out.getChildFile ("raw"); raw.createDirectory();
    const auto rawFile = raw.getChildFile (profileFile.getFileNameWithoutExtension() + ".tonecheck.1.txt");
    rawFile.replaceWithText (r.out, false, false, "\n");
    if (! r.cleanExit()) { say ("TONE: the probe " + r.describe()); return 1; }
    sweep::ProcessOut po { r.out, true, r.describe(), (float) pick.norm };
    const auto m = sweep::mergeProcesses ({ juce::String(), true, "none", -1.0f }, { po });
    const auto d = sweep::derive (m, { Lpeak }, plan.ratioIndex, true);
    const auto key = sweep::levelKey (Lpeak);
    std::optional<double> gr = d.reduction.count (key) && ! d.reduction.at (key).empty() ? d.reduction.at (key)[0] : std::nullopt;
    const bool quietOk = ! d.quietCheckDb.empty() && d.quietCheckDb[0] && std::abs (*d.quietCheckDb[0]) <= sweep::kQuietTolDb;
    const bool pass = gr && quietOk && std::abs (*gr - g) <= 0.5;
    auto* o = new juce::DynamicObject();
    o->setProperty ("product", product); o->setProperty ("map_fp", profile.getProperty ("plugin", {}).getProperty ("map_fp", ""));
    o->setProperty ("L_rms_dbfs", Lrms); o->setProperty ("L_peak_dbfs", Lpeak); o->setProperty ("g_db", g);
    auto* pk = new juce::DynamicObject(); pk->setProperty ("norm", pick.norm); pk->setProperty ("point", pick.i0); if (pick.i1 >= 0) pk->setProperty ("point_next", pick.i1);
    pk->setProperty ("in_at_g", pick.inAtG0); pk->setProperty ("stepped", pick.stepped); o->setProperty ("pick", juce::var (pk));
    o->setProperty ("writes", writes);
    o->setProperty ("quiet_check_ok", quietOk);
    o->setProperty ("gr_measured_db", gr ? juce::var (std::round (*gr * 100.0) / 100.0) : juce::var());
    o->setProperty ("pass_within_0_5_db", pass);
    o->setProperty ("probe", id.cdhash); o->setProperty ("measuredAt", juce::Time::getCurrentTime().toISO8601 (false));
    o->setProperty ("ratio_norm_source", ratioNote);
    const auto out = profileFile.getSiblingFile (profileFile.getFileNameWithoutExtension() + ".tonecheck.json");
    out.replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("TONE: " + product + " - GR " + (gr ? juce::String (*gr, 2) : juce::String ("unreadable")) + " dB at L (target " + juce::String (g, 1) + ", quiet check "
         + (quietOk ? "ok" : "FAILED") + ") -> " + (pass ? "PASS" : "FAIL") + " (within 0.5 dB) -> " + out.getFileName());
    return pass ? 0 : 1;
}

//==============================================================================
// THE SWEEP CENSUS (--cert-sweep-census): which fixtures a sweep can run on HERE, read-only, nothing
// instantiated. A product is runnable when it is measurable (the version guard is on comparison, not
// measurement) AND its plan finds exactly one threshold. For an unseen version the plan is predicted from
// the pushed fixture's controls; the real plan is made from the defaults sampled at the installed version.
inline int runSweepCensus (const juce::File& fixturesDir, const juce::File& ledgerRoot, bool includePace, bool retryRefused = false, bool retryAll = false)
{
    juce::StringArray wl;
    auto subjects = buildWorklist (fixturesDir, ledgerRoot, includePace, wl, retryRefused, retryAll);
    int runnable = 0, runnableNewIdentity = 0, runnableDiscovered = 0;
    std::map<juce::String, juce::StringArray> notRunnable;
    juce::StringArray rows;
    for (const auto& s : subjects)
    {
        const bool discovered = s.reach == Subject::Reach::unfixtured;
        const auto plan = sweep::planFromFixture (s.pushed);
        if (! discovered && ! plan.ok)
        {
            notRunnable[plan.why.contains ("unity") ? "ratio instantiates at 1:1 - the section 4.2 raise to 4:1 is not built (class " + plan.cls + ")"
                                                    : "no single threshold (" + plan.cls + ") - deferred to review after the sweep"].add (s.product);
            continue;
        }
        if (! measurable (s, includePace))
        {
            juce::String why = reachName (s.reach);
            if (s.reach == Subject::Reach::versionMismatch)
                why = ! s.installedUnique ? why + " - several versions installed"
                    : s.hardware ? "CONDITIONAL - " + s.detail.fromFirstOccurrenceOf ("; ", false, false) + " (one re-run once attached)"
                    : why + " - licence-bound";
            if (s.reach == Subject::Reach::requiresHardware) why = "CONDITIONAL - " + s.detail.upToFirstOccurrenceOf (";", false, false) + " (one re-run once attached)";
            if (s.reach == Subject::Reach::unfixtured)
                why = s.hardware ? "discovered, CONDITIONAL - " + s.detail.fromFirstOccurrenceOf ("; ", false, false) + " (one re-run once attached)"
                                 : juce::String ("discovered, licence-bound (run with --include-pace)");
            notRunnable[why].add (s.product);
            continue;
        }
        ++runnable;
        const bool fresh = s.reach == Subject::Reach::versionMismatch;
        if (fresh) ++runnableNewIdentity;
        if (discovered) ++runnableDiscovered;
        rows.add ("  " + s.product.paddedRight (' ', 40)
                  + (! plan.candidates.empty() ? juce::String ((int) plan.candidates.size()) + " CANDIDATE thresholds (" + plan.cls + "), each swept and labelled for review; " : juce::String())
                  + (discovered ? "DISCOVERED at " + s.desc.version + ", " + s.detail + " (defaults first; plan after sampling)"
                     : fresh ? "NEW IDENTITY at installed " + s.desc.version + " (defaults first)" : juce::String ("at the fixture's version"))
                  + (s.licenceBound ? "  [licence-bound]" : ""));
    }
    std::cout << "SWEEP CENSUS - PACE " << (includePace ? "included" : "held") << "\n" << wl.joinIntoString ("\n") << "\n"
              << "RUNNABLE: " << runnable << " (" << runnableNewIdentity << " as a new identity at the installed version, "
              << runnableDiscovered << " discovered with no fixture at all)\n"
              << rows.joinIntoString ("\n") << "\nNOT RUNNABLE, BY REASON\n";
    for (const auto& [why, names] : notRunnable)
        std::cout << "  " << why << " (" << names.size() << "): " << names.joinIntoString (", ") << "\n";
    std::cout << std::flush;
    return 0;
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
