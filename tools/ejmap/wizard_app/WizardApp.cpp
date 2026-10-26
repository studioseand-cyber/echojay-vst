/*
  EchoJay Wizard - the window app (docs/WIZARD_SPEC.md, Kathy 11 Oct; feat/ejmap-wizard). A separate Mac app, nothing to do with the
  plugin: plain window, the EchoJay logo, one step at a time.

    Welcome + sign in -> Scan (a Logic-style bar, each plugin's name as it is checked) -> "We found N plugins we haven't mapped yet" (14 Oct: no times on screen)
    -> Measuring (the plugin's name, time left; the Mac kept awake; closing pauses, reopening carries on) -> Upload -> Reward.
    A wizard that went down with a plugin loaded says so on the next launch, skips that plugin for good and carries on.

  The ENGINE is EJ Map's wizard (`ejmap --wizard`, beside this app's executable, with EchoJayProbe beside it): this app starts it in its
  own process group, reads its progress.json, and stops it with SIGINT (Phase B's clean stop) when the window closes.
  THE SERVER IS STUBBED (spec section 6): sign-in = a local account file; "which do you already have" = the mock database folder handed
  to the engine (--db); upload = the bundle copied into <work>/server_stub/uploads; the promo code = a local ledger, one per account.
  Nothing leaves the Mac.

    "EchoJay Wizard" [--work DIR] [--db DIR] [--store DIR] [--engine PATH] [--jobs N] [--only NAME]... [--snapshots DIR] [--auto]
  --auto (rehearsals only): fills the stub sign-in and presses each step's button after a pause, so a run needs no hands;
  --auto-close-after S: presses the close button S seconds into measuring (the pause path, as a user closing the window).
  --auto-send-at N / --auto-pause-at N: presses Send what's done / Pause once N plugins are measured so far (13 Oct).
  --checkpoint-minutes M: the quiet checkpoint's interval (default 60).
  --snapshots: every screen (and the bars at each quarter) saved as a PNG of the window's content.
*/
#include <libproc.h>
#include <CoreGraphics/CoreGraphics.h>   // 17 Oct: does the DAW have a window (CGWindowList, owner pids only)
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>
#include "EchoJayLogo.h"
#include "EjmapWizardApp.h"
#include "EjmapWizardProgress.h"
#include "EjmapWizardServer.h"
#include "EjmapWizardContract.h"   // 21 Oct: the beta's real server (contract v1)
#include "EjmapKeychain.h"
#include <mutex>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>   // 23 Oct: the battery
#include <IOKit/ps/IOPSKeys.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <signal.h>
#include <atomic>
#include <csignal>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

extern char** environ;

namespace
{
using namespace ejmap;
using Screen = wizapp::Screen;

// THE BUILD'S MODE (Kathy, 13 Oct, beta item 4): the beta build (target ejmap-wizard-beta, EJ_WIZARD_BETA) saves the results as one file
// to email instead of uploading; no sign-in, no promo. The normal build keeps the stubbed server.
#if EJ_WIZARD_BETA
constexpr auto kMode = wizapp::Mode::beta;
#else
constexpr auto kMode = wizapp::Mode::normal;
#endif
// THE TEST BUILD (Kathy, 14 Oct, target ejmap-wizard-test, EJ_WIZARD_TEST): the release flow with the stub server inside, seeded with the
// beta snapshot; a banner on every screen; its own folder, icon, editable server config and a Reset test button
#if EJ_WIZARD_TEST
constexpr bool kTest = true;
#else
constexpr bool kTest = false;
#endif
// THE SERVER, PER BUILD (Kathy, 21 Oct): the beta's base URL and public OAuth client id come from CMake (EJ_WIZARD_BETA_SERVER_URL /
// EJ_WIZARD_BETA_CLIENT_ID). No secret is ever compiled in: a device-code client is public
#ifndef EJ_WIZARD_SERVER_URL
#define EJ_WIZARD_SERVER_URL ""
#endif
#ifndef EJ_WIZARD_OAUTH_CLIENT_ID
#define EJ_WIZARD_OAUTH_CLIENT_ID ""
#endif
constexpr bool kNetworked = kMode == wizapp::Mode::beta;   // 21 Oct: the beta's contribution flow is real; the Test build keeps its stub
#ifndef EJ_WIZARD_BETA_EMAIL
#define EJ_WIZARD_BETA_EMAIL "admin@signaturestudiosonline.com"   // Kathy, 13 Oct
#endif

struct Config
{
    juce::File work, db, store, engine, snapshots, reports;
    juce::String serverUrl { EJ_WIZARD_SERVER_URL }, clientId { EJ_WIZARD_OAUTH_CLIENT_ID }; bool checkpointSet = false;   // 21 Oct
    int jobs = 0; juce::StringArray only; bool autoMode = false; double autoCloseAfterS = 0.0, checkpointMinutes = 60.0; int autoSendAt = 0, autoPauseAt = 0; bool autoAskClose = false, autoResetTest = false; double snapScanAtPct = -1.0;   // jobs 0: chosen by the Mac (13 Oct)
};

// THE TEST BUILD's server: the stub db seeded from the bundled snapshot (Contents/Resources/beta_db), the editable config written once
inline void seedTestBuild (const Config& c)
{
    const auto snap = juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources/beta_db");
    wizapp::seedTestServer (snap, c.db); wizapp::ensureTestConfig (c.work);
    wizserver::stub::testOverrides() = true;   // test_qualify_minimum is honoured (only here)
}
// THE MAC (13 Oct, jobs by Mac): performance cores (Apple silicon's perflevel0; an Intel Mac's physical cores) and the memory free now
inline int performanceCores()
{
    int n = 0; size_t sz = sizeof n;
    if (sysctlbyname ("hw.perflevel0.physicalcpu", &n, &sz, nullptr, 0) == 0 && n > 0) return n;
    sz = sizeof n; if (sysctlbyname ("hw.physicalcpu", &n, &sz, nullptr, 0) == 0) return n;
    return 1;
}
inline double availableMemoryBytes()
{
    vm_statistics64_data_t st {}; mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT; vm_size_t page = 0;
    if (host_page_size (mach_host_self(), &page) != KERN_SUCCESS || host_statistics64 (mach_host_self(), HOST_VM_INFO64, (host_info64_t) &st, &cnt) != KERN_SUCCESS) return -1.0;
    return (double) (st.free_count + st.inactive_count + st.purgeable_count) * (double) page;
}
// THE OPEN DAW (15 Oct; 17 Oct "DAW PAUSE": by bundle id, main executable only - a helper inside Logic Pro.app is not Logic). Read only: the
// process list, each outermost .app's Info.plist (CFBundleIdentifier, CFBundleExecutable; cached per app), and whether the DAW's pid owns
// a normal window (CGWindowList - owner pids and layers, no screen recording). `ignorePid`: the one the user chose to carry on beside
static bool appInfo (const juce::String& app, juce::String& bundleId, juce::String& exe)
{
    static std::map<juce::String, std::pair<juce::String, juce::String>> cache;
    if (const auto it = cache.find (app); it != cache.end()) { bundleId = it->second.first; exe = it->second.second; return bundleId.isNotEmpty(); }
    bundleId = {}; exe = {};
    auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) app.toRawUTF8(), (CFIndex) app.getNumBytesAsUTF8(), true);
    if (url != nullptr)
    {
        if (auto b = CFBundleCreate (nullptr, url))
        {
            if (auto id = CFBundleGetIdentifier (b)) bundleId = juce::String::fromCFString (id);
            if (auto e = (CFStringRef) CFBundleGetValueForInfoDictionaryKey (b, kCFBundleExecutableKey); e != nullptr && CFGetTypeID (e) == CFStringGetTypeID()) exe = juce::String::fromCFString (e);
            CFRelease (b);
        }
        CFRelease (url);
    }
    cache[app] = { bundleId, exe }; return bundleId.isNotEmpty();
}
static bool hasWindow (int pid)
{
    bool found = false;
    if (auto list = CGWindowListCopyWindowInfo (kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
    {
        for (CFIndex i = 0; i < CFArrayGetCount (list) && ! found; ++i)
        {
            auto w = (CFDictionaryRef) CFArrayGetValueAtIndex (list, i); int owner = 0, layer = -1;
            if (auto n = (CFNumberRef) CFDictionaryGetValue (w, kCGWindowOwnerPID)) CFNumberGetValue (n, kCFNumberIntType, &owner);
            if (auto n = (CFNumberRef) CFDictionaryGetValue (w, kCGWindowLayer)) CFNumberGetValue (n, kCFNumberIntType, &layer);
            found = owner == pid && layer == 0;   // a document / project window (layer 0); the menu bar and status items are other layers
        }
        CFRelease (list);
    }
    return found;
}
// THE POWER (Kathy, 23 Oct): what macOS says powers the Mac now, and the internal battery's charge (IOPowerSources; no permission asked).
// A Mac with no battery reports AC: known, never on battery
ejmap::wizapp::Power readPower()
{
    ejmap::wizapp::Power p;
    // REHEARSALS ONLY: EJ_WIZARD_POWER_FILE names a file holding "battery <percent>" or "ac" - the pause and the carry-on can be rehearsed
    // without unplugging the Mac (never set in a tester's run; unset, macOS is asked)
    if (const char* f = std::getenv ("EJ_WIZARD_POWER_FILE"); f != nullptr && *f)
    {   const auto t = juce::File (juce::String::fromUTF8 (f)).loadFileAsString().trim().toLowerCase();
        if (t.isNotEmpty()) { p.known = true; p.onBattery = t.startsWith ("battery"); p.percent = p.onBattery ? t.fromFirstOccurrenceOf (" ", false, false).getIntValue() : 100; return p; } }
    CFTypeRef info = IOPSCopyPowerSourcesInfo(); if (info == nullptr) return p;
    if (CFStringRef src = IOPSGetProvidingPowerSourceType (info)) { p.known = true; p.onBattery = CFStringCompare (src, CFSTR (kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo; }
    if (CFArrayRef list = IOPSCopyPowerSourcesList (info))
    {
        for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
        {
            CFDictionaryRef d = IOPSGetPowerSourceDescription (info, CFArrayGetValueAtIndex (list, i)); if (d == nullptr) continue;
            auto type = (CFStringRef) CFDictionaryGetValue (d, CFSTR (kIOPSTypeKey));
            if (type == nullptr || CFStringCompare (type, CFSTR (kIOPSInternalBatteryType), 0) != kCFCompareEqualTo) continue;
            int cur = 0, mx = 0;
            if (auto n = (CFNumberRef) CFDictionaryGetValue (d, CFSTR (kIOPSCurrentCapacityKey))) CFNumberGetValue (n, kCFNumberIntType, &cur);
            if (auto n = (CFNumberRef) CFDictionaryGetValue (d, CFSTR (kIOPSMaxCapacityKey))) CFNumberGetValue (n, kCFNumberIntType, &mx);
            if (mx > 0) p.percent = (int) std::lround (100.0 * cur / mx);
        }
        CFRelease (list);
    }
    CFRelease (info);
    return p;
}
ejmap::wizapp::DawHit openDaw (int ignorePid = 0)
{
    ejmap::wizapp::DawHit h;
    int n = proc_listallpids (nullptr, 0); if (n <= 0) return h;
    std::vector<pid_t> pids ((size_t) n + 32); n = proc_listallpids (pids.data(), (int) (pids.size() * sizeof (pid_t)));
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (int i = 0; i < n; ++i)
    {
        const auto pid = pids[(size_t) i]; if (pid <= 0 || pid == ignorePid || proc_pidpath (pid, path, sizeof (path)) <= 0) continue;
        const auto exec = juce::String::fromUTF8 (path); const auto app = ejmap::wizapp::outermostApp (exec); if (app.isEmpty()) continue;
        juce::String id, exe; if (! appInfo (app, id, exe)) continue;
        const auto name = ejmap::wizapp::dawFromBundleId (id);
        if (name.isEmpty() || ! ejmap::wizapp::isMainExecutable (exec, app, exe)) continue;   // a helper inside the app is not the DAW
        h.name = name; h.bundleId = id; h.path = exec; h.pid = (int) pid; h.windows = hasWindow ((int) pid); return h;
    }
    return h;
}
juce::File homeChild (const juce::String& p) { return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (p); }

Config parseArgs (const juce::StringArray& a)
{
    Config c;
    c.work = homeChild ("Library/Application Support/" + wizapp::workFolderName (kMode, kTest));   // 13 Oct: beta and release apart; 14 Oct: the test build
    c.engine = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("ejmap");
    c.reports = homeChild ("Library/Logs/DiagnosticReports");
    for (int i = 0; i < a.size(); ++i)
    {
        const auto k = a[i]; const auto v = i + 1 < a.size() ? a[i + 1] : juce::String();
        auto file = [&] { ++i; return juce::File::isAbsolutePath (v) ? juce::File (v) : juce::File::getCurrentWorkingDirectory().getChildFile (v); };
        if (k == "--work") c.work = file(); else if (k == "--db") c.db = file(); else if (k == "--store") c.store = file(); else if (k == "--engine") c.engine = file();
        else if (k == "--snapshots") c.snapshots = file(); else if (k == "--reports") c.reports = file();
        else if (k == "--jobs") { c.jobs = juce::jlimit (1, 8, v.getIntValue()); ++i; } else if (k == "--only") { c.only.add (v); ++i; }
        else if (k == "--auto") c.autoMode = true;
        else if (k == "--auto-send-at") { c.autoSendAt = v.getIntValue(); ++i; }     // rehearsals: Send what's done once N are measured so far
        else if (k == "--auto-pause-at") { c.autoPauseAt = v.getIntValue(); ++i; }   // rehearsals: Pause once N are measured so far (then quits)
        else if (k == "--auto-ask-close") c.autoAskClose = true;
        else if (k == "--auto-reset-test") c.autoResetTest = true;
        else if (k == "--snap-scan-at") { c.snapScanAtPct = v.getDoubleValue(); ++i; }   // rehearsals: snap the scan screen when its bar first reaches N %   // rehearsals (test build): press Reset test once, snap the welcome screen, quit   // rehearsals: open the close question once while measuring, snap it, keep measuring
        else if (k == "--checkpoint-minutes") { c.checkpointMinutes = v.getDoubleValue(); c.checkpointSet = true; ++i; }
        else if (k == "--server-url") { if (ejmap::wizcontract::localOverrideAllowed (v)) c.serverUrl = v; ++i; }   // 21 Oct: rehearsals against the mock - THIS Mac only, never another server
        else if (k == "--client-id") { if (ejmap::wizcontract::localOverrideAllowed (c.serverUrl)) c.clientId = v; ++i; }   // rehearsals: the quiet checkpoint's interval
        else if (k == "--auto-close-after") { c.autoCloseAfterS = v.getDoubleValue(); ++i; }   // rehearsals: the close button, pressed after N s of measuring
    }
    if (c.jobs <= 0) c.jobs = wizapp::jobsFor (performanceCores(), availableMemoryBytes());   // 13 Oct: jobs by Mac
    if (kNetworked && ! c.checkpointSet) c.checkpointMinutes = 2.0;   // 21 Oct: the results flow out every ~2 min while measuring
    const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    if (c.db == juce::File()) c.db = wizapp::databaseFolder (kMode, app, c.work);       // beta: the bundled snapshot (read-only); release: the STUB
    if (c.store == juce::File()) c.store = c.work.getChildFile ("store");                // the wizard's own map store (never ~/Library/ejmap)
    if (! c.db.isAChildOf (app))
    {   // 13 Oct: the mock DB's 290 agreed references, from the app's Resources (assemble.sh), into the STUB db only (never into the bundle)
        const auto res = juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources/mock_db/references");
        wizserver::stub::seedMockReferences (res, c.db);
    }
    if (kTest) seedTestBuild (c);
    if (kTest && c.only.isEmpty()) c.only = wizapp::testOnlyPlugins (c.work);   // 14 Oct: the small run (test_only_plugins)
    return c;
}

// --- THE STUBBED SERVER: EjmapWizardServer.h (the base URL https://www.echojay.ai, both upload paths, the category table) -----------
namespace stub
{
inline juce::var signIn (const juce::File& w, const juce::String& e, const juce::String& p) { return wizserver::stub::signIn (w, e, p); }
inline juce::var upload (const juce::File& w, const juce::File& z, const juce::File& m, const juce::var& a, const juce::String& dev) { return wizserver::stub::upload (w, z, m, a, dev); }
inline juce::var promo (const juce::File& w, const juce::var& a, const juce::var& r) { return wizserver::stub::promo (w, a, r); }
inline juce::var lookup (const juce::File& w, const juce::var& a, const juce::String& dev, const juce::var& state) { return wizserver::stub::lookupEligibility (w, a.getProperty ("account_id", "").toString(), dev, state); }
} // namespace stub

// --- THE ENGINE: ejmap --wizard in its own process group --------------------------------------------------------------------------------
struct Engine
{
    pid_t pid = -1, group = -1; juce::String mode;   // group: the process group it started (its first pid) - kept after it ends, to close what is left
    bool start (const juce::File& exe, const juce::StringArray& args, const juce::File& log, const juce::String& m)
    {
        std::vector<std::string> sv { exe.getFullPathName().toStdString() }; for (const auto& a : args) sv.push_back (a.toStdString());
        std::vector<char*> argv; for (auto& s : sv) argv.push_back (s.data()); argv.push_back (nullptr);
        posix_spawnattr_t at; posix_spawnattr_init (&at); posix_spawnattr_setflags (&at, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup (&at, 0);
        posix_spawn_file_actions_t fa; posix_spawn_file_actions_init (&fa);
        const auto lp = log.getFullPathName().toStdString();
        posix_spawn_file_actions_addopen (&fa, 1, lp.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); posix_spawn_file_actions_adddup2 (&fa, 1, 2);
        const int rc = posix_spawn (&pid, argv[0], &fa, &at, argv.data(), environ);
        posix_spawn_file_actions_destroy (&fa); posix_spawnattr_destroy (&at);
        if (rc != 0) { pid = -1; return false; }
        mode = m; group = pid; return true;
    }
    bool running() const { return pid > 0; }
    std::optional<int> poll()   // the exit code once it has ended
    {
        if (pid <= 0) return std::nullopt; int st = 0;
        if (waitpid (pid, &st, WNOHANG) == pid) { pid = -1; return WIFEXITED (st) ? WEXITSTATUS (st) : 128 + WTERMSIG (st); }
        return std::nullopt;
    }
    void pause() { if (pid > 0) kill (-pid, SIGINT); }   // Phase B's clean stop: no new plugin, the in-flight ones run again next time
};

// --- THE MAC'S DEVICE HASH (items 7 / 8): the hardware UUID (IOPlatformUUID), salted and hashed (wizserver::deviceHashOf) - the raw UUID
// never leaves the Mac. EJ_WIZARD_FAKE_DEVICE stands in for it in rehearsals (a "second Mac").
inline juce::String platformUuid()
{
    if (const char* f = std::getenv ("EJ_WIZARD_FAKE_DEVICE"); f != nullptr && *f) return f;
    juce::String out; io_service_t svc = IOServiceGetMatchingService (kIOMainPortDefault, IOServiceMatching ("IOPlatformExpertDevice"));
    if (svc) { if (CFTypeRef u = IORegistryEntryCreateCFProperty (svc, CFSTR ("IOPlatformUUID"), kCFAllocatorDefault, 0)) { char b[128] = {}; if (CFStringGetCString ((CFStringRef) u, b, sizeof b, kCFStringEncodingUTF8)) out = b; CFRelease (u); } IOObjectRelease (svc); }
    return out;
}

// --- CTRL-C (Kathy, 13 Oct): the app launched from a Terminal - SIGINT takes the close button's path (the engine is in its own group)
inline std::atomic<int>& appInterrupted() { static std::atomic<int> f { 0 }; return f; }

// --- KEEP THE MAC AWAKE while the engine runs ----------------------------------------------------------------------------------------
struct KeepAwake
{
    IOPMAssertionID id = 0; bool held = false;
    void hold (bool on)
    {
        if (on && ! held) held = IOPMAssertionCreateWithName (kIOPMAssertPreventUserIdleSystemSleep, kIOPMAssertionLevelOn, CFSTR ("EchoJay Wizard is measuring your plugins"), &id) == kIOReturnSuccess;
        else if (! on && held) { IOPMAssertionRelease (id); held = false; }
    }
    ~KeepAwake() { hold (false); }
};

// --- THE WINDOW'S CONTENT ------------------------------------------------------------------------------------------------------------
class WizardComponent : public juce::Component, private juce::Timer
{
public:
    explicit WizardComponent (Config c) : cfg (std::move (c))
    {
        cfg.work.createDirectory();
        logo = juce::ImageCache::getFromMemory (echoJayLogoPNG, echoJayLogoPNGSize);
        for (auto* b : { &primary, &secondary }) { addChildComponent (*b); b->setColour (juce::TextButton::buttonColourId, accent); b->setColour (juce::TextButton::textColourOffId, juce::Colours::white); }
        secondary.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe9ecf2)); secondary.setColour (juce::TextButton::textColourOffId, ink);
        for (auto* e : { &email, &password })
        { addChildComponent (*e); e->setFont (juce::FontOptions (15.0f)); e->setIndents (10, 10);
          e->setColour (juce::TextEditor::backgroundColourId, juce::Colours::white); e->setColour (juce::TextEditor::textColourId, ink);
          e->setColour (juce::TextEditor::outlineColourId, juce::Colour (0xffc9cfdb)); e->setColour (juce::TextEditor::focusedOutlineColourId, accent);
          e->setColour (juce::CaretComponent::caretColourId, ink); }
        email.setTextToShowWhenEmpty ("Email", juce::Colours::grey); password.setTextToShowWhenEmpty ("Password", juce::Colours::grey); password.setPasswordCharacter ((juce::juce_wchar) 0x2022);
        primary.onClick = [this] { onPrimary(); }; secondary.onClick = [this] { onSecondary(); };
        addChildComponent (anyway); anyway.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe9ecf2)); anyway.setColour (juce::TextButton::textColourOffId, ink);   // 17 Oct
        anyway.onClick = [this] { onAnyway(); };
        addChildComponent (resetTestButton); resetTestButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe9ecf2)); resetTestButton.setColour (juce::TextButton::textColourOffId, ink);
        resetTestButton.onClick = [this] { resetTestNow(); };
        addChildComponent (agree); agree.setButtonText (wizapp::consentTick()); agree.setColour (juce::ToggleButton::textColourId, ink); agree.setColour (juce::ToggleButton::tickColourId, accent);
        agree.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff8a90a0)); agree.onClick = [this] { primary.setEnabled (agree.getToggleState()); };
        device = wizserver::deviceHashOf (platformUuid());
        if (kNetworked && ep().configured())
        {   // 21 Oct: the tokens live only in the Keychain (one item per server); the session logs through the app log, redacted
            keychain = std::make_unique<wizcontract::KeychainTokenStore> (wizcontract::keychainServiceFor (ep().base));
            session = std::make_unique<wizcontract::Session> (ep(), wizcontract::urlTransport (30000), *keychain, [this] (const juce::String& l) { logLine ("server: " + l); }, "0.3.0");
        }
        setSize (640, 560);   // 15 Oct (FEELS FROZEN): room for the job lines under the measuring bar
        // WHERE TO OPEN: from the files alone (a crash first)
        crashed = wizapp::recover (cfg.work, engineDir().getChildFile ("inflight"), cfg.reports, juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(),
                                   juce::SystemStats::getLogonName(), hostName());
        wizapp::Where w; const auto st = appState();
        if (const auto given = cfg.only; (cfg.only = wizapp::runOnlyList (given, st)) != given) logLine ("this run's own plugin list (saved at its start): " + cfg.only.joinIntoString (", "));   // 26 Oct
        w.signedIn = kNetworked ? (session != nullptr && session->signedIn()) : cfg.work.getChildFile ("account.json").existsAsFile(); w.scanned = (bool) st.getProperty ("scanned", false);
        w.measureStarted = (bool) st.getProperty ("measure_started", false); w.measureDone = (bool) st.getProperty ("measure_done", false);
        w.uploaded = (bool) st.getProperty ("uploaded", false); w.rewarded = cfg.work.getChildFile ("reward.json").existsAsFile(); w.crashed = (int) crashed.size();
        w.consented = (bool) st.getProperty ("consented", false); w.saved = (bool) st.getProperty ("saved", false);
        logLine ("open (" + juce::String (kMode == wizapp::Mode::beta ? "beta" : "normal") + "): " + wizapp::screenName (wizapp::openOn (w, kMode)) + (crashed.empty() ? juce::String() : " (" + juce::String ((int) crashed.size()) + " plugin(s) put on the never-retry list)"));
        if (const int n = drainOutbox(); n > 0)
        {   // 13 Oct: what an offline run kept is sent now; a finished run's code arrives with it
            logLine ("launch: " + juce::String (n) + " kept upload(s) sent");
            w.uploaded = (bool) appState().getProperty ("uploaded", false); w.rewarded = cfg.work.getChildFile ("reward.json").existsAsFile();
        }
        if (kTest && ! cfg.only.isEmpty()) logLine ("small run (test_only_plugins): " + cfg.only.joinIntoString (", ") + " | qualify minimum " + juce::String ((int) wizserver::stub::serverConfig (cfg.work).getProperty ("qualify_minimum", 0)));
        logLine ("jobs: " + juce::String (cfg.jobs) + " (" + juce::String (performanceCores()) + " performance cores, " + juce::String (availableMemoryBytes() / 1073741824.0, 1) + " GB free)");
        show (wizapp::openOn (w, kMode, wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString()))));   // 15 Oct: a finished scan carries on into measuring
        startTimer (250);
        if (kNetworked) cacheRun();   // 26 Oct (staging run 1): the run id FROM DISK before the uploader starts - a reopened wizard sent nothing
        if (kNetworked && session != nullptr && (bool) appState().getProperty ("consented", false)) uploader.startThread();   // 21 Oct: what waits in the outbox goes
        shimmer.tick = [this] { if (wizapp::shimmerOn (screen, pausing, finishing)) repaint(); };   // 15 Oct: the wave by the title and the job lines too
        shimmer.startTimerHz (30);   // 15 Oct: the shimmer moves even while one plugin takes 20 minutes
    }
    ~WizardComponent() override { closing = true; uploader.stopThread (4000); net.removeAllJobs (true, 8000); stopTimer(); awake.hold (false); if (engine.running()) engine.pause();
                                  else if (const auto left = wizapp::closeProcessGroup (engine.group); ! left.isEmpty()) logLine ("quit: closed what the engine left running: " + left.joinIntoString (", ")); }   // 19 Oct   // 14 Oct: never leave an engine running

    // closing the window PAUSES: the engine stops cleanly (Phase B's SIGINT), the app quits when it has
    bool requestQuit (bool askFirst = true)
    {
        if (! engine.running()) return true;
        if (askFirst && ! quitting && ! finishing && ! pausing && screen == Screen::measuring && closeAsk == nullptr && (! cfg.autoMode || forceAsk))
        {   // 13 Oct (Kathy): closing while measuring asks - pause (the default) or send what's done; Escape keeps measuring
            const auto ch = wizapp::measuringChoices (kMode);
            closeAsk = std::make_unique<juce::AlertWindow> ("Stop measuring for now?", wizapp::percentDoneWords (shownPct) + ". " + ch.line, juce::MessageBoxIconType::NoIcon, this);
            closeAsk->addButton (ch.pause, 1, juce::KeyPress (juce::KeyPress::returnKey)); closeAsk->addButton (ch.send, 2); closeAsk->addButton ("Keep measuring", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            closeAsk->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<WizardComponent> (this)] (int r)
            { if (safe == nullptr) return; safe->closeAsk.reset();
              if (r == 1) safe->pauseNow ("the close button: pause", true);
              else if (r == 2) safe->finishNow ("the close button: send what's done"); }));
            if (cfg.snapshots != juce::File()) juce::Timer::callAfterDelay (500, [safe = juce::Component::SafePointer<WizardComponent> (this)] { if (safe != nullptr && safe->closeAsk != nullptr) safe->snapWindow (*safe->closeAsk, "close_question"); });
            return false;
        }
        if (quitting) return false;
        if (screen == Screen::measuring && ! finishing) { pauseNow ("quit", true); return false; }   // a pause: its checkpoint is packed, then the app quits
        if (! quitting) { quitting = true; engine.pause(); pauseNote = wizapp::stoppedNote(); logLine ("close: engine stopped (SIGINT) on " + wizapp::screenName (screen)); repaint(); }
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::white);
        if (kTest)
        {   // the TEST banner, on every screen
            g.setColour (juce::Colour (0xfff5731a)); g.fillRect (0, 0, getWidth(), 22);
            g.setColour (juce::Colours::white); g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            g.drawText (wizapp::testBanner(), juce::Rectangle<int> (0, 0, getWidth(), 22), juce::Justification::centred);
        }
        if (powerNotice.isNotEmpty())
        {   // 23 Oct (THE BATTERY): on every screen while the Mac is on battery, under the buttons (never over them); the run goes on
            const auto pill = juce::Rectangle<float> ((float) getWidth() / 2.0f - 200.0f, (float) getHeight() - 27.0f, 400.0f, 22.0f);
            g.setColour (juce::Colour (0xfffff4e0)); g.fillRoundedRectangle (pill, 11.0f);
            g.setColour (juce::Colour (0xffb26a00)); g.drawRoundedRectangle (pill, 11.0f, 1.0f);
            g.setFont (juce::FontOptions (12.5f, juce::Font::bold)); g.drawText (powerNotice, pill.toNearestInt(), juce::Justification::centred);
        }
        const auto r = getLocalBounds().reduced (40, 28);
        if (logo.isValid())   // the logo is white on transparent (made for the plugin's dark UI): drawn in ink through its alpha
        { const float w = 210.0f, h = w * (float) logo.getHeight() / (float) logo.getWidth(); g.setColour (ink);
          g.drawImage (logo, juce::Rectangle<float> ((float) getWidth() / 2.0f - w / 2.0f, 30.0f, w, h), juce::RectanglePlacement::centred, true); }
        auto body = r.withTrimmedTop (92);
        if ((screen == Screen::scan || screen == Screen::measuring) && stepText.isNotEmpty())   // 18 Oct (NEVER LOOKS FROZEN): the step, above the title
        { g.setColour (accent); g.setFont (juce::FontOptions (12.5f, juce::Font::bold)); g.drawText (stepText, juce::Rectangle<int> (40, 100, getWidth() - 80, 16), juce::Justification::centred); }
        g.setColour (ink); g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        g.drawFittedText (title, body.removeFromTop (40), juce::Justification::centred, 2);
        g.setColour (juce::Colour (0xff4a5163)); g.setFont (juce::FontOptions (15.5f));
        g.drawFittedText (text, body.removeFromTop (screen == Screen::consent ? 150 : 64).reduced (10, 0), juce::Justification::centredTop, screen == Screen::consent ? 9 : 3);
        if (showBar)
        {
            const float barY = screen == Screen::measuring ? 236.0f : 268.0f; const int labelY = (int) barY + 18;   // 15 Oct: room for the job lines
            const auto bar = juce::Rectangle<float> (70.0f, barY, (float) getWidth() - 140.0f, 10.0f);
            g.setColour (juce::Colour (0xffe6e9ef)); g.fillRoundedRectangle (bar, 5.0f);
            g.setColour (accent); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, barValue)), 5.0f);
            if (wizapp::shimmerOn (screen, pausing, finishing))
            {   // 15 Oct: the "it's working" shimmer - a soft light band drifting along the bar, calm, every 3.2 s
                const double ph = wizapp::shimmerPhase (juce::Time::getMillisecondCounterHiRes());
                if (ph >= 0.0)
                {
                    const float cx = bar.getX() + (float) ph * bar.getWidth(), half = 60.0f;
                    juce::ColourGradient grad (juce::Colours::white.withAlpha (0.0f), cx - half, bar.getY(), juce::Colours::white.withAlpha (0.0f), cx + half, bar.getY(), false);
                    grad.addColour (0.5, juce::Colours::white.withAlpha (0.55f));
                    juce::Graphics::ScopedSaveState ss (g); juce::Path clip; clip.addRoundedRectangle (bar, 5.0f); g.reduceClipRegion (clip);
                    g.setGradientFill (grad); g.fillRect (juce::Rectangle<float> (cx - half, bar.getY(), 2.0f * half, bar.getHeight()));
                }
            }
            g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (13.5f));
            {   // 15 Oct (Kathy: "APB Royal Mu" cut off): the % keeps only the room it needs on the right; the name gets the rest of the width
                // and shrinks a little rather than being cut ("..."), one line, never under 70% size
                const juce::Font f (juce::FontOptions (13.5f)); const int rightW = barRight.isEmpty() ? 0 : (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, barRight)) + 14;
                g.drawText (barRight, juce::Rectangle<int> (70, labelY, getWidth() - 140, 20), juce::Justification::centredRight);
                g.drawFittedText (barLeft, juce::Rectangle<int> (70, labelY, getWidth() - 140 - rightW, 20), juce::Justification::centredLeft, 1, 0.7f);
            }
        }
        if (screen == Screen::measuring && showBar && ! pausing && ! finishing)
        {   // 15 Oct (FEELS FROZEN): each running job on its own line - its name and a small bar of its steps; the plugins done; the live words
            int y = 282;
            // 18 Oct (NEVER LOOKS FROZEN): the now line first - plugin + what it is doing (between passes: say so, never a blank gap)
            { g.setColour (juce::Colour (0xff4a5163)); g.setFont (juce::FontOptions (13.0f));
              // 18 Oct (BAR % WRONG): only while no job line names a plugin - never the same name twice
              const auto nw = ! jobsNow.empty() ? juce::String() : nowText.isNotEmpty() ? nowText : ! doneLine.isEmpty() ? wizapp::betweenPluginsWords() : juce::String();
              g.drawFittedText (nw, juce::Rectangle<int> (70, y, getWidth() - 140, 17), juce::Justification::centred, 1, 0.7f); y += 19; }
            int shownJobs = 0;
            for (const auto& j : jobsNow)
            {
                if (++shownJobs > 3) break;   // 18 Oct: three under the now line leave the counter its room
                g.setColour (juce::Colour (0xff4a5163)); g.setFont (juce::FontOptions (13.0f));
                g.drawFittedText (j.name, juce::Rectangle<int> (70, y, 250, 17), juce::Justification::centredLeft, 1, 0.7f);
                const auto mini = juce::Rectangle<float> (330.0f, (float) y + 6.0f, (float) getWidth() - 400.0f, 5.0f);
                if (j.fraction >= 0.0)
                {   g.setColour (juce::Colour (0xffe6e9ef)); g.fillRoundedRectangle (mini, 2.5f);
                    g.setColour (accent.withAlpha (0.75f)); g.fillRoundedRectangle (mini.withWidth (mini.getWidth() * (float) juce::jlimit (0.0, 1.0, j.fraction)), 2.5f); }
                else { g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (12.0f)); g.drawText ("checking", mini.toNearestInt().withHeight (17).translated (0, -6), juce::Justification::centredLeft); }
                y += 19;
            }
            g.setColour (ink); g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
            if (doneLine.isNotEmpty()) g.drawText (doneLine, juce::Rectangle<int> (70, 362, getWidth() - 140, 18), juce::Justification::centred);
            const auto lw = wizapp::liveWords (wizapp::liveState (juce::Time::currentTimeMillis(), heartbeatMs));
            if (lw.isNotEmpty()) { g.setColour (juce::Colour (0xffb26a00)); g.setFont (juce::FontOptions (13.0f)); g.drawText (lw, juce::Rectangle<int> (40, 382, getWidth() - 80, 18), juce::Justification::centred); }
        }
        if (screen == Screen::measuring || screen == Screen::scan)
        {   // 15 Oct (FEELS FROZEN): a soft sound wave beside the title, moving only while the engine's heartbeat is live (18 Oct: the scan too)
            const bool live = ! pausing && ! finishing && wizapp::liveState (juce::Time::currentTimeMillis(), heartbeatMs) == wizapp::Live::live;
            const juce::Font tf (juce::FontOptions (24.0f, juce::Font::bold)); const float tw = juce::GlyphArrangement::getStringWidth (tf, title);
            const float x0 = (float) getWidth() / 2.0f + tw / 2.0f + 14.0f, yc = 120.0f + 20.0f; const double ms = juce::Time::getMillisecondCounterHiRes();
            g.setColour (accent.withAlpha (live ? 0.8f : 0.35f));
            for (int k = 0; k < 5; ++k) { const float h = wizapp::waveHeight (k, ms, live); g.fillRoundedRectangle (x0 + (float) k * 5.0f, yc - h, 3.0f, 2.0f * h, 1.5f); }
        }
        if (bigCode.isNotEmpty())
        {
            const auto box = juce::Rectangle<float> ((float) getWidth() / 2.0f - 150.0f, 250.0f, 300.0f, 54.0f);
            g.setColour (juce::Colour (0xfff2f5fb)); g.fillRoundedRectangle (box, 10.0f); g.setColour (accent); g.drawRoundedRectangle (box, 10.0f, 1.5f);
            g.setColour (ink); g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::bold)); g.drawText (bigCode, box, juce::Justification::centred);
        }
        if (screen == Screen::scan)   // 14 Oct: the licence-window warning
        { // 18 Oct (NEVER LOOKS FROZEN): the now line and the ticking counter under the bar, as on the measuring screen
          g.setColour (juce::Colour (0xff4a5163)); g.setFont (juce::FontOptions (13.0f)); g.drawFittedText (nowText, juce::Rectangle<int> (70, 310, getWidth() - 140, 17), juce::Justification::centred, 1, 0.7f);
          g.setColour (ink); g.setFont (juce::FontOptions (13.5f, juce::Font::bold)); g.drawText (doneLine, juce::Rectangle<int> (70, 330, getWidth() - 140, 18), juce::Justification::centred);
          g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (13.5f)); g.drawFittedText (wizapp::scanWarning(), juce::Rectangle<int> (70, 352, getWidth() - 140, 40), juce::Justification::centred, 2);
          // 18 Oct (LOOKING FOR PLUGINS): the same 2 min "still working" / 15 min "may be stuck" lines as measuring
          const auto lw = wizapp::liveWords (wizapp::liveState (juce::Time::currentTimeMillis(), heartbeatMs));
          if (lw.isNotEmpty()) { g.setColour (juce::Colour (0xffb26a00)); g.setFont (juce::FontOptions (13.0f)); g.drawText (lw, juce::Rectangle<int> (40, 396, getWidth() - 80, 18), juce::Justification::centred); } }
        if (screen == Screen::paused && anyway.isVisible() && dawPaused.isNotEmpty())   // 17 Oct: Carry on anyway's one-line warning
        { g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (12.0f)); g.drawFittedText (wizapp::carryOnAnywayWarning (dawPaused), juce::Rectangle<int> (30, getHeight() - 182, getWidth() - 60, 20), juce::Justification::centred, 1); }
        if (screen == Screen::measuring && choiceLine.isNotEmpty() && ! finishing && ! pausing)
        { g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (13.0f)); g.drawFittedText (choiceLine, juce::Rectangle<int> (40, getHeight() - 150, getWidth() - 80, 20), juce::Justification::centred, 1); }
        if (pauseNote.isNotEmpty()) { g.setColour (juce::Colour (0xff6b7285)); g.setFont (juce::FontOptions (13.0f)); g.drawFittedText (pauseNote, juce::Rectangle<int> (40, getHeight() - 92, getWidth() - 80, 20), juce::Justification::centred, 1); }
    }
    void resized() override
    {
        primary.setBounds (getWidth() / 2 - 110, getHeight() - 70, 220, 40);
        secondary.setBounds (getWidth() / 2 - 110, getHeight() - 116, 220, 36);
        anyway.setBounds (getWidth() / 2 - 110, getHeight() - 158, 220, 30);   // 17 Oct: above the two, with its warning above it
        email.setBounds (getWidth() / 2 - 160, 250, 320, 38); password.setBounds (getWidth() / 2 - 160, 296, 320, 38);
        agree.setBounds (getWidth() / 2 - 60, getHeight() - 118, 160, 30);
        resetTestButton.setBounds (14, getHeight() - 40, 96, 26);
    }

private:
    Config cfg; Engine engine; KeepAwake awake;
    juce::Image logo; juce::TextButton primary, secondary, resetTestButton { "Reset test" }, anyway { wizapp::kCarryOnAnyway }; juce::TextEditor email, password; juce::ToggleButton agree; bool diskRefused = false; juce::String device; juce::File savedFile; juce::var eligibility;
    const juce::Colour accent { 0xff3d5afe }, ink { 0xff141824 };
    Screen screen = Screen::welcome; juce::String title, text, barLeft, barRight, bigCode, pauseNote; bool showBar = false; double barValue = 0.0;
    wizapp::Power power; bool batteryPaused = false, plugInFirst = false; double lastPowerCheck = -1.0e9; juce::String powerNotice, lastPowerLog;   // 23 Oct: the battery
    juce::String dawPaused; Screen resumeTo = Screen::measuring; double dawQuietSince = -1.0, lastDawCheck = 0.0; ejmap::wizapp::DawHit dawHit; int dawIgnoredPid = 0;   // 17 Oct: the hit (for the words and the log), the DAW the user chose to carry on beside   // 15 Oct: a pause the DAW caused, and where it carries on
    double shownPctT = 0.0; std::vector<wizapp::JobLine> jobsNow; juce::String doneLine; juce::int64 heartbeatMs = 0;   // 15 Oct (FEELS FROZEN)
    wizapp::StepBar stepBar; juce::String stepText, nowText, lastSnapStep;
    void resetShownProgress() { shownPct = 0; shownPctT = 0.0; stepBar = {}; jobsNow.clear(); doneLine.clear(); nowText.clear(); stepText.clear(); lastSnapStep.clear(); }   // a NEW run starts from 0   // 18 Oct (NEVER LOOKS FROZEN): the step line, the now line
    void snapOnStep (const juce::String& key)   // rehearsals (--snapshots): one picture a few seconds into each stage
    { if (cfg.snapshots == juce::File() || key == lastSnapStep) return; lastSnapStep = key;
      juce::Timer::callAfterDelay (1200, [safe = juce::Component::SafePointer<WizardComponent> (this), key] { if (safe != nullptr) safe->snap ("stage-" + juce::File::createLegalFileName (key)); }); }
    int shownPct = 0; juce::int64 readyStamp = 0;   // 15 Oct: the % shown (never backwards); wizard_state.json's time, for the ready line
    struct Shimmer : juce::Timer { std::function<void()> tick; void timerCallback() override { if (tick) tick(); } } shimmer;
    void refreshReadyLine()
    {   // 15 Oct: the ready line follows the engine's own count (the measuring run re-checks the Mac first: 504 at night, 509 this morning)
        const auto f = engineDir().getChildFile ("wizard_state.json"); const auto t = f.getLastModificationTime().toMilliseconds();
        if (t == readyStamp || ! f.existsAsFile()) return; readyStamp = t;
        const int n = (int) juce::JSON::parse (f.loadFileAsString()).getProperty ("to_measure", 0);
        if (n > 0 && ! pausing && ! finishing)
        {   text = wizapp::measuringText (n);
            if (kNetworked) { const auto el = wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString()));   // 21 Oct: the check's own words stay
                              text = wizapp::knownWords (el.known, n) + ". " + text; } }
    }
    std::vector<wizapp::Crashed> crashed; bool quitting = false, finishing = false, pausing = false, quitAfterPause = false, forceAsk = false, lastSendKept = false; juce::String soFar, choiceLine; int lastCheckpointSeq = 0; std::unique_ptr<juce::AlertWindow> closeAsk; int autoTicks = 0, snapN = 0, resetTicks = 0; double lastSnapQuarter = -1.0;
    double uploadT0 = 0.0; juce::var account, receipt; const juce::String launchTag = juce::Time::getCurrentTime().formatted ("%H%M%S");

    juce::File engineDir() const { return cfg.work.getChildFile ("engine"); }
    // ===== THE REAL SERVER (Kathy, 21 Oct; contract v1) - the beta only. Every network call runs on `net` (one job at a time, under netMutex);
    // the uploader thread drains the outbox; screens change on the message thread. No token is ever logged or written to a file =====
    wizcontract::Endpoints ep() const { return { cfg.serverUrl, cfg.clientId }; }
    // rehearsals against the mock only (a local --server-url AND EJ_WIZARD_REHEARSAL_QUIET): no browser opened, the clipboard left alone
    bool rehearsalQuiet() const { return std::getenv ("EJ_WIZARD_REHEARSAL_QUIET") != nullptr && wizcontract::localOverrideAllowed (cfg.serverUrl); }
    std::unique_ptr<wizcontract::KeychainTokenStore> keychain; std::unique_ptr<wizcontract::Session> session; std::mutex netMutex;
    wizcontract::DeviceStart deviceFlow; std::atomic<bool> closing { false };
    juce::File outboxDir() const { return cfg.work.getChildFile ("outbox"); }
    juce::File answersFile() const { return cfg.work.getChildFile ("catalogue_answers.json"); }
    juce::File requestFile() const { return cfg.work.getChildFile ("catalogue_request.json"); }
    juce::CriticalSection runLock; juce::String cachedRunId, cachedConsentAt, cachedStartedAt; int cachedQueue = 0;
    std::atomic<int> sentN { 0 }, pendingN { 0 }, rejectedN { 0 }, failures { 0 }; std::atomic<bool> offlineNow { false }, signInNow { false }, completing { false };
    std::atomic<bool> stoppedNow { false }, noRunNow { false }; juce::CriticalSection wordsLock; juce::String waitWordsNow, lastUploadSaid;   // 26 Oct: the sending screen's reason
    std::atomic<double> nextTryMs { 0.0 }; juce::String netWords;   // 26 Oct: atomic - the uploader writes it, the screen reads it   // netWords: the last plain words a call left for a screen
    juce::StringArray cachedCollection;   // 21 Oct: the run's plugin collection (contract v1 §3), from the catalogue request
    wizcontract::RunInfo runInfo() { const juce::ScopedLock l (runLock); return { cachedRunId, cachedConsentAt, cachedQueue, cachedCollection, cachedStartedAt }; }
    void cacheRun() { const auto st = appState(); const juce::ScopedLock l (runLock); cachedRunId = st.getProperty ("run_id", "").toString(); cachedConsentAt = st.getProperty ("consent_at", "").toString(); cachedStartedAt = st.getProperty ("run_started_at", "").toString();
                      cachedQueue = (int) juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString()).getProperty ("to_measure", 0);
                      cachedCollection = wizcontract::collectionFromRequest (juce::JSON::parse (requestFile().loadFileAsString())); }
    void uploadTick()
    {   // THE LIVE UPLOAD (contract v1 §4): one result per request, in order, from the outbox; offline / busy -> back-off (5 s .. 10 min); a
        // rejected result is kept apart with the server's words; a sign-in problem waits for the user. Nothing is lost by a quit
        if (session == nullptr) return;
        wizcontract::Outbox box { outboxDir() };
        pendingN = box.pendingFiles().size(); sentN = box.count (box.sent()); rejectedN = box.count (box.rejected());
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (pendingN == 0 || now < nextTryMs) return;
        const auto run = runInfo();
        if (run.runId.isEmpty())
        {   // 26 Oct: never wait silently - a result is waiting and there is no run to send it under: said once in the log, and on screen
            if (! noRunNow.exchange (true)) logLine ("upload: " + juce::String (pendingN.load()) + " result(s) waiting, but no run id is on record - nothing can be sent");
            return;
        }
        noRunNow = false;
        wizcontract::DrainStats st;
        { std::lock_guard<std::mutex> l (netMutex); st = wizcontract::drain (*session, box, run, [this] { return closing.load(); }); }
        offlineNow = st.offline; signInNow = st.signIn || session->needsSignIn; stoppedNow = st.stopped;
        { const juce::ScopedLock l (wordsLock); waitWordsNow = st.stopped ? st.stopWords : st.waitWords; }   // 26 Oct: why it waits, for the screen
        if (st.stopped) { nextTryMs = now + 1.0e12; logLine ("upload stopped (" + st.stopCode + "): " + st.stopWords + " - nothing more is sent until the wizard is opened again"); }   // v1.1: never retried this session
        else if (st.offline || st.retryAfterS > 0) { ++failures; nextTryMs = now + 1000.0 * juce::jmax (st.retryAfterS, wizcontract::backoffSeconds (failures)); }
        else if (st.signIn) nextTryMs = now + 30000.0;
        else failures = 0;
        if (st.held > 0) logLine ("upload: " + juce::String (st.held) + " kept by EchoJay but not used yet (" + st.heldWords.joinIntoString ("; ") + ")");
        if (st.sent + st.duplicates + st.rejected > 0)
            logLine ("upload: " + juce::String (st.sent) + " sent, " + juce::String (st.duplicates) + " duplicate, " + juce::String (st.rejected) + " not accepted"
                     + (st.rejectedWords.isEmpty() ? juce::String() : " (" + st.rejectedWords.joinIntoString ("; ") + ")"));
        pendingN = box.pendingFiles().size(); sentN = box.count (box.sent()); rejectedN = box.count (box.rejected());
    }
    struct Uploader : juce::Thread
    {
        WizardComponent& c; explicit Uploader (WizardComponent& o) : juce::Thread ("EchoJay uploader"), c (o) {}
        void run() override { while (! threadShouldExit()) { c.uploadTick(); wait (1000); } }
    };
    Uploader uploader { *this };
    juce::ThreadPool net { juce::ThreadPoolOptions().withThreadName ("EchoJay server").withNumberOfThreads (1) };
    template <typename F> void onNet (F&& job) { net.addJob ([this, job = std::forward<F> (job)] { if (! closing) job(); }); }
    template <typename F> void onUi (F&& f) { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<WizardComponent> (this), f = std::forward<F> (f)] () mutable { if (safe != nullptr) f(); }); }
    // SIGN-IN (contract v1 §1): the browser opens on echojay.ai; the wizard shows the code to check and polls - it never sees a password
    void startSignIn()
    {
        if (session == nullptr) return;
        primary.setEnabled (false); text = "Opening your browser..."; repaint();
        onNet ([this]
        {
            wizcontract::DeviceStart d; { std::lock_guard<std::mutex> l (netMutex); d = session->startDevice(); }
            onUi ([this, d]
            {
                deviceFlow = d;
                if (! d.ok) { netWords = wizcontract::plainWords (d.reply); show (Screen::welcome); return; }
                if (! rehearsalQuiet()) juce::URL (d.verificationUriComplete.isNotEmpty() ? d.verificationUriComplete : d.verificationUri).launchInDefaultBrowser();
                logLine (rehearsalQuiet() ? "sign-in: rehearsal - the browser NOT opened (the approval comes from the mock's hook)" : "sign-in: the browser opened on the verification page (the code is on screen)");
                show (Screen::signIn); pollSignIn();
            });
        });
    }
    void pollSignIn()
    {
        onNet ([this]
        {
            auto d = deviceFlow; const double until = juce::Time::getMillisecondCounterHiRes() + 1000.0 * juce::jmax (30, d.expiresIn);
            while (! closing && juce::Time::getMillisecondCounterHiRes() < until)
            {
                juce::Thread::sleep (1000 * d.interval);
                juce::String words; wizcontract::Poll p;
                { std::lock_guard<std::mutex> l (netMutex); p = session->pollOnce (d, words); }
                if (p == wizcontract::Poll::pending || p == wizcontract::Poll::slowDown) continue;
                onUi ([this, p, words]
                {
                    if (p == wizcontract::Poll::ok) { logLine ("signed in as " + session->displayName()); netWords = {}; show (Screen::consent); return; }
                    logLine ("sign-in did not finish: " + words); netWords = words; show (Screen::welcome);
                });
                return;
            }
            onUi ([this] { if (screen == Screen::signIn) { netWords = "That sign-in link has expired - try again."; show (Screen::welcome); } });
        });
    }
    // THE CATALOGUE CHECK (contract v1 §2): the engine lists the plugins (nothing measured), the server answers, the engine scans with the answers
    void checkCatalogue()
    {
        barLeft = "Checking with EchoJay"; repaint();
        const auto rid = runId();   // 21 Oct (Sean): every check carries the run id (made at consent, kept for the run)
        onNet ([this, rid]
        {
            const auto req = juce::JSON::parse (requestFile().loadFileAsString());
            wizcontract::CatalogueResult cr; { std::lock_guard<std::mutex> l (netMutex); cr = wizcontract::catalogueCheck (*session, req, rid); }
            onUi ([this, cr]
            {
                if (! cr.ok)
                {   netWords = wizcontract::plainWords (cr.reply); logLine ("catalogue check failed: " + netWords);
                    title = "Couldn't reach EchoJay"; text = netWords; showBar = false; primary.setButtonText (session->needsSignIn ? "Sign in again" : "Try again"); primary.setVisible (true); primary.setEnabled (true);
                    retryCatalogue = true; repaint(); return; }
                answersFile().replaceWithText (juce::JSON::toString (cr.answers));
                logLine ("catalogue check: " + wizapp::knownWords (cr.have + cr.held, cr.need) + " (have " + juce::String (cr.have) + ", held " + juce::String (cr.held) + ")");
                barLeft = wizapp::knownWords (cr.have + cr.held, cr.need); startEngine (false);
            });
        });
    }
    bool retryCatalogue = false;
    // THE RUN'S END (contract v1 §5): every result sent first; then the server decides and returns the promo (never made here)
    void reportRunEnd()
    {
        if (completing) return; completing = true;
        onNet ([this]
        {
            const auto m = juce::JSON::parse (engineDir().getChildFile ("contribution.manifest.json").loadFileAsString());
            wizcontract::Outbox box { outboxDir() }; juce::StringArray ids;
            for (const auto& f : box.sent().findChildFiles (juce::File::findFiles, false, "*.json.gz")) ids.add (wizcontract::Outbox::resultIdOf (f));
            const auto kind = m.getProperty ("run", {}).getProperty ("kind", "finished").toString();
            wizcontract::Completion c; { std::lock_guard<std::mutex> l (netMutex); c = wizcontract::complete (*session, runInfo().runId, kind, m.getProperty ("queue", {}), m.getProperty ("outcomes", {}), ids); }
            if (! c.missing.isEmpty())
            {   // the server lacks some: back into the outbox, sent again, then asked again
                for (const auto& f : box.sent().findChildFiles (juce::File::findFiles, false, "*.json.gz")) if (c.missing.contains (wizcontract::Outbox::resultIdOf (f))) f.moveFileTo (box.pending().getChildFile (f.getFileName()));
                logLine ("run end: the server lacked " + juce::String (c.missing.size()) + " result(s) - sending them again"); nextTryMs = 0.0;
            }
            onUi ([this, c]
            {
                completing = false;
                if (! c.missing.isEmpty()) return;   // tickUpload asks again once they are sent
                if (! c.done) { netWords = wizcontract::plainWords (c.reply); logLine ("run end not reported: " + netWords); title = c.reply.network ? "You're offline" : "Couldn't reach EchoJay";
                                text = netWords + " Your results are kept on this Mac."; showBar = false; primary.setButtonText ("Try again"); primary.setVisible (true); secondary.setButtonText ("Done"); secondary.setVisible (true); repaint(); return; }
                setState ("uploaded", true);
                logLine ("run end reported: promo " + juce::String (c.eligible ? (c.issuedNow ? "issued" : "already issued") : "not eligible") + (c.message.isNotEmpty() ? " - " + c.message : juce::String()));
                if (c.eligible && c.code.isNotEmpty())
                {   auto* r = new juce::DynamicObject(); r->setProperty ("code", c.code); r->setProperty ("months", c.months); r->setProperty ("message", c.message); r->setProperty ("expires_at", c.expiresAt);
                    cfg.work.getChildFile ("reward.json").replaceWithText (juce::JSON::toString (juce::var (r))); show (Screen::reward); }
                else { auto* r = new juce::DynamicObject(); r->setProperty ("reward_message", c.message); cfg.work.getChildFile ("receipt.json").replaceWithText (juce::JSON::toString (juce::var (r))); show (Screen::noCode); }
            });
        });
    }
    // THE RUN ID (13 Oct): made once, when measuring first starts; a pause and a resume days later are the same run
    juce::String runId()
    {
        auto id = appState().getProperty ("run_id", "").toString();
        if (id.isEmpty()) { id = "run_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()).paddedLeft ('0', 16); auto st = appState();   /* 21 Oct: always 16 hex (contract v1 §3) */ st.getDynamicObject()->setProperty ("run_id", id);
                            st.getDynamicObject()->setProperty ("run_started_at", juce::Time::getCurrentTime().toISO8601 (true));   // v1.1: the run's started_at
                            cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (st)); logLine ("run id: " + id); }
        return id;
    }
    static juce::String hostName() { char h[256] = {}; gethostname (h, sizeof h - 1); return juce::String (h).upToFirstOccurrenceOf (".", false, false); }
    juce::var appState() const { const auto v = juce::JSON::parse (cfg.work.getChildFile ("app_state.json").loadFileAsString()); return v.isObject() ? v : juce::var (new juce::DynamicObject()); }
    void setState (const char* k, bool v) { auto s = appState(); s.getDynamicObject()->setProperty (k, v); cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (s)); }
    wizapp::ProgressLogClock progressClock;   // 20 Oct: the progress line's clock
    void logLine (const juce::String& s) { cfg.work.getChildFile ("app.log").appendText (juce::Time::getCurrentTime().toISO8601 (true) + " " + s + "\n"); }

    juce::StringArray engineArgs (bool measure)
    {
        juce::StringArray a { "--wizard", "--db", cfg.db.getFullPathName(), "--out", engineDir().getFullPathName(), "--ejmap-ledger", cfg.store.getFullPathName(),
                              "--jobs", juce::String (cfg.jobs), "--skip", cfg.work.getChildFile ("skip.json").getFullPathName() };
        for (const auto& o : cfg.only) { a.add ("--only"); a.add (o); }
        if (kNetworked && answersFile().existsAsFile()) { a.add ("--catalogue-answers"); a.add (answersFile().getFullPathName()); }   // 21 Oct: the server's answers
        if (kNetworked && measure) { a.add ("--results-outbox"); a.add (outboxDir().getFullPathName()); }                              // 21 Oct: one result per plugin, sent live
        if (measure) { a.add ("--measure"); a.add ("--bundle"); a.add ("--run-id"); a.add (runId()); a.add ("--checkpoint-minutes"); a.add (juce::String (cfg.checkpointMinutes)); }
        return a;
    }
    void startCatalogueRequest() { startEngine (false, true); }   // 21 Oct: the engine lists the plugins for the server's check, nothing measured
    void startEngine (bool measure, bool catalogueRequest = false)
    {
        if (const auto dv = wizapp::diskVerdict (wizapp::freeBytes (cfg.work), true); ! dv.ok)   // 13 Oct: never start below the floor
        { const auto c = wizapp::diskRefusedCopy (dv.words); title = c.title; text = c.text; showBar = false; primary.setButtonText (c.primary); primary.setVisible (true); diskRefused = true;
          logLine ("refused to start: " + dv.words); resized(); repaint(); return; }
        if (const auto hit = openDaw (dawIgnoredPid); hit.any())   // 15 Oct: a DAW is open - wait, do not start (it carries on by itself when it closes)
        { dawPaused = hit.name; dawHit = hit; resumeTo = measure ? Screen::measuring : Screen::scan; logLine ("not started: " + wizapp::dawLogLine (hit) + " is open");
          juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<WizardComponent> (this)] { if (safe != nullptr) safe->show (Screen::paused); }); return; }
        engineDir().createDirectory(); engineDir().getChildFile ("progress.json").deleteFile();
        engineDir().getChildFile ("checkpoint.json").deleteFile();   // 13 Oct: the last launch's checkpoint was handled then (or is in the outbox)   // the last run's "Done 100%" must not flash on the new screen
        auto args = engineArgs (measure);
        if (catalogueRequest) { args.removeString ("--catalogue-answers"); args.removeString (answersFile().getFullPathName()); args.add ("--catalogue-request"); args.add (requestFile().getFullPathName()); }
        if (! engine.start (cfg.engine, args, cfg.work.getChildFile ("engine_output.txt"), catalogueRequest ? "catalogue" : measure ? "measure" : "scan"))
        { title = "Something went wrong"; text = "The measuring engine could not start (" + cfg.engine.getFullPathName() + ")."; repaint(); return; }
        awake.hold (true); logLine ("engine started: " + engine.mode + " (pid " + juce::String (engine.pid) + "), keep-awake held");
    }

    void show (Screen s)
    {
        screen = s; showBar = false; bigCode = {}; pauseNote = {}; primary.setVisible (false); secondary.setVisible (false); anyway.setVisible (false); email.setVisible (false); password.setVisible (false);
        agree.setVisible (false); primary.setEnabled (true); secondary.setEnabled (true); resetTestButton.setVisible (kTest && wizapp::resetTestShownOn (s));
        autoTicks = 0; lastSnapQuarter = -1.0;
        auto copy = [&] (const wizapp::Copy& c) { title = c.title; text = c.text; primary.setButtonText (c.primary); primary.setVisible (c.primary.isNotEmpty());
                                                  secondary.setButtonText (c.secondary); secondary.setVisible (c.secondary.isNotEmpty()); };
        switch (s)
        {
            case Screen::welcome:
                title = "Help EchoJay learn your plugins";
                if (kNetworked)
                {   // 21 Oct: sign in on echojay.ai in the browser - the wizard never sees the password
                    if (session == nullptr) { title = "Not connected yet"; text = "This build of the EchoJay Wizard isn't connected to EchoJay's server yet."; primary.setButtonText ("Quit"); primary.setVisible (true); break; }
                    text = netWords.isNotEmpty() ? netWords + " " : juce::String();
                    text << "Sign in with your EchoJay account - it opens in your browser, and the wizard never sees your password.";
                    primary.setButtonText ("Sign in with EchoJay"); primary.setVisible (true); break;
                }
                if (kMode == wizapp::Mode::beta) { text = "Thank you for testing the EchoJay Wizard. It measures the plugins on this Mac that EchoJay doesn't know yet."; primary.setButtonText ("Continue"); primary.setVisible (true); break; }
                text = "Sign in with your EchoJay account. What we measure is linked to your account - and so is your reward: a free month.";
                email.setVisible (true); password.setVisible (true); primary.setButtonText ("Sign in"); primary.setVisible (true); break;
            case Screen::consent:
                title = "Before we start"; text = wizapp::consentText (kMode);
                agree.setToggleState (false, juce::dontSendNotification); agree.setVisible (true); primary.setButtonText ("Continue"); primary.setVisible (true); primary.setEnabled (false); break;
            case Screen::scan:
                title = "Looking for plugins"; text = "We're looking at each plugin on this Mac and checking it against what EchoJay already knows.";
                resetShownProgress();   // 18 Oct (BAR % WRONG): a scan starts a run - nothing shown carries over from the last one
                showBar = true; barValue = 0.0; barLeft = "Starting"; barRight = {};
                if (kNetworked) { retryCatalogue = false; requestFile().deleteFile(); startCatalogueRequest(); break; }   // 21 Oct: the list first, then the server's check
                startEngine (false); break;
            case Screen::alreadyDone: copy (wizapp::alreadyDoneCopy()); break;
            case Screen::signIn:   // 21 Oct: the code to check in the browser
                title = "Approve in your browser"; bigCode = deviceFlow.userCode;
                text = "We've opened echojay.ai in your browser. Check this code matches, then approve EchoJay Wizard.";
                secondary.setButtonText ("Open the page again"); secondary.setVisible (true); break;
            case Screen::scanDone:
            {
                const auto ws = juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString());
                const double eta = (double) ws.getProperty ("to_measure_eta_s", 0.0);   // the per-category estimate (measured seconds once any exist, --jobs aware)
                const auto el = wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString()));
                copy (wizapp::scanDoneCopy (kMode, el));   // 14 Oct: no time on screen - the estimate is only logged
                logLine ("scan done: " + juce::String (el.count) + " to measure, estimate " + juce::String (eta, 0) + " s at --jobs " + juce::String (cfg.jobs));
                break;
            }
            case Screen::crashRecovered:
            {
                juce::StringArray names; for (const auto& c : crashed) names.add (c.product);
                title = "Welcome back";
                text = "The wizard closed while checking " + names.joinIntoString (" and ") + " - we've skipped " + (names.size() == 1 ? "it" : "them") + " and carried on.";
                primary.setButtonText ("Carry on"); primary.setVisible (true); break;
            }
            case Screen::measuring:
            {
                const auto ch = wizapp::measuringChoices (kMode);
                title = "Measuring";
                {   // 18 Oct: a resumed run says what is LEFT (the last run's left.json), a first run what it measures
                    const int left = wizapp::leftCount ((bool) appState().getProperty ("measure_started", false), juce::JSON::parse (engineDir().getChildFile ("left.json").loadFileAsString()));
                    text = left > 0 ? wizapp::measuringResumedText (left) : wizapp::measuringText (wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString())).count);
                }
                showBar = true; barValue = 0.0; barLeft = "Getting ready"; barRight = {}; setState ("measure_started", true); finishing = false; pausing = false;
                primary.setButtonText (ch.pause); primary.setVisible (true); secondary.setButtonText (ch.send); secondary.setVisible (true); secondary.setEnabled (false);
                choiceLine = ch.line; soFar = wizapp::measuredSoFarWords (0);
                if (kNetworked) { runId(); cacheRun(); if (! uploader.isThreadRunning()) uploader.startThread();
                                  const auto el = wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString()));
                                  if (el.count > 0) text = wizapp::knownWords (el.known, el.count) + ". " + text; }
                startEngine (true); break;
            }
            case Screen::paused:
                {   // 15 Oct: the bar's % (this session's, else the stopped engine's own), never a count
                    const int pct = shownPct > 0 ? shownPct : wizapp::measuringPercent (juce::JSON::parse (engineDir().getChildFile ("progress.json").loadFileAsString()), 0);
                    title = resumeTo == Screen::scan ? juce::String ("Paused") : wizapp::pausedTitle (pct);
                    text = plugInFirst && wizapp::carryOnBlocked (power) ? juce::String (wizapp::kPlugInFirst) + ". " + wizapp::batteryPausedText (power.percent) : batteryPaused ? wizapp::batteryPausedText (power.percent) : dawPaused.isNotEmpty() ? wizapp::dawPausedText (dawPaused, dawHit.windows) : wizapp::pausedText (kMode, kNetworked ? (bool) offlineNow : lastSendKept);   // 15 Oct: the DAW's own line (17 Oct: no window -> "still running in the background")
                }
                primary.setButtonText ("Carry on"); primary.setVisible (true); secondary.setButtonText ("Quit"); secondary.setVisible (true);
                anyway.setVisible (dawPaused.isNotEmpty()); break;   // 17 Oct: Carry on anyway, beside the DAW
            case Screen::upload:
                title = "Sending your results"; text = wizapp::kUploadText;
                if (kNetworked) { setState ("saved", true); nextTryMs = 0.0; }   // 21 Oct: the run is over; its last results go, then its end is reported
                showBar = true; barValue = 0.0; barLeft = "Sending"; barRight = {}; uploadT0 = juce::Time::getMillisecondCounterHiRes(); break;
            case Screen::reward:
            {
                const auto rw = juce::JSON::parse (cfg.work.getChildFile ("reward.json").loadFileAsString());
                title = "Thank you - here's your free month"; bigCode = rw.getProperty ("code", "").toString();   // 21 Oct (beta): the SERVER's code, never one made here
                text = kNetworked ? juce::String ("Use this code in EchoJay.") : "Use this code in EchoJay. We've emailed it to you too (" + rw.getProperty ("emailed_to", "").toString() + ").";
                if (const auto ew = wizapp::expiryWords (rw.getProperty ("expires_at", "").toString()); ew.isNotEmpty()) text << " " << ew;   // v1.1: the server's expiry
                {   // nothing dropped (12 Oct): say what went for review instead of being measured
                    const auto m = juce::JSON::parse (engineDir().getChildFile ("contribution/manifest.json").loadFileAsString());
                    const int nc = m.getProperty ("needs_category", {}).size();
                    if (nc > 0) text << " " << nc << (nc == 1 ? " plugin we couldn't identify was" : " plugins we couldn't identify were") << " sent for review.";
                }
                secondary.setButtonText ("Copy code"); secondary.setVisible (true); primary.setButtonText ("Done"); primary.setVisible (true); break;
            }
            case Screen::noCode:
            {
                const auto rc = juce::JSON::parse (cfg.work.getChildFile ("receipt.json").loadFileAsString());
                title = "Thank you - your results arrived"; text = rc.getProperty ("reward_message", "").toString().replace ("Thank you - your results arrived. ", "");   // the title says it
                primary.setButtonText ("Done"); primary.setVisible (true); break;
            }
            case Screen::betaSaved:
            {
                savedFile = juce::File (appState().getProperty ("saved_file", "").toString());
                title = "Thank you - your results are ready";
                text = "Saved as " + savedFile.getFileName() + " on your Desktop. Email this file to " + juce::String (EJ_WIZARD_BETA_EMAIL) + " - even if you stop early, email the file on your Desktop.";
                secondary.setButtonText ("Show in Finder"); secondary.setVisible (true); primary.setButtonText ("Done"); primary.setVisible (true); break;
            }
        }
        logLine ("screen: " + wizapp::screenName (s));
        resized(); repaint();
        juce::Timer::callAfterDelay (400, [safe = juce::Component::SafePointer<WizardComponent> (this), s] { if (safe != nullptr && safe->screen == s) safe->snap ("screen"); });
    }

    void onPrimary()
    {
        if (diskRefused) { logLine ("quit (not enough disk space)"); juce::JUCEApplication::getInstance()->systemRequestedQuit(); return; }
        switch (screen)
        {
            case Screen::welcome:
            {
                if (kNetworked) { if (session == nullptr) { juce::JUCEApplication::getInstance()->systemRequestedQuit(); break; } startSignIn(); break; }   // 21 Oct
                if (kMode == wizapp::Mode::beta) { show (Screen::consent); break; }
                account = stub::signIn (cfg.work, email.getText().trim(), password.getText());
                if (account.isVoid()) { text = "Please enter your EchoJay email and password."; repaint(); return; }
                logLine ("signed in (STUB): " + account.getProperty ("account_id", "").toString()); show (Screen::consent); break;
            }
            case Screen::consent:
                if (! agree.getToggleState()) return;   // the tick is required
                setState ("consented", true); logLine ("consent: I agree (" + juce::Time::getCurrentTime().toISO8601 (true) + ")");
                if (! cfg.only.isEmpty())   // 26 Oct: --only belongs to the run - a reopen without it measures the same plugins, never the whole Mac
                {   auto so = appState(); juce::Array<juce::var> ol; for (const auto& n : cfg.only) ol.add (n); so.getDynamicObject()->setProperty ("only", ol);
                    cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (so)); }
                if (kNetworked)
                {   // 21 Oct: the consent's version and time travel with the run (contract v1 §3); then the plugin list and the server's check
                    auto st = appState(); st.getDynamicObject()->setProperty ("consent_at", juce::Time::getCurrentTime().toISO8601 (true)); st.getDynamicObject()->setProperty ("consent_version", wizcontract::kConsentVersion);
                    cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (st)); runId(); cacheRun(); if (! uploader.isThreadRunning()) uploader.startThread();   // 21 Oct (Sean): the run id exists before the first check
                    show (Screen::scan); break;
                }
                {   // /lookup before anything loads: an account or a Mac that has already taken part runs nothing (STUB)
                    if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
                    const auto early = stub::lookup (cfg.work, account, device, juce::var());
                    logLine ("lookup before the scan (STUB): already_contributed " + juce::String ((int) (bool) early.getProperty ("already_contributed", false)));
                    show (wizapp::afterConsent (wizapp::eligibilityOf (early)));
                }
                break;
            case Screen::scanDone:
            {
                const auto el = wizapp::eligibilityOf (juce::JSON::parse (cfg.work.getChildFile ("eligibility.json").loadFileAsString()));
                if (el.count == 0 || (kMode == wizapp::Mode::normal && ! el.eligible)) { logLine ("not eligible / nothing to measure: done, nothing runs"); juce::JUCEApplication::getInstance()->systemRequestedQuit(); break; }
                show (Screen::measuring); break;
            }
            case Screen::crashRecovered:
            {
                crashed.clear(); const auto st = appState();
                show ((bool) st.getProperty ("measure_started", false) ? Screen::measuring : Screen::scan); break;
            }
            case Screen::alreadyDone: case Screen::reward: case Screen::noCode: case Screen::betaSaved: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            case Screen::measuring: pauseNow ("the Pause button", false); break;
            case Screen::scan: if (retryCatalogue) { retryCatalogue = false; primary.setVisible (false); if (session != nullptr && session->needsSignIn) { show (Screen::welcome); break; } title = "Checking your plugins"; showBar = true; checkCatalogue(); } break;   // 21 Oct
            case Screen::upload: if (kNetworked && ! completing) { primary.setVisible (false); secondary.setVisible (false); title = "Sending your results"; showBar = true; nextTryMs = 0.0; } break;   // 21 Oct: Try again
            case Screen::paused:
                if (wizapp::carryOnBlocked (power))   // 24 Oct (Kathy): never carry on into a sleep - stay paused, carry on once plugged in
                { logLine ("carry on refused: " + wizapp::powerWords (power) + " - " + wizapp::kPlugInFirst); plugInFirst = true; batteryPaused = true; show (Screen::paused); break; }
                logLine ("carry on"); dawPaused.clear(); dawQuietSince = -1.0; batteryPaused = false; plugInFirst = false; show (resumeTo); break;   // 15 Oct: a paused scan carries on scanning (a DAW still open pauses it again)
            default: break;
        }
    }
    void onSecondary()
    {
        if (screen == Screen::signIn) { juce::URL (deviceFlow.verificationUriComplete.isNotEmpty() ? deviceFlow.verificationUriComplete : deviceFlow.verificationUri).launchInDefaultBrowser(); logLine ("sign-in: the page opened again"); }   // 21 Oct
        if (kNetworked && screen == Screen::upload && secondary.isVisible()) { logLine ("done before the run's end was reported: it is reported on the next launch"); juce::JUCEApplication::getInstance()->quit(); }
        if (screen == Screen::reward) { juce::SystemClipboard::copyTextToClipboard (bigCode); secondary.setButtonText ("Copied"); logLine ("code copied"); }
        if (screen == Screen::betaSaved && savedFile.existsAsFile()) { savedFile.revealToUser(); logLine ("shown in Finder: " + savedFile.getFileName()); }
        if (screen == Screen::scanDone) { logLine ("contribute anyway (no reward)"); show (Screen::measuring); }
        if (screen == Screen::measuring && secondary.isEnabled()) finishNow ("the Send what's done button");
        if (screen == Screen::paused) { logLine ("quit while paused"); juce::JUCEApplication::getInstance()->quit(); }
    }
    void finishNow (const juce::String& from)
    {   // FINISH NOW (Kathy, 13 Oct): the request file, then the pause's clean stop - the engine packs what landed and ends 0
        if (finishing || ! engine.running()) return;
        if (! secondary.isEnabled() && from != "rehearsal")   // the close question before measuring began: nothing to pack - a pause
        { logLine ("send asked before measuring began: paused instead"); pauseNow ("send before measuring", true); return; }
        finishing = true; engineDir().getChildFile (wizapp::kFinishRequestFile).replaceWithText ("finish\n"); engine.pause();
        secondary.setEnabled (false); primary.setEnabled (false); secondary.setButtonText ("Sending..."); barLeft = kMode == wizapp::Mode::beta ? "Saving what's measured" : "Sending what's measured"; barRight = {};
        logLine ("finish now (" + from + "): " + soFar + "; engine stopped (SIGINT) to pack"); repaint();
    }
    void resetTestNow()
    {   // RESET TEST (Kathy, 14 Oct): the stub server's account / device / reward records and the whole work folder, keeping config.json;
        // the snapshot seeded again - so once-only and the reward can be tried again
        if (! kTest || engine.running()) return;
        const int n = wizapp::resetTest (cfg.work); cfg.work.createDirectory(); seedTestBuild (cfg); cfg.only = wizapp::testOnlyPlugins (cfg.work);
        account = juce::var(); receipt = juce::var(); crashed.clear(); lastSendKept = false; lastCheckpointSeq = 0; email.clear(); password.clear();
        lastPowerLog.clear();   // 24 Oct: the reset clears the log, so the power is said again at the next check
        logLine ("reset test: " + juce::String (n) + " item(s) cleared, config.json kept, the snapshot seeded again");
        resetShownProgress();   // 18 Oct (BAR % WRONG): the 98.9% was the run before the reset, kept by "never go back"
        show (Screen::welcome);
    }
    void pauseNow (const juce::String& from, bool quitAfter)
    {   // PAUSE (13 Oct): the engine's clean stop - in-flight plugins discarded, a checkpoint packed - then the Paused screen (or quit)
        if (pausing || finishing || ! engine.running()) { if (quitAfter && ! engine.running()) juce::JUCEApplication::getInstance()->quit(); return; }
        if (dawPaused.isEmpty()) resumeTo = screen == Screen::scan ? Screen::scan : Screen::measuring;
        pausing = true; quitAfterPause = quitAfter; engine.pause(); primary.setEnabled (false); secondary.setEnabled (false);
        barLeft = "Pausing - saving what's done"; barRight = {}; pauseNote = wizapp::stoppedNote();
        logLine ("pause (" + from + "): " + soFar + "; engine stopped (SIGINT), a checkpoint follows"); repaint();
    }
    // A CHECKPOINT the engine packed (engine/checkpoint.json): beta - the Desktop file rewritten; release - a quiet partial upload (kept in the
    // outbox when offline). A final pack (sent_early / finished) goes through the upload screen instead
    void handleCheckpoint()
    {
        const auto cp = juce::JSON::parse (engineDir().getChildFile ("checkpoint.json").loadFileAsString());
        // 13 Oct (rehearsal B): a checkpoint is known by its time stamp, remembered across launches - an engine's numbering restarts at 1,
        // and a stale checkpoint.json from the last launch was sent again while the new engine's first one was skipped
        const int seq = (int) cp.getProperty ("seq", 0); const auto at = cp.getProperty ("at", "").toString();
        if (seq <= 0 || at.isEmpty() || ! wizapp::isNewCheckpoint (at, appState().getProperty ("last_checkpoint_at", "").toString())) return;
        { auto st = appState(); st.getDynamicObject()->setProperty ("last_checkpoint_at", at); cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (st)); }
        lastCheckpointSeq = seq; const auto kind = cp.getProperty ("kind", "").toString();
        const auto zip = engineDir().getChildFile ("contribution.zip"), man = engineDir().getChildFile ("contribution.manifest.json");
        if (kNetworked) { logLine ("checkpoint " + juce::String (seq) + " (" + kind + "): its results are in the outbox"); nextTryMs = 0.0; return; }   // 21 Oct: sent live, one per plugin
        if (kind != "checkpoint") return;
        sendOrKeep (zip, man, kind);
    }
    void sendOrKeep (const juce::File& zip, const juce::File& man, const juce::String& kind)
    {
        if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
        drainOutbox();   // what was kept goes first: the server merges in order
        const auto r = stub::upload (cfg.work, zip, man, account, device);
        lastSendKept = ! r.isObject();
        if (r.isObject()) logLine ("partial upload (" + kind + ", STUB): merged into " + r.getProperty ("run_id", "").toString() + ", " + juce::String ((int) r.getProperty ("run_final", 0)) + " of " + juce::String ((int) r.getProperty ("run_queued", 0)) + " with a result");
        else { const auto kept = wizapp::outboxPut (cfg.work, zip, man, kind); logLine ("offline: " + kind + " kept for later (" + kept.getFileName() + ")"); }
    }
    int drainOutbox()
    {
        if (kMode == wizapp::Mode::beta) return 0;
        if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
        const int n = wizapp::outboxDrain (cfg.work, [&] (const juce::File& z, const juce::File& m)
        {
            const auto r = stub::upload (cfg.work, z, m, account, device); if (! r.isObject()) return false;
            logLine ("outbox: sent " + z.getFileName() + " (" + r.getProperty ("kind", "").toString() + ")");
            if ((bool) r.getProperty ("reward_eligible", false))
            { cfg.work.getChildFile ("receipt.json").replaceWithText (juce::JSON::toString (r)); setState ("uploaded", true);
              if (const auto pr = stub::promo (cfg.work, account, r); pr.isObject()) cfg.work.getChildFile ("reward.json").replaceWithText (juce::JSON::toString (pr)); }
            return true;
        });
        return n;
    }
    void snapWindow (juce::Component& c, const juce::String& why)
    {
        if (cfg.snapshots == juce::File()) return;
        const auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
        const auto f = cfg.snapshots.getChildFile (launchTag + "_" + juce::String (++snapN).paddedLeft ('0', 2) + "_" + why + ".png");
        juce::FileOutputStream os (f); if (os.openedOk()) { os.setPosition (0); os.truncate(); juce::PNGImageFormat().writeImageToStream (img, os); }
    }
    void saveBetaResults()
    {   // THE ONE FILE (13 Oct): the same Desktop file rewritten each time (pause / close / hourly / finish)
        const char* rd = std::getenv ("EJ_WIZARD_RESULTS_DIR");   // rehearsals only: never the tester's Desktop
        const auto desk = rd != nullptr && *rd ? juce::File (rd) : juce::File::getSpecialLocation (juce::File::userDesktopDirectory); desk.createDirectory();
        const auto f = wizapp::betaResultsTarget (appState().getProperty ("saved_file", "").toString(), desk, juce::Time::getCurrentTime());
        if (wizapp::rewriteResultsFile (engineDir().getChildFile ("contribution.zip"), f))
        { auto st = appState(); st.getDynamicObject()->setProperty ("saved_file", f.getFullPathName()); cfg.work.getChildFile ("app_state.json").replaceWithText (juce::JSON::toString (st)); logLine ("saved: " + f.getFullPathName()); }
        else logLine ("could not save the results file");
    }
    void snap (const juce::String& why)
    {
        if (cfg.snapshots == juce::File()) return;
        cfg.snapshots.createDirectory();
        const auto img = createComponentSnapshot (getLocalBounds(), true, 2.0f);
        const auto f = cfg.snapshots.getChildFile (launchTag + "_" + juce::String (++snapN).paddedLeft ('0', 2) + "_" + wizapp::screenName (screen) + "_" + why + ".png");   // launch time first: launches never share a name
        juce::FileOutputStream os (f); if (os.openedOk()) { os.setPosition (0); os.truncate(); juce::PNGImageFormat().writeImageToStream (img, os); }
    }
    void snapAtQuarters()
    {
        const double q = std::floor (barValue * 4.0) / 4.0;
        if (q > lastSnapQuarter && q > 0.0) { lastSnapQuarter = q; snap (juce::String ((int) (q * 100.0)) + "pct"); }
    }

    void timerCallback() override
    {
        if (appInterrupted() != 0 && ! quitting) { appInterrupted() = 0; logLine ("Ctrl-C: a pause (no question asked)"); if (requestQuit (false)) juce::JUCEApplication::getInstance()->quit(); }
        if (const double now = juce::Time::getMillisecondCounterHiRes(); now - lastDawCheck >= 2000.0) { lastDawCheck = now; dawWatch (openDaw (dawIgnoredPid)); }
        if (const double now = juce::Time::getMillisecondCounterHiRes(); now - lastPowerCheck >= 10000.0) { lastPowerCheck = now; powerWatch (readPower()); }
        if (screen == Screen::scan || screen == Screen::measuring) pollEngine();
        if (screen == Screen::upload) tickUpload();
        if (cfg.autoMode && ! quitting) autoAdvance();
        if (cfg.autoResetTest && resetTestButton.isVisible() && ++resetTicks == 6)
        {   cfg.autoResetTest = false; snap ("before_reset"); resetTestNow(); snap ("after_reset");
            juce::Timer::callAfterDelay (1500, [] { juce::JUCEApplication::getInstance()->quit(); }); }
    }
    // THE BATTERY (Kathy, 23 Oct; wizapp::batteryStep): the notice on every screen while on battery; below 20% on battery while scanning or
    // measuring, the button's clean pause with the reason; plugged in again, a pause the battery caused carries on by itself. Logged
    void powerWatch (const wizapp::Power& p)
    {
        power = p;
        const bool running = (screen == Screen::scan || screen == Screen::measuring) && engine.running() && ! pausing && ! finishing;
        const auto step = wizapp::batteryStep (p, running, batteryPaused && screen == Screen::paused);
        const auto notice = step.notice ? wizapp::plugInNotice() : juce::String();
        if (notice != powerNotice) { powerNotice = notice; repaint(); }
        const auto words = wizapp::powerWords (p);
        if (words.isNotEmpty() && (words.upToFirstOccurrenceOf (" ", false, false) != lastPowerLog.upToFirstOccurrenceOf (" ", false, false)))
        { lastPowerLog = words; logLine ("power: " + words + (step.notice ? " - the notice asks to plug in" : "")); }
        if (step.pause && dawPaused.isEmpty())
        { batteryPaused = true; resumeTo = screen; logLine ("paused by itself: " + step.why); pauseNow (step.why, false); return; }
        if (step.resume)
        { logLine ("carrying on by itself: plugged in (" + words + ")"); batteryPaused = false; plugInFirst = false; show (resumeTo); }
    }
    // SHARING THE MAC (Kathy, 15 Oct): a DAW opens while the wizard scans or measures -> the same clean pause as the button, with the DAW's
    // name; only a pause the DAW caused carries on by itself, once no DAW has been open for kDawQuietS. The user's own pause never does
    void dawWatch (const ejmap::wizapp::DawHit& hit)
    {
        if (dawIgnoredPid != 0 && kill (dawIgnoredPid, 0) != 0) { logLine ("the DAW carried on beside (pid " + juce::String (dawIgnoredPid) + ") has quit"); dawIgnoredPid = 0; }   // signal 0: only asks whether it lives
        const auto daw = hit.name;
        if (dawPaused.isEmpty() && (screen == Screen::scan || screen == Screen::measuring) && engine.running() && ! pausing && ! finishing && daw.isNotEmpty())
        { dawPaused = daw; dawHit = hit; resumeTo = screen; logLine ("paused by itself: " + wizapp::dawLogLine (hit) + " is open"); pauseNow (daw + " is open", false); return; }
        if (screen != Screen::paused || dawPaused.isEmpty()) return;
        if (daw.isNotEmpty()) { dawQuietSince = -1.0; if (daw != dawPaused || hit.windows != dawHit.windows) { dawPaused = daw; dawHit = hit; text = wizapp::dawPausedText (daw, hit.windows); repaint(); } return; }
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (dawQuietSince < 0.0) { dawQuietSince = now; return; }
        if (now - dawQuietSince >= wizapp::kDawQuietS * 1000.0)
        { logLine ("carrying on by itself: " + dawPaused + " is closed"); dawPaused.clear(); dawQuietSince = -1.0; anyway.setVisible (false); show (resumeTo); }
    }
    // CARRY ON ANYWAY (Kathy, 17 Oct): the user chooses to measure beside the DAW - that process is not paused for again until it quits
    // (a new DAW process pauses as before); said in the log with what was running
    void onAnyway()
    {
        if (screen != Screen::paused || dawPaused.isEmpty()) return;
        if (wizapp::carryOnBlocked (power))   // 24 Oct: beside a DAW too - never carry on into a sleep
        { logLine ("carry on refused: " + wizapp::powerWords (power) + " - " + wizapp::kPlugInFirst); plugInFirst = true; batteryPaused = true; show (Screen::paused); return; }
        logLine ("carry on anyway beside " + wizapp::dawLogLine (dawHit)); dawIgnoredPid = dawHit.pid;
        dawPaused.clear(); dawQuietSince = -1.0; anyway.setVisible (false); show (resumeTo);
    }
    void pollEngine()
    {
        const auto p = juce::JSON::parse (engineDir().getChildFile ("progress.json").loadFileAsString());
        // 22 Oct (the bar flicker): an empty or partial read changes nothing on screen - the last good one stays; the engine's exit is still seen
        if (wizapp::progressUsable (p)) showProgress (p);
        pollEngineExit();
    }
    void showProgress (const juce::var& p)
    {
        const auto stage = p.getProperty ("stage", "").toString(); const double pct = (double) p.getProperty ("percent", 0.0);
        juce::String cur = p.getProperty ("current_latest", "").toString();   // 15 Oct: the most recently started plugin (was the oldest still listed)
        if (cur.isEmpty()) if (const auto* a = p.getProperty ("current", {}).getArray()) if (! a->isEmpty()) cur = (*a)[a->size() - 1].toString();
        if (screen == Screen::scan)
        {   // the Logic-style bar: the scan is the engine's Scanning + Checking (0..15 of its bar)
            // 18 Oct (NEVER LOOKS FROZEN): step 1 / 2, a bar per step that never goes back, its % to a tenth, the now line, the counter
            const int sidx = (int) p.getProperty ("stage_index", 0);
            barValue = stepBar.next (sidx <= 0 ? 1 : 2, wizapp::scanBarValue (p)); barLeft = wizapp::scanLeftWords (p, cur); barRight = wizapp::barPercentWords (barValue);
            stepText = wizapp::stepLine (false, sidx); nowText = wizapp::nowWords (p); doneLine = wizapp::counterWords (p, false);
            title = sidx <= 0 ? "Looking for plugins" : "Checking your plugins";   // 18 Oct: the title follows the step
            heartbeatMs = (juce::int64) p.getProperty ("heartbeat_ms", (juce::int64) 0);   // 18 Oct: the live wave and the 2 min / 15 min lines here too
            snapOnStep (stepText);
            if (cfg.snapScanAtPct >= 0.0 && barValue * 100.0 >= cfg.snapScanAtPct) { cfg.snapScanAtPct = -1.0; repaint(); juce::Timer::callAfterDelay (300, [safe = juce::Component::SafePointer<WizardComponent> (this)] { if (safe != nullptr) safe->snap ("scan_at_pct"); }); }
        }
        else
        {   // measuring: the engine's Measuring band (15..97) is the bar; its rescan is "getting ready"
            if (! finishing && ! pausing) { soFar = wizapp::measuredSoFarWords (wizapp::measuredSoFar (p)); secondary.setEnabled (wizapp::finishAvailable (p)); }
            if (kNetworked && ! pausing) choiceLine = wizapp::uploadLine (sentN, pendingN, rejectedN, offlineNow, signInNow);   // 21 Oct: what the server has, live
            handleCheckpoint();   // 13 Oct: the hourly checkpoint, as soon as the engine has packed it
            jobsNow = wizapp::jobLines (p); heartbeatMs = (juce::int64) p.getProperty ("heartbeat_ms", (juce::int64) 0);   // 15 Oct (FEELS FROZEN)
            stepText = wizapp::stepLine (true, (int) p.getProperty ("stage_index", 0)); nowText = wizapp::nowWords (p);   // 18 Oct (NEVER LOOKS FROZEN)
            doneLine = wizapp::measuringCounterWords (stage, p);   // 18 Oct (BAR % WRONG): "19 of 389 done - 83 skipped"; 23 Oct: "Packing results..." while packing
            if (wizapp::packingStage (stage)) jobsNow.clear();       // 23 Oct: no "the contribution bundle 0%" job line under a full bar
            if (pausing) { barLeft = "Pausing - saving what's done"; barRight = {}; }
            else if (finishing && stage != "Packing" && stage != "Done") { barLeft = kMode == wizapp::Mode::beta ? "Saving what's measured" : "Sending what's measured"; barRight = {}; }
            else if (const auto rb = wizapp::readyBar (p, cur); rb.on)
            {   // 18 Oct (NEVER LOOKS FROZEN): getting ready is part of step 3 - the bar stays the measuring % (never back); its own count is the counter
                shownPctT = wizapp::measuringShareTenths (p, shownPctT); barValue = stepBar.next (3, shownPctT / 100.0); barLeft = rb.left; barRight = wizapp::barPercentWords (barValue);
                doneLine = wizapp::counterWords (p, true); snapOnStep (stepText + " - getting ready"); }
            else if (stage == "Measuring") { shownPct = wizapp::measuringPercent (p, shownPct); shownPctT = wizapp::measuringShareTenths (p, shownPctT); barValue = stepBar.next (3, shownPctT / 100.0); snapOnStep (stepText);   /* the % never goes back; after a getting-ready bar the % bar starts from the % */ barLeft = "Measuring"; barRight = wizapp::percentWordsTenths (shownPctT);   /* 18 Oct: no name above the job lines (they name each plugin) */   // 15 Oct: the step-weighted %, one decimal, never backwards
                                             refreshReadyLine(); }   // 14 Oct: n of N, never time left
            else if (stage == "Packing" || stage == "Done") { barValue = 1.0; barLeft = "Packing your results"; barRight = {}; }
            else { barValue = 0.0; barLeft = "Getting ready"; barRight = {}; }   // the first second, before the engine's first progress
        }
        repaint(); snapAtQuarters();
        // 20 Oct (Kathy): the progress line - every 30 s and at every stage change - so a tester's log shows what the screen showed
        if (wizapp::progressLogDue (progressClock, wizapp::screenName (screen) + "|" + stage + "|" + stepText, juce::Time::getMillisecondCounterHiRes()))
            logLine (wizapp::progressLogLine (wizapp::screenName (screen), p, stepText, barValue, barRight, doneLine, wizapp::powerWords (power)));
    }
    void pollEngineExit()
    {
        if (const auto code = engine.poll())
        {
            if (const auto left = wizapp::closeProcessGroup (engine.group); ! left.isEmpty())   // 19 Oct: nothing the engine started outlives it
                logLine ("closed what the engine left running: " + left.joinIntoString (", "));
            awake.hold (false); logLine ("engine ended: " + juce::String (*code) + ", keep-awake released");
            if (screen == Screen::measuring) handleCheckpoint();   // the pause's checkpoint (packed just before the engine ended)
            if (quitting) { juce::JUCEApplication::getInstance()->quit(); return; }
            if (kNetworked && engine.mode == "catalogue" && ! pausing)
            {   // 21 Oct: the plugin list is ready - the server's check, then the scan with its answers
                if (*code == 0 && requestFile().existsAsFile()) { checkCatalogue(); return; }
                title = "Something went wrong"; text = "The wizard couldn't list your plugins (" + juce::String (*code) + "). Open it again to try once more."; showBar = false; repaint(); return;
            }
            if (pausing && *code != 0)
            {   // PAUSED (13 Oct): what is done went with the checkpoint; the same run carries on next time
                pausing = false; logLine ("paused: " + soFar);
                if (quitAfterPause) { juce::JUCEApplication::getInstance()->quit(); return; }
                show (Screen::paused); return;
            }
            switch (wizapp::afterEngine (screen, *code))
            {
                case wizapp::AfterEngine::scanDone:
                {   // /lookup's answer (STUB): already taken part -> nothing runs; else eligibility for the results screen
                    setState ("scanned", true);
                    if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
                    if (kNetworked)
                    {   // 21 Oct: no stub - the server's check decided what is known; the engine's own count is what is left to measure
                        const auto ws = juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString()); const auto an = juce::JSON::parse (answersFile().loadFileAsString());
                        auto* e = new juce::DynamicObject(); e->setProperty ("count", (int) ws.getProperty ("to_measure", 0)); e->setProperty ("eligible", true); e->setProperty ("minimum", 0);
                        e->setProperty ("known", (int) an.getProperty ("have", 0) + (int) an.getProperty ("held", 0)); eligibility = juce::var (e);
                    }
                    else eligibility = stub::lookup (cfg.work, account, device, juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString()));
                    cfg.work.getChildFile ("eligibility.json").replaceWithText (juce::JSON::toString (eligibility)); logLine ((kNetworked ? "eligibility (from the catalogue check): " : "lookup (STUB): ") + juce::JSON::toString (eligibility, true));
                    {   // 15 Oct: one Start carries the whole run - straight into measuring when there is something to measure
                        const auto el = wizapp::eligibilityOf (eligibility); const auto next = wizapp::afterScan (el, kMode);
                        const auto ws = juce::JSON::parse (engineDir().getChildFile ("wizard_state.json").loadFileAsString());
                        logLine ("scan done: " + juce::String (el.count) + " to measure, estimate " + juce::String ((double) ws.getProperty ("to_measure_eta_s", 0.0), 0) + " s at --jobs " + juce::String (cfg.jobs)
                                 + (next == wizapp::Screen::measuring ? " - measuring starts now (no second button)" : juce::String()));
                        show (next);
                    }
                    break;
                }
                case wizapp::AfterEngine::upload:
                    setState ("measure_done", true);
                    if (wizapp::afterMeasure (kMode) == Screen::betaSaved) { saveBetaResults(); setState ("saved", true); }
                    if (kNetworked) cacheRun();   // 21 Oct: the last pack's results are in the outbox
                    show (wizapp::afterMeasure (kMode)); break;
                case wizapp::AfterEngine::left:
                {   // 15 Oct: the run reached the end with plugins left - say what, offer Carry on (never "needs the full run")
                    const auto words = wizapp::leftWords (juce::JSON::parse (engineDir().getChildFile ("left.json").loadFileAsString()));
                    logLine ("reached the end with plugins left: " + words);
                    dawPaused.clear(); resumeTo = Screen::measuring; show (Screen::paused); title = "Almost done"; text = words; repaint(); break;
                }
                case wizapp::AfterEngine::stopped:
                {   // the engine's own words when it said why (a low disk); else the stop note
                    const auto sum = juce::JSON::parse (engineDir().getChildFile ("progress.json").loadFileAsString()).getProperty ("summary", "").toString();
                    title = "Stopped"; text = sum.containsIgnoreCase ("disk space") ? sum : wizapp::stoppedNote() + " - open the wizard again to carry on."; showBar = false; repaint(); break;
                }
            }
        }
    }
    void tickUpload()
    {
        if (kNetworked)
        {   // 21 Oct: the bar is what the server has confirmed; once nothing waits, the run's end is reported and the server answers the promo
            const int sent = sentN, waiting = pendingN; const int total = juce::jmax (1, sent + waiting);
            if (primary.isVisible()) return;   // waiting on the user (Try again)
            barValue = (double) sent / (double) total; barLeft = wizapp::uploadLine (sent, waiting, rejectedN, offlineNow, signInNow); barRight = {};
            {   // 26 Oct (Kathy): the reason and the next try under the title - never "N waiting" with nothing said
                wizapp::UploadView uv; uv.waiting = waiting; uv.offline = offlineNow; uv.signIn = signInNow; uv.stopped = stoppedNow; uv.noRun = noRunNow;
                { const juce::ScopedLock l (wordsLock); uv.words = waitWordsNow; }
                if (! uv.stopped && nextTryMs > 0.0) uv.retryInS = (int) std::ceil (juce::jmax (0.0, nextTryMs - juce::Time::getMillisecondCounterHiRes()) / 1000.0);
                const auto d = wizapp::uploadDetail (uv); text = d.isNotEmpty() ? d : juce::String (wizapp::kUploadText);
                if (const auto said = d.upToFirstOccurrenceOf (" Trying again", false, false); said != lastUploadSaid) { lastUploadSaid = said; if (said.isNotEmpty()) logLine ("sending screen: " + d); }   // what the tester sees, in the log
            }
            repaint(); snapAtQuarters();
            if (signInNow) { title = "Please sign in again"; text = "Your results are kept on this Mac."; primary.setButtonText ("Sign in with EchoJay"); primary.setVisible (true); showBar = false; screen = Screen::welcome; repaint(); return; }
            if (waiting == 0 && ! completing && juce::Time::getMillisecondCounterHiRes() - uploadT0 > 1500.0 && wizcontract::Outbox { outboxDir() }.pendingFiles().isEmpty()) { barLeft = "Checking it arrived"; reportRunEnd(); }
            return;
        }   // a short bar over the (stubbed) send, then the server's genuine check and the promo
        const double t = (juce::Time::getMillisecondCounterHiRes() - uploadT0) / 2500.0;
        barValue = juce::jlimit (0.0, 1.0, t); barLeft = barValue < 1.0 ? "Sending" : "Checking it arrived"; repaint(); snapAtQuarters();
        if (t < 1.3 || receipt.isObject()) return;
        if (account.isVoid()) account = juce::JSON::parse (cfg.work.getChildFile ("account.json").loadFileAsString());
        drainOutbox();
        receipt = stub::upload (cfg.work, engineDir().getChildFile ("contribution.zip"), engineDir().getChildFile ("contribution/manifest.json"), account, device);
        if (! receipt.isObject())
        {   // OFFLINE (13 Oct): kept, sent on the next launch; the code (if earned) comes then
            wizapp::outboxPut (cfg.work, engineDir().getChildFile ("contribution.zip"), engineDir().getChildFile ("contribution/manifest.json"), "final");
            setState ("uploaded", true); logLine ("offline: the final upload is kept for the next launch");
            title = "You're offline"; text = "Your results are saved on this Mac and will be sent the next time you open the wizard while online.";
            showBar = false; primary.setButtonText ("Done"); primary.setVisible (true); screen = Screen::noCode; repaint(); return;
        }
        cfg.work.getChildFile ("receipt.json").replaceWithText (juce::JSON::toString (receipt));
        setState ("uploaded", true); logLine ("uploaded (STUB): " + juce::JSON::toString (receipt, true));
        const auto pr = stub::promo (cfg.work, account, receipt);   // the server grants on the ACCEPTED results, never the client's count
        if (pr.isObject()) { cfg.work.getChildFile ("reward.json").replaceWithText (juce::JSON::toString (pr)); logLine ("promo (STUB): " + juce::JSON::toString (pr, true)); }
        show (wizapp::afterUpload (receipt, pr));
    }
    void autoAdvance()
    {   // rehearsals only: no hands
        ++autoTicks;
        if (screen == Screen::welcome && autoTicks == 6) { const char* e = std::getenv ("EJ_WIZARD_AUTO_EMAIL"); email.setText (e != nullptr && *e ? e : "rehearsal@example.com"); password.setText ("stub-password"); repaint(); snap ("filled"); }
        if (screen == Screen::welcome && autoTicks == 10) onPrimary();
        if (screen == Screen::consent && autoTicks == 6) { snap ("unticked"); agree.setToggleState (true, juce::sendNotification); repaint(); }
        if (screen == Screen::consent && autoTicks == 10) { snap ("ticked"); onPrimary(); }
        if (screen == Screen::scanDone && std::getenv ("EJ_WIZARD_AUTO_ANYWAY") != nullptr && autoTicks == 12 && secondary.isVisible()) { onSecondary(); return; }
        if ((screen == Screen::alreadyDone || screen == Screen::noCode || screen == Screen::betaSaved) && autoTicks == 12) onPrimary();
        if (screen == Screen::reward && autoTicks == 16) onPrimary();
        if ((screen == Screen::scanDone || screen == Screen::crashRecovered) && autoTicks == 12) onPrimary();
        if (screen == Screen::reward && autoTicks == 8 && ! rehearsalQuiet()) { onSecondary(); snap ("copied"); }
        if (screen == Screen::measuring && (cfg.autoSendAt > 0 || cfg.autoPauseAt > 0) && secondary.isEnabled())
        {
            const int n = wizapp::measuredSoFar (juce::JSON::parse (engineDir().getChildFile ("progress.json").loadFileAsString()));
            if (cfg.autoSendAt > 0 && n >= cfg.autoSendAt) { cfg.autoSendAt = 0; snap ("before_send"); finishNow ("rehearsal"); snap ("sending"); }
            if (cfg.autoPauseAt > 0 && n >= cfg.autoPauseAt) { cfg.autoPauseAt = 0; snap ("before_pause"); pauseNow ("rehearsal", false); snap ("pausing"); }
        }
        if (screen == Screen::measuring && cfg.autoAskClose && secondary.isEnabled())
        {   // the close question, shown and snapped, then Escape (keep measuring)
            cfg.autoAskClose = false; forceAsk = true; requestQuit (true); forceAsk = false;
            juce::Timer::callAfterDelay (1500, [safe = juce::Component::SafePointer<WizardComponent> (this)] { if (safe != nullptr && safe->closeAsk != nullptr) { safe->logLine ("rehearsal: the close question, kept measuring"); safe->closeAsk->exitModalState (0); } });
        }
        // 24 Oct: a pause the BATTERY caused waits for the power (it carries on by itself when plugged in) - a rehearsal sees that, never quits it
        // 26 Oct: a rehearsal beside an open DAW (Kathy's Logic) presses Carry on anyway under EJ_WIZARD_AUTO_ANYWAY - rehearsals only (--auto)
        if (screen == Screen::paused && autoTicks == 4 && dawPaused.isNotEmpty() && std::getenv ("EJ_WIZARD_AUTO_ANYWAY") != nullptr) { logLine ("rehearsal: carry on anyway beside " + dawPaused); onAnyway(); return; }
        if (screen == Screen::paused && autoTicks == 8 && ! batteryPaused) { logLine ("rehearsal: quit while paused"); juce::JUCEApplication::getInstance()->quit(); }
        if (screen == Screen::measuring && cfg.autoCloseAfterS > 0.0 && autoTicks * 0.25 >= cfg.autoCloseAfterS)
        { cfg.autoCloseAfterS = 0.0; logLine ("rehearsal: the close button"); juce::JUCEApplication::getInstance()->systemRequestedQuit(); snap ("closing"); }
    }
};

class WizardWindow : public juce::DocumentWindow
{
public:
    explicit WizardWindow (Config c) : juce::DocumentWindow (JUCE_APPLICATION_NAME_STRING, juce::Colours::white, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true); content = new WizardComponent (std::move (c)); setContentOwned (content, true);
        setResizable (false, false); centreWithSize (getWidth(), getHeight()); setVisible (true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    WizardComponent* content = nullptr;
};

class WizardApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }   // 13 Oct: each build its own name - the one-instance guard is keyed on it
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return std::getenv ("EJ_WIZARD_SECOND_INSTANCE") != nullptr; }   // 15 Oct: a rehearsal beside a running wizard (its own --work); never set by a user
    void initialise (const juce::String&) override
    {
        std::signal (SIGINT, [] (int) { appInterrupted() = 1; }); std::signal (SIGTERM, [] (int) { appInterrupted() = 1; });
        window = std::make_unique<WizardWindow> (parseArgs (getCommandLineParameterArray()));
    }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { if (window == nullptr || window->content == nullptr || window->content->requestQuit()) quit(); }
private:
    std::unique_ptr<WizardWindow> window;
};
} // namespace

START_JUCE_APPLICATION (WizardApplication)
