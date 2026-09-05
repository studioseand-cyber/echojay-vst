#include "EJDialWrites.h"
#include "EchoJayBridgedAU.h"   // FIRST: pulls CoreFoundation before JUCE (Point ambiguity)
#include "ChainHost.h"
#include "EJVariantPreference.h"
#include "EJWavesAlias.h"
#include "EJWavesRegistryFeed.h"   // Waves candidates come from the scan, not the catalog
#include "EJNameLadder.h"          // normalizeName, trailingModelNumber, the match ladder
#include "EJParamReads.h"    // 6c section 8: one slot's current reads, header-inline for the pins
#include "EchoJayParamApply.h"
#include "EchoJayParamMaps.h"
#include "EJDialTally.h"          // dial-4 A8: requestedEntryCount, the A7.2 keys semantic
#include "SurgicalEqProcessor.h"   // built-in EQ device (see kBuiltinFormat)
#include "LinkShm.h"               // the EQ curve's grid, clamp and point count
#include "AUEnumerator.h"
#include "NativeClip.h"   // EchoJay_NSLog
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#if JUCE_MAC
 #include <libproc.h>
 #include <dlfcn.h>
 #include <unistd.h>
 #include <signal.h>      // kill(pid, 0): is the process that wrote a death marker still alive
 #include <cerrno>
 #include <sys/param.h>   // MAXCOMLEN
 #include <mach-o/dyld.h>     // _dyld_get_image_header: the process cputype (arch gate)
 #include <mach-o/fat.h>      // FAT_MAGIC, fat_arch (arch gate)
 #include <mach-o/loader.h>   // MH_MAGIC, mach_header (arch gate)
#endif

// The name helpers and the match ladder live in EJNameLadder.h (28 Aug 2026):
// the ladder is what every non-feed path resolves through, and a pin cannot run
// it from the previous build's lib. Unqualified here so the twenty call sites
// below read exactly as they did.
using echojay::normalizeName;
using echojay::trailingModelNumber;

// ---------------------------------------------------------------------------
// File path helpers
// ---------------------------------------------------------------------------
static juce::File appSupportDir()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
           .getChildFile("EchoJay");
}

juce::File ChainHost::getPluginListFile()   { return appSupportDir().getChildFile("chain_plugins.xml"); }
juce::File ChainHost::getEntriesCacheFile() { return appSupportDir().getChildFile("chain_entries.xml"); }
juce::File ChainHost::getParamMapsCacheFile() { return appSupportDir().getChildFile("param_maps.json"); }
juce::File ChainHost::getBlacklistFile()  { return appSupportDir().getChildFile("chain_blacklist.txt"); }
juce::File ChainHost::getDeadmanFile()    { return appSupportDir().getChildFile("chain_load_deadman.txt"); }
juce::File ChainHost::getStateOversizeFile() { return appSupportDir().getChildFile("chain_state_oversize.txt"); }

// Recursive .vst3 collector, defined below; used by the scan above its definition.
static void collectVst3BundlesRecursively(const juce::File& dir, juce::Array<juce::File>& out, int depthLeft);

// ---------------------------------------------------------------------------
// Death markers (17 Aug 2026). A mark is pushed before a call into a hosted
// plugin that can take the process down and popped after it returns; the
// live set is written to chain_load_deadman.<pid>.txt, one line per mark
// (phase, path, name). A file whose pid is no longer running was left by a
// process that died with those calls in flight, and the next ChainHost to
// construct on this machine turns each line into a chain_blacklist.txt row
// ("crashed the host during <phase> (deadman)"), which withholds the plugin
// at feed and at load from then on. Per-pid files because Logic constructs
// EchoJay instances at any moment: instance 5's constructor must not consume
// a mark instance 1 is holding mid-load in the same process. The old single
// chain_load_deadman.txt (validation only, path only) is still consumed.
//
// Covered: instantiate on every route (asyncCreatePlugin, held through the
// caller's callback so completeLoad's graph insert and prepareToPlay are
// inside it), thin-VST3 validation, setStateInformation on restore and on
// the same-plugin replace, and createEditor. NOT covered, on purpose: a
// death while processing or while an editor is open. Those windows hold N
// racked plugins and no attribution; blacklisting all of them would punish
// good plugins for a sibling's crash or a force-quit, and Logic kills the
// AU host on quit without running destructors, so a "racked at death"
// record would fire falsely on every launch.
// ---------------------------------------------------------------------------
namespace {
struct DeathMark { int id; juce::String phase, path, name; };
std::mutex& deathMarkMutex() { static std::mutex m; return m; }
std::vector<DeathMark>& deathMarks() { static std::vector<DeathMark> v; return v; }
juce::File deathMarkFile()
{
    return appSupportDir().getChildFile("chain_load_deadman." + juce::String((int) getpid()) + ".txt");
}
// Caller holds deathMarkMutex()
void writeDeathMarksLocked()
{
    auto f = deathMarkFile();
    if (deathMarks().empty()) { f.deleteFile(); return; }
    juce::String out;
    for (const auto& m : deathMarks())
        out << m.phase << "\t" << m.path << "\t" << m.name << "\n";
    f.getParentDirectory().createDirectory();
    f.replaceWithText(out);
}
int pushDeathMark(const juce::String& phase, const juce::PluginDescription& d)
{
    if (d.fileOrIdentifier.isEmpty()) return 0;   // built-ins: nothing to blacklist
    std::lock_guard<std::mutex> lk(deathMarkMutex());
    static int nextId = 1;
    const int id = nextId++;
    deathMarks().push_back({ id, phase, d.fileOrIdentifier, d.name });
    writeDeathMarksLocked();
    return id;
}
void popDeathMark(int id)
{
    if (id == 0) return;
    std::lock_guard<std::mutex> lk(deathMarkMutex());
    auto& v = deathMarks();
    v.erase(std::remove_if(v.begin(), v.end(), [id](const DeathMark& m) { return m.id == id; }), v.end());
    writeDeathMarksLocked();
}
bool processAlive(int pid)
{
    if (pid <= 0) return false;
    if (::kill((pid_t) pid, 0) == 0) return true;
    return errno == EPERM;   // exists, not ours: alive
}
} // namespace

// Consume every marker file left by a dead process. Runs in the ChainHost
// constructor (main plugin and Link alike: the blacklist is machine-wide).
void ChainHost::consumeDeathMarks()
{
    // Legacy single-path file (validation deadman before 17 Aug 2026).
    // FIRST LINE ONLY: the dashboard-branch builds wrote a second line naming
    // the phase ("state restore"); reading the whole file as one identifier
    // would blacklist a string no plugin path can ever match.
    auto legacy = getDeadmanFile();
    if (legacy.existsAsFile())
    {
        auto lines = juce::StringArray::fromLines(legacy.loadFileAsString());
        const juce::String crashed = lines.size() > 0 ? lines[0].trim() : juce::String();
        const juce::String phase   = lines.size() > 1 ? lines[1].trim() : juce::String("load");
        if (crashed.isNotEmpty())
            addToBlacklist(crashed,
                           phase == "state restore"
                               ? juce::String("crashed the host restoring its saved settings (deadman)")
                               : juce::String("crashed the host during load (deadman)"));
        legacy.deleteFile();
    }
    for (const auto& f : appSupportDir().findChildFiles(juce::File::findFiles, false, "chain_load_deadman.*.txt"))
    {
        const int pid = f.getFileName().fromFirstOccurrenceOf("chain_load_deadman.", false, false)
                                       .upToFirstOccurrenceOf(".txt", false, false).getIntValue();
        if (pid == (int) getpid()) continue;     // ours: live marks, by definition
        if (processAlive(pid)) continue;         // another live host process
        juce::StringArray lines;
        lines.addLines(f.loadFileAsString());
        for (const auto& line : lines)
        {
            juce::StringArray cols;
            cols.addTokens(line, "\t", "");
            if (cols.size() < 2 || cols[1].trim().isEmpty()) continue;
            const juce::String phase = cols[0].trim(), path = cols[1].trim();
            const juce::String name  = cols.size() > 2 ? cols[2].trim() : path;
            EchoJay_NSLog(("EJScan: deadman: \"" + name + "\" was in flight (" + phase
                           + ") when host process " + juce::String(pid) + " died; added to chain_blacklist.txt").toRawUTF8());
            addToBlacklist(path, "crashed the host during " + phase + " (deadman)");
        }
        f.deleteFile();
    }
}

// Session load-failure key (see ChainHost.h: deliberately NOT persisted —
// a load failure means "could not authorise right now", e.g. iLok absent,
// not "not owned")
static juce::String sessionLoadKey(const juce::String& name, const juce::String& format)
{
    return normalizeName(name) + "|" + format;
}

// ---------------------------------------------------------------------------
// Popout-only plugins — shared local list, normalised-name keyed. The cache
// mtime-reloads so a mark made by one host is seen by the other immediately.
// ---------------------------------------------------------------------------
// Feed split (P16) runtime gate. Present => the chain feed narrows to the
// dialable subset; ABSENT (the default) => today's full undifferentiated list
// and no existence query fires at all. A file flag, matching popout_only, so
// the switch is a touch/rm with no rebuild and no UI. This is the field kill
// switch: if the endpoint answers wrongly or a vendor's identities stop
// matching, the feed reverts to full by deleting one file.
static juce::File feedSplitFlagFile() { return appSupportDir().getChildFile("feed_split_on.txt"); }

static juce::File popoutOnlyFile() { return appSupportDir().getChildFile("popout_only.txt"); }
static juce::StringArray& popoutOnlyCache() { static juce::StringArray c; return c; }
static juce::Time& popoutOnlyLoadTime()     { static juce::Time t;        return t; }

static void popoutOnlyReloadIfStale()
{
    auto f  = popoutOnlyFile();
    auto mt = f.getLastModificationTime();
    if (mt == popoutOnlyLoadTime()) return;
    popoutOnlyLoadTime() = mt;
    popoutOnlyCache().clear();
    if (f.existsAsFile())
    {
        popoutOnlyCache().addLines(f.loadFileAsString());
        popoutOnlyCache().trim();
        popoutOnlyCache().removeEmptyStrings();
    }
}

// Format-qualified key: the same plugin's VST3 build is in-process and may
// contain fine when the AU proxy cannot
static juce::String popoutOnlyKey(const juce::String& name, const juce::String& format)
{
    return normalizeName(name) + "|" + format;
}

bool ChainHost::isPopoutOnly(const juce::String& pluginName, const juce::String& format)
{
    popoutOnlyReloadIfStale();
    return popoutOnlyCache().contains(popoutOnlyKey(pluginName, format));
}

void ChainHost::markPopoutOnly(const juce::String& pluginName, const juce::String& format)
{
    // A builtin can never be popout-only: its editor is a plain JUCE
    // component the native-size poll cannot see (31 Aug 2026 — the Link
    // recorded exactly that blindness as a "plugin limitation").
    if (format == kBuiltinFormat) return;
    // Static (machine-wide flag), so it cannot carry the borrowed read-only
    // guard; not on the spec §2.2 shared-file list, and no borrowed-mode
    // path calls it — the byte-identical gate still covers the file.
    popoutOnlyReloadIfStale();
    auto key = popoutOnlyKey(pluginName, format);
    if (key.startsWith("|") || popoutOnlyCache().contains(key)) return;
    popoutOnlyCache().add(key);
    auto f = popoutOnlyFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(popoutOnlyCache().joinIntoString("\n") + "\n");
    popoutOnlyLoadTime() = f.getLastModificationTime();
}

// ---------------------------------------------------------------------------
// Host session identity — "session" means the user-facing DAW's lifetime.
// Resolved ONCE per process (a process cannot change hosts).
// ---------------------------------------------------------------------------
#if JUCE_MAC
static bool bsdInfoFor(pid_t pid, struct proc_bsdinfo& bi)
{
    return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, (int) sizeof(bi)) == (int) sizeof(bi);
}

static juce::String procNameFor(pid_t pid)
{
    char nm[2 * MAXCOMLEN + 1] = {};
    proc_name(pid, nm, sizeof(nm));
    return juce::String(nm);
}

// Names of processes that host plugins on another app's behalf — never the
// user-facing session identity themselves.
static bool looksLikePluginHostHelper(const juce::String& n)
{
    return n.containsIgnoreCase("XPC")
        || n.containsIgnoreCase("AUHostingService")
        || n.containsIgnoreCase("PlugInRunner");
}
#endif

const ChainHost::HostIdentity& ChainHost::getHostIdentity()
{
    static const HostIdentity identity = []
    {
        HostIdentity h;
#if JUCE_MAC
        const pid_t self = getpid();
        pid_t host = self;
        juce::String route = "self";

        // Primary: the responsibility API maps an XPC service straight to the
        // user-facing app it works for (Logic Pro). In-process hosts map to
        // themselves. Private-but-stable symbol, so resolve it dynamically —
        // a missing symbol just degrades to the walk below.
        using RespFn = pid_t (*)(pid_t);
        if (auto respFn = (RespFn) dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid"))
            if (pid_t rp = respFn(self); rp > 0)
            {
                host  = rp;
                route = "responsible-pid";
            }

        // Backstop: if what we have still looks like a hosting helper, walk
        // the parent chain toward the user-facing app. XPC services are
        // children of launchd (pid 1), so this walk cannot cross that gap —
        // it exists for nested/in-process helper arrangements.
        for (int hops = 0; hops < 8; ++hops)
        {
            if (!looksLikePluginHostHelper(procNameFor(host))) break;
            struct proc_bsdinfo bi {};
            if (!bsdInfoFor(host, bi) || bi.pbi_ppid <= 1) break;
            host  = (pid_t) bi.pbi_ppid;
            route = "ppid-walk";
        }

        struct proc_bsdinfo bi {};
        if (!bsdInfoFor(host, bi))
        {
            host  = self;                       // can't even read the candidate
            route = "degraded-self";
            if (!bsdInfoFor(host, bi)) { bi.pbi_start_tvsec = 0; bi.pbi_start_tvusec = 0; }
        }

        h.pid       = (int) host;
        h.startSec  = (juce::int64) bi.pbi_start_tvsec;
        h.startUsec = (juce::int64) bi.pbi_start_tvusec;
        h.name      = procNameFor(host);
        h.degraded  = looksLikePluginHostHelper(h.name) || route == "degraded-self";
#else
        // Non-mac hosts run plugins in-process: our own process IS the
        // session. First-touch time stands in for process start time — it is
        // constant within the process, which is all the match rule needs.
        h.pid      = (int) getpid();
        h.startSec = juce::Time::currentTimeMillis() / 1000;
        h.name     = juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFileNameWithoutExtension();
        juce::String route = "self";
#endif
        EchoJay_NSLog(("EJPrompt: host identity resolved: \"" + h.name + "\" pid=" + juce::String(h.pid)
                       + " start=" + juce::String(h.startSec) + "." + juce::String(h.startUsec)
                       + " via " + route
                       + (h.degraded ? " DEGRADED (sharing limited to this process)" : "")).toRawUTF8());
        return h;
    }();
    return identity;
}

static juce::String describeHostIdentity()
{
    const auto& h = ChainHost::getHostIdentity();
    return "\"" + h.name + "\" pid=" + juce::String(h.pid)
         + " start=" + juce::String(h.startSec) + "." + juce::String(h.startUsec);
}

static void stampHostIdentity(juce::DynamicObject* o)
{
    const auto& h = ChainHost::getHostIdentity();
    o->setProperty("hostPid",       h.pid);
    o->setProperty("hostStartSec",  h.startSec);
    o->setProperty("hostStartUsec", h.startUsec);
    o->setProperty("hostName",      h.name);
}

static bool stampMatchesCurrentHost(juce::DynamicObject* o)
{
    const auto& h = ChainHost::getHostIdentity();
    return (int)         o->getProperty("hostPid")       == h.pid
        && (juce::int64) o->getProperty("hostStartSec")  == h.startSec
        && (juce::int64) o->getProperty("hostStartUsec") == h.startUsec;
}

static juce::String describeStamp(juce::DynamicObject* o)
{
    return "\"" + o->getProperty("hostName").toString() + "\" pid="
         + o->getProperty("hostPid").toString()
         + " start=" + o->getProperty("hostStartSec").toString();
}

// ---------------------------------------------------------------------------
// Session project name — shared file, mtime-cached (popout_only pattern).
// The cache holds the VALIDATED value: empty when the file's host stamp does
// not match the current host, so a previous DAW session's value is invisible
// to all callers (they prompt instead). updatedAt stays purely informational.
// ---------------------------------------------------------------------------
static juce::File sessionProjectFile() { return appSupportDir().getChildFile("session_project.json"); }
static juce::String& sessionProjectCache() { static juce::String s; return s; }
static juce::Time& sessionProjectLoadTime() { static juce::Time t; return t; }

// Shared reload for both session files: returns the validated value ("" on
// stamp mismatch) and logs the match outcome once per file change.
static juce::String loadValidatedSessionValue(const juce::File& f,
                                              const juce::String& jsonKey,
                                              const juce::String& logNoun)
{
    if (!f.existsAsFile()) return {};
    auto v = juce::JSON::parse(f.loadFileAsString());
    auto* o = v.getDynamicObject();
    if (o == nullptr) return {};
    auto val = o->getProperty(jsonKey).toString().trim();
    if (val.isEmpty()) return {};
    if (stampMatchesCurrentHost(o))
    {
        EchoJay_NSLog(("EJPrompt: session " + logNoun + " \"" + val
                       + "\" same host, adopted (" + describeStamp(o) + ")").toRawUTF8());
        return val;
    }
    EchoJay_NSLog(("EJPrompt: session " + logNoun + " \"" + val
                   + "\" from previous host, ignoring, will prompt (file " + describeStamp(o)
                   + " vs current " + describeHostIdentity() + ")").toRawUTF8());
    return {};
}

juce::String ChainHost::getSessionProjectName()
{
    auto f  = sessionProjectFile();
    auto mt = f.getLastModificationTime();
    if (mt != sessionProjectLoadTime())
    {
        sessionProjectLoadTime() = mt;
        sessionProjectCache() = loadValidatedSessionValue(f, "name", "project");
    }
    return sessionProjectCache();
}

void ChainHost::publishSessionProjectName(const juce::String& name)
{
    auto trimmed = name.trim();
    if (getSessionProjectName() == trimmed) return;   // no-op republish (same value, same host)
    auto* o = new juce::DynamicObject();
    o->setProperty("name",      trimmed);
    o->setProperty("updatedAt", juce::Time::getCurrentTime().toISO8601(true));
    stampHostIdentity(o);
    auto f = sessionProjectFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::JSON::toString(juce::var(o), true));
    sessionProjectCache()    = trimmed;
    sessionProjectLoadTime() = f.getLastModificationTime();
    EchoJay_NSLog(("EJPrompt: published session project \"" + trimmed
                   + "\" as " + describeHostIdentity()).toRawUTF8());
}

// Session genre — identical pattern, separate file (independent mtimes)
static juce::File sessionGenreFile() { return appSupportDir().getChildFile("session_genre.json"); }
static juce::String& sessionGenreCache() { static juce::String s; return s; }
static juce::Time& sessionGenreLoadTime() { static juce::Time t; return t; }

juce::String ChainHost::getSessionGenre()
{
    auto f  = sessionGenreFile();
    auto mt = f.getLastModificationTime();
    if (mt != sessionGenreLoadTime())
    {
        sessionGenreLoadTime() = mt;
        sessionGenreCache() = loadValidatedSessionValue(f, "genre", "genre");
    }
    return sessionGenreCache();
}

void ChainHost::publishSessionGenre(const juce::String& genre)
{
    auto trimmed = genre.trim();
    if (trimmed.isEmpty() || getSessionGenre() == trimmed) return;
    auto* o = new juce::DynamicObject();
    o->setProperty("genre",     trimmed);
    o->setProperty("updatedAt", juce::Time::getCurrentTime().toISO8601(true));
    stampHostIdentity(o);
    auto f = sessionGenreFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::JSON::toString(juce::var(o), true));
    sessionGenreCache()    = trimmed;
    sessionGenreLoadTime() = f.getLastModificationTime();
    EchoJay_NSLog(("EJPrompt: published session genre \"" + trimmed
                   + "\" as " + describeHostIdentity()).toRawUTF8());
}

static juce::File sessionAutoProjectFile() { return appSupportDir().getChildFile("session_autoproject.json"); }
static juce::String& sessionAutoProjectCache() { static juce::String s; return s; }
static juce::Time& sessionAutoProjectLoadTime() { static juce::Time t; return t; }

juce::String ChainHost::getSessionAutoProject()
{
    auto f  = sessionAutoProjectFile();
    auto mt = f.getLastModificationTime();
    if (mt != sessionAutoProjectLoadTime())
    {
        sessionAutoProjectLoadTime() = mt;
        // Reuse the shared validator but keep it quiet — this is not a prompt
        // surface; a stamp mismatch simply means "new session, pick a name".
        if (!f.existsAsFile()) sessionAutoProjectCache() = {};
        else
        {
            auto v = juce::JSON::parse(f.loadFileAsString());
            auto* o = v.getDynamicObject();
            sessionAutoProjectCache() =
                (o != nullptr && stampMatchesCurrentHost(o))
                    ? o->getProperty("name").toString().trim() : juce::String();
        }
    }
    return sessionAutoProjectCache();
}

void ChainHost::setSessionAutoProject(const juce::String& name)
{
    auto trimmed = name.trim();
    if (getSessionAutoProject() == trimmed) return;
    auto* o = new juce::DynamicObject();
    o->setProperty("name",      trimmed);
    o->setProperty("updatedAt", juce::Time::getCurrentTime().toISO8601(true));
    stampHostIdentity(o);
    auto f = sessionAutoProjectFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::JSON::toString(juce::var(o), true));
    sessionAutoProjectCache()    = trimmed;
    sessionAutoProjectLoadTime() = f.getLastModificationTime();
}

// ---------------------------------------------------------------------------
// Built-in devices
// ---------------------------------------------------------------------------
// Every built-in now comes from BuiltinDeviceRegistry, which each device adds
// itself to from its own .cpp. Identity (name, identifier, uid) lives with the
// device, so nothing here has to be edited when one is added.
juce::PluginDescription ChainHost::builtinDescriptionFor(const juce::String& rawName)
{
    if (auto* d = BuiltinDeviceRegistry::instance().findByName(stripParenthetical(rawName.trim())))
        return BuiltinDeviceRegistry::descriptionFor(*d);
    return {};
}

bool ChainHost::isBuiltinDescription(const juce::PluginDescription& d) noexcept
{
    return BuiltinDeviceRegistry::isBuiltinDescription(d);
}

bool ChainHost::isBuiltinName(const juce::String& rawName)
{
    return BuiltinDeviceRegistry::instance()
             .findByName(stripParenthetical(rawName.trim())) != nullptr;
}

bool ChainHost::isBuiltinSlot(int i) const
{
    if (i < 0 || i >= (int)slots_.size()) return false;
    return isBuiltinDescription(slots_[(size_t)i].desc);
}

juce::PluginDescription ChainHost::preferInlineHostableDesc(const juce::PluginDescription& d)
{
    if (isBuiltinDescription(d)) return d;   // never swapped for a VST3 build
    if (d.pluginFormatName != "AudioUnit") return d;
    if (editorPlacement(d.name, "AudioUnit") != EditorPlacement::Float) return d;
    auto alt = findVst3Alternative(d.name);
    if (alt.name.isNotEmpty())
    {
        EchoJay_NSLog(("ChainHost: hosting VST3 build of \"" + d.name
                       + "\" -> \"" + alt.name + "\" (AU editor is popout-only)").toRawUTF8());
        return alt;
    }
    return d;
}

juce::PluginDescription ChainHost::findVst3Alternative(const juce::String& pluginName)
{
    // AU mono/stereo suffix "(m)"/"(s)" maps to VST3 shell naming "Mono"/"Stereo"
    auto lower = pluginName.trim().toLowerCase();
    juce::String variant = lower.endsWith("(m)") ? "mono"
                         : lower.endsWith("(s)") ? "stereo" : juce::String();
    auto wantNorm = normalizeName(stripParenthetical(pluginName));
    auto matches = [&](const juce::String& entryName)
    {
        if (namesMatchLoose(pluginName, entryName)) return true;
        auto en = normalizeName(entryName);
        if (wantNorm.isEmpty() || !en.startsWith(wantNorm)) return false;
        return variant.isEmpty() || en.contains(variant);
    };

    // 1. Direct VST3 entry / previously deep-scanned cache. A withheld VST3
    //    (crash-blacklisted, or no slice for this process) is not an
    //    alternative: the loader would refuse or fail it, and the caller
    //    falls back to the AU it started with. pluginsMutex_ is held.
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        for (auto& d : entries_)
            if (d.pluginFormatName == "VST3" && matches(d.name)
                && ! isWithheld(withholdReasonLocked(d)))
                return d;
        for (const auto& d : knownPlugins_.getTypes())
            if (d.pluginFormatName == "VST3" && matches(d.name)
                && ! isWithheld(withholdReasonLocked(d)))
                return d;
    }

    // 2. WaveShell VST3 modules bundle many plugins; the thin scan records
    //    only the shell filename. Deep-enumerate on demand (instantiates the
    //    module once; results land in knownPlugins_ so this is one-time).
    auto* vst3 = getFormatByName("VST3");
    if (vst3 == nullptr) return {};
    auto paths = vst3->getDefaultLocationsToSearch();
    for (int pi = 0; pi < paths.getNumPaths(); ++pi)
    {
        auto dir = paths[pi];
        if (!dir.isDirectory()) continue;
        for (auto& f : dir.findChildFiles(juce::File::findDirectories | juce::File::findFiles,
                                          false, "*.vst3"))
        {
            if (!f.getFileName().containsIgnoreCase("WaveShell")) continue;
            EchoJay_NSLog(("ChainHost: deep-scanning " + f.getFileName()
                           + " for \"" + pluginName + "\"...").toRawUTF8());
            juce::OwnedArray<juce::PluginDescription> types;
            {
                std::lock_guard<std::mutex> lock(pluginsMutex_);
                knownPlugins_.scanAndAddFile(f.getFullPathName(), true, types, *vst3);
            }
            EchoJay_NSLog(("ChainHost: " + juce::String(types.size())
                           + " plugins enumerated in " + f.getFileName()).toRawUTF8());
            for (auto* d : types)
                if (matches(d->name))
                    return *d;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Per-slot wet/dry blend node.
//
// Inputs 0-1 = the slot plugin's (wet) output; inputs 2-3 = the slot's dry
// input, tapped in parallel by rebuildGraph(). JUCE's render sequence aligns
// ALL of a node's input channels to the same max source latency (it computes
// one maxInputLatency per node and inserts DelayChannelOps on the earlier
// legs), so a latent plugin's dry leg arrives sample-aligned by construction.
//
// Phase caveat (inherent, not a bug): plugins that rotate phase — most
// minimum-phase EQs — comb-filter against the dry signal at partial wet.
// Latency alignment cannot remove that; it is true of every wet/dry blend.
class SlotWetBlend : public juce::AudioProcessor
{
public:
    explicit SlotWetBlend(std::shared_ptr<std::atomic<float>> wet)
        : juce::AudioProcessor(BusesProperties()
              .withInput("Wet", juce::AudioChannelSet::stereo(), true)
              .withInput("Dry", juce::AudioChannelSet::stereo(), true)
              .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
          wet_(std::move(wet)) {}

    void prepareToPlay(double sampleRate, int) override
    {
        smooth_.reset(sampleRate, 0.05);
        smooth_.setCurrentAndTargetValue(wet_ ? wet_->load(std::memory_order_relaxed) : 1.0f);
        // The tallies clear ONLY on a sample-rate change (a new source, or a
        // re-prepare that changes what a sample means). Hosts re-prepare on
        // buffer-size changes and transport events too, and a tally that
        // forgot the track on every play would never reach its floor.
        if (sampleRate != tallySr_)
        {
            tallySr_ = sampleRate;
            inTally_.prepare(sampleRate);
            outTally_.prepare(sampleRate);
        }
    }
    void releaseResources() override {}

    // Running level on BOTH legs (17 Aug 2026): inputs 2/3 are this slot's
    // input (the dry tap), inputs 0/1 the plugin's output. Measured before
    // the fully-wet early-out below, so a slot at 100% is measured too.
    // Cost is a few flops per sample; see EchoJayLevelTally.h.
    const echojay::LevelTally& inTally()  const { return inTally_; }
    const echojay::LevelTally& outTally() const { return outTally_; }
    void resetTallies() { inTally_.reset(); outTally_.reset(); }
    void restoreTallies(const juce::var& in, const juce::var& out) { inTally_.fromVar(in); outTally_.fromVar(out); }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const float target = juce::jlimit(0.0f, 1.0f,
            wet_ ? wet_->load(std::memory_order_relaxed) : 1.0f);
        smooth_.setTargetValue(target);

        const int n = buffer.getNumSamples();
        {
            const int nch = buffer.getNumChannels();
            if (nch >= 1)
                outTally_.push(buffer.getReadPointer(0), nch >= 2 ? buffer.getReadPointer(1) : nullptr, n);
            if (nch >= 3)
                inTally_.push(buffer.getReadPointer(2), nch >= 4 ? buffer.getReadPointer(3) : nullptr, n);
        }
        // Fully wet and settled: output channels 0/1 already hold the wet
        // signal in-place — nothing to do (zero cost at the default setting).
        if (!smooth_.isSmoothing() && target >= 0.9995f)
            return;
        if (buffer.getNumChannels() < 4) { smooth_.skip(n); return; }

        auto* outL = buffer.getWritePointer(0);
        auto* outR = buffer.getWritePointer(1);
        auto* dryL = buffer.getReadPointer(2);
        auto* dryR = buffer.getReadPointer(3);
        for (int i = 0; i < n; ++i)
        {
            const float w = smooth_.getNextValue();
            outL[i] = outL[i] * w + dryL[i] * (1.0f - w);
            outR[i] = outR[i] * w + dryR[i] * (1.0f - w);
        }
    }

    const juce::String getName() const override        { return "EJ Slot Wet/Dry"; }
    bool acceptsMidi() const override                  { return false; }
    bool producesMidi() const override                 { return false; }
    double getTailLengthSeconds() const override       { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                    { return false; }
    int getNumPrograms() override                      { return 1; }
    int getCurrentProgram() override                   { return 0; }
    void setCurrentProgram(int) override               {}
    const juce::String getProgramName(int) override    { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override { return true; }

private:
    std::shared_ptr<std::atomic<float>> wet_;
    juce::SmoothedValue<float>          smooth_;
    // Plain (dBFS RMS) on the slot legs: a threshold is set in the units the
    // detector sees, and out minus in cancels any weighting anyway.
    echojay::LevelTally                 inTally_  { echojay::LevelTally::Weighting::Plain };
    echojay::LevelTally                 outTally_ { echojay::LevelTally::Weighting::Plain };
    double                              tallySr_ = 0.0;
};

// Defined up here, not with the rest of the settings-cache code below:
// ~ChainHost destroys a unique_ptr to it, so the type has to be complete
// before the destructor.
struct ChainHost::StateCacheTimer : juce::Timer
{
    explicit StateCacheTimer(ChainHost& o) : owner(o) {}
    void timerCallback() override { owner.stateCacheTick(); }
    ChainHost& owner;
};

// Runtime latency rebuild (16 Aug 2026). triggerAsyncUpdate is safe from
// the audio thread; handleAsyncUpdate lands on the message thread and only
// (re)starts the debounce, so a burst of latencyChanged notifications
// collapses into one timer callback, which is the one place the graph is
// touched.
struct ChainHost::LatencyRebuilder : juce::AsyncUpdater, juce::Timer
{
    explicit LatencyRebuilder(ChainHost& o) : owner(o) {}
    ~LatencyRebuilder() override { cancelPendingUpdate(); stopTimer(); }
    void handleAsyncUpdate() override { startTimer(kDebounceMs); }
    void timerCallback() override { stopTimer(); owner.rebuildForLatencyIfChanged(); }
    static constexpr int kDebounceMs = 80;
    ChainHost& owner;
};

ChainHost::ChainHost(Mode mode) : mode_(mode)
{
    juce::addDefaultFormatsToManager(formatManager_);

    graph_ = std::make_unique<juce::AudioProcessorGraph>();
    latencyRebuilder_ = std::make_unique<LatencyRebuilder>(*this);

    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode_  = graph_->addNode(std::make_unique<IOProc>(IOProc::audioInputNode));
    outputNode_ = graph_->addNode(std::make_unique<IOProc>(IOProc::audioOutputNode));

    rebuildGraph(); // passthrough (no slots yet)

    // BORROWED MODE (spec §2.1): plugin resolution rides the PRIMARY's lists
    // read-only — implemented as reading the same shared files the primary
    // wrote (loadFromDisk and the blacklist are pure reads; the read-only
    // rule bans WRITES). Skipped: param maps, bootstrap, helper catalogue,
    // the scan thread, and death-mark CONSUMPTION — that stays the primary's
    // job; a borrowed host is created lazily after a primary exists, and
    // consuming here would race it for the same marker files.
    if (mode_ == Mode::Borrowed)
    {
        loadFromDisk();
        reloadBlacklistFromDisk();
        return;
    }

    loadFromDisk();
    loadParamMapsFromDisk();
    mergeBootstrapMaps();
    loadHelperCatalogue();

    consumeDeathMarks();
}

// Process-lifetime store for hosted plugin instances — INTENTIONALLY leaked
// (never destroyed). Plugins with leaked repeating UI timers (AMEK EQ 250)
// crash the shared AU hosting service whenever their instance memory or code
// is freed: disposing on removeSlot crashed (use-after-free at 0x178), and
// disposing in ~ChainHost crashed on the next EchoJay open (timer fired into
// UNLOADED code, jump to 0x0). Keeping the instances alive until the process
// exits makes the stray timers permanently harmless; the OS reclaims
// everything when the hosting service quits.
static std::vector<juce::AudioProcessorGraph::Node::Ptr>& leakedNodeStore()
{
    static auto* store = new std::vector<juce::AudioProcessorGraph::Node::Ptr>();
    return *store;
}

ChainHost::~ChainHost()
{
    cancelFlag_.store(true);
    if (scanThread_.joinable()) scanThread_.join();

    // Stop the cache timer and drop every listener BEFORE the nodes are
    // parked in the process-lifetime store. Those instances outlive us by
    // design, so a listener left attached would be a call into freed memory
    // the first time a stray plugin timer fired.
    if (stateCacheTimer_) stateCacheTimer_->stopTimer();
    if (latencyRebuilder_) { latencyRebuilder_->cancelPendingUpdate(); latencyRebuilder_->stopTimer(); }
    stateCacheEnabled_ = false;
    for (int i = 0; i < (int)slots_.size(); ++i)
        detachHostedListener(i);
    for (auto& n : graveyard_)
        if (n)
            if (auto* p = n->getProcessor())
                p->removeListener(this);

    // Detach nodes from the graph, then park them in the process-lifetime
    // store instead of letting them be destroyed (see leakedNodeStore above)
    if (graph_)
    {
        for (auto& c : graph_->getConnections()) graph_->removeConnection(c);
        for (auto& s : slots_)
            if (s.node)
            {
                graph_->removeNode(s.node->nodeID);
                leakedNodeStore().push_back(s.node);
            }
    }
    for (auto& n : graveyard_)
        if (n) leakedNodeStore().push_back(n);
    graveyard_.clear();
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------
void ChainHost::prepare(double sampleRate, int blockSize)
{
    sampleRate_ = sampleRate;
    blockSize_  = blockSize;
    prepared_   = true;

    // Master wet/dry resources — dry copy scratch + latency-alignment ring
    dryScratch_.setSize(2, juce::jmax(blockSize, 16));
    dryRing_.setSize(2, kDryRingLen);
    dryRing_.clear();
    dryRingWrite_ = 0;
    masterWetSmooth_.reset(sampleRate, 0.05);
    masterWetSmooth_.setCurrentAndTargetValue(masterWet_.load(std::memory_order_relaxed));
    preGainSmooth_.reset(sampleRate, 0.03);
    preGainSmooth_.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(preGainDb_.load(std::memory_order_relaxed)));
    // Running level: cleared only when the sample rate CHANGES (see
    // SlotWetBlend::prepareToPlay for why not on every prepare)
    if (sampleRate != tallySr_)
    {
        tallySr_ = sampleRate;
        chainInTally_.prepare(sampleRate);
        chainOutTally_.prepare(sampleRate);
    }

    graph_->setPlayConfigDetails(2, 2, sampleRate, blockSize);
    graph_->prepareToPlay(sampleRate, blockSize);
}

void ChainHost::release()
{
    if (graph_) graph_->releaseResources();
    prepared_ = false;
}

void ChainHost::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Skip the graph entirely when no plugins are loaded — pure passthrough.
    // An empty AudioProcessorGraph with only IO nodes can drop audio under
    // certain prepare/rebuild orderings; bypassing it avoids that entirely.
    // (Also means master wet/dry costs nothing on an empty chain.)
    if (!prepared_ || !graph_) return;
    // Running level at the chain INPUT, before anything, including on an
    // empty rack: a build on an empty rack still needs to know the level.
    if (buffer.getNumChannels() >= 1)
        chainInTally_.push(buffer.getReadPointer(0),
                           buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : nullptr,
                           buffer.getNumSamples());

    // PRE-CHAIN GAIN, after the raw-input tally and before anything else, so
    // chainInTally_ is the raw input and everything downstream (slot 1's input
    // tally = the operating level, the graph, chainOutTally_) sees the trim.
    // It is part of what EchoJay does, not a hidden compensation: a DAW bypass
    // compares the raw input against pre-gain + chain.
    {
        const float target = juce::Decibels::decibelsToGain(preGainDb_.load(std::memory_order_relaxed));
        preGainSmooth_.setTargetValue(target);
        if (preGainSmooth_.isSmoothing())
        {
            const int nn = buffer.getNumSamples();
            const int nc = buffer.getNumChannels();
            for (int i = 0; i < nn; ++i)
            {
                const float g = preGainSmooth_.getNextValue();
                for (int c = 0; c < nc; ++c) buffer.getWritePointer(c)[i] *= g;
            }
        }
        else if (preGainSmooth_.getCurrentValue() != 1.0f)
        {
            buffer.applyGain(preGainSmooth_.getCurrentValue());
        }
    }
    if (!hasActiveSlots_.load())
    {
        // Passthrough: the chain output IS the input
        if (buffer.getNumChannels() >= 1)
            chainOutTally_.push(buffer.getReadPointer(0),
                                buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : nullptr,
                                buffer.getNumSamples());
        return;   // buffer passes through untouched
    }

    const int n   = buffer.getNumSamples();
    const int chs = juce::jmin(2, buffer.getNumChannels());
    const float target = juce::jlimit(0.0f, 1.0f,
                                      masterWet_.load(std::memory_order_relaxed));
    masterWetSmooth_.setTargetValue(target);

    // Push the pre-graph input into the dry ring EVERY block (cheap copy), so
    // history is already aligned the moment the knob leaves 100% — no stale
    // window on the first grab.
    const bool ringOk = (n <= dryScratch_.getNumSamples() && chs > 0
                         && dryRing_.getNumSamples() == kDryRingLen);
    if (ringOk)
    {
        for (int rc = 0; rc < 2; ++rc)
        {
            const float* src = buffer.getReadPointer(juce::jmin(rc, chs - 1));
            float* ring = dryRing_.getWritePointer(rc);
            int w = dryRingWrite_;
            for (int i = 0; i < n; ++i)
            {
                ring[w] = src[i];
                if (++w == kDryRingLen) w = 0;
            }
        }
    }

    const bool blendActive = masterWetSmooth_.isSmoothing() || target < 0.9995f;
    if (!ringOk || !blendActive)
    {
        // Fully wet and settled — plain graph pass, keep the smoother in time
        if (ringOk) dryRingWrite_ = (dryRingWrite_ + n) % kDryRingLen;
        masterWetSmooth_.skip(n);
        graph_->processBlock(buffer, midi);
        chainOutTally_.push(buffer.getReadPointer(0), chs >= 2 ? buffer.getReadPointer(1) : nullptr, n);
        return;
    }

    // Read the dry leg back delayed by the chain's reported latency so wet
    // and dry are sample-aligned. The graph maintains its own latency total
    // (set from the render sequence), which already accounts for the per-slot
    // blend topology. Phase caveat: latency alignment cannot undo plugins
    // that ROTATE phase (most minimum-phase EQs) — those comb-filter against
    // the dry signal at partial wet. Inherent to wet/dry, not a bug.
    const int d = juce::jlimit(0, kDryRingLen - 1, graph_->getLatencySamples());
    for (int rc = 0; rc < chs; ++rc)
    {
        const float* ring = dryRing_.getReadPointer(rc);
        float* dst = dryScratch_.getWritePointer(rc);
        int r = dryRingWrite_ - d;
        if (r < 0) r += kDryRingLen;
        for (int i = 0; i < n; ++i)
        {
            dst[i] = ring[r];
            if (++r == kDryRingLen) r = 0;
        }
    }
    dryRingWrite_ = (dryRingWrite_ + n) % kDryRingLen;

    graph_->processBlock(buffer, midi);   // buffer is now the wet signal

    for (int i = 0; i < n; ++i)
    {
        const float w = masterWetSmooth_.getNextValue();
        for (int c = 0; c < chs; ++c)
        {
            float* out = buffer.getWritePointer(c);
            out[i] = out[i] * w + dryScratch_.getReadPointer(c)[i] * (1.0f - w);
        }
    }
    // Running level at the chain OUTPUT: post master wet, pre bus trim
    chainOutTally_.push(buffer.getReadPointer(0), chs >= 2 ? buffer.getReadPointer(1) : nullptr, n);
}

// ---------------------------------------------------------------------------
// List refresh — no plugin instantiation
// ---------------------------------------------------------------------------
void ChainHost::startScan()
{
    // EJScan instrumentation (13 Aug 2026): a July chain_entries.xml served
    // five weeks of sessions and a Scan press produced no rewrite, and this
    // path had NO logging, so which mechanism ate the press is unknowable
    // after the fact. Every decision on the path now says what it did; next
    // time the log names it.
    if (scanning_.load())
    {
        const auto ageS = scanStartedAtMs_ > 0
            ? (juce::Time::currentTimeMillis() - scanStartedAtMs_) / 1000
            : (juce::int64) -1;
        EchoJay_NSLog(("EJScan: press REJECTED, a scan is already running ("
                       + (ageS >= 0 ? juce::String(ageS) + "s old" : juce::String("age unknown"))
                       + "). A scan that never completes bricks this button "
                         "silently; if this line repeats with a growing age, "
                         "that is the stuck-flag case.").toRawUTF8());
        return;
    }
    EchoJay_NSLog("EJScan: press accepted, scan starting");
    scanStartedAtMs_ = juce::Time::currentTimeMillis();
    cancelFlag_.store(false);
    scanning_.store(true);
    scanProgress_.store(0.0f);
    if (scanThread_.joinable()) scanThread_.join();
    scanThread_ = std::thread([this] { doRefresh(); });
}

void ChainHost::cancelScan()
{
    cancelFlag_.store(true);
    if (scanThread_.joinable()) scanThread_.join();
}

juce::String ChainHost::getScanStatus() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return scanStatus_;
}

void ChainHost::setScanStatus(const juce::String& s)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    scanStatus_ = s;
}

void ChainHost::doRefresh()
{    // Borrowed mode is READ-ONLY on every shared file (spec §2.2): one
    // writer per file, and the primary host is it.
    if (mode_ == Mode::Borrowed) return;

    struct Finally { ChainHost* h; ~Finally() { h->scanning_.store(false); } } fin{this};

    // The FILE is the authority at scan time: deleting a line from
    // chain_blacklist.txt re-enables that plugin on the next scan without
    // restarting the host, exactly as the file's header promises. Safe to
    // replace the in-memory set because every add persists immediately
    // (addToBlacklist), so memory never holds an entry the file lacks.
    reloadBlacklistFromDisk();
    reloadStateOversizeFromDisk();
    // Union the helper's catalogue BEFORE the VST3 un-thin join below reads
    // knownPlugins_, so a background sweep that finished since startup lands in
    // this scan's rows rather than waiting for a restart.
    loadHelperCatalogue();

    juce::Array<juce::PluginDescription> auEntries, vst3Entries;

#if JUCE_MAC
    setScanStatus("Reading AU registry...");
    for (const auto& e : enumerateAUs())
    {
        juce::PluginDescription pd;
        pd.name             = e.name;
        pd.manufacturerName = e.manufacturer;
        pd.pluginFormatName = "AudioUnit";
        pd.fileOrIdentifier = e.identifier;
        pd.category         = e.category;
        pd.version          = e.version;
        pd.isInstrument     = e.isInstrument;
        pd.uniqueId = pd.deprecatedUid = e.uniqueId;
        auEntries.add(pd);
    }
    // Phase count: how many AU rows, and how many of those carry a real
    // plist-resolved version. The shape test is safe HERE because the
    // enumerator's output is only ever a dotted version or the triple, and
    // the triple always contains commas.
    {
        int plistResolved = 0;
        for (const auto& d : auEntries)
            if (! d.version.containsChar(',')) ++plistResolved;
        EchoJay_NSLog(("EJScan: AU registry read, " + juce::String(auEntries.size())
                       + " entr(ies), " + juce::String(plistResolved)
                       + " with plist version(s)").toRawUTF8());
    }
    scanProgress_.store(0.5f);
#endif

    // RECORD, no fix (13 Aug 2026): cancelScan() has no callers, so this
    // early return is UNREACHABLE. It is kept only because removing dead
    // code is not this commit's job; do not trace it as a way a scan ends.
    if (cancelFlag_.load()) { std::lock_guard<std::mutex> lk(pluginsMutex_); entries_ = auEntries; return; }

    setScanStatus("Reading VST3 folders...");
    // The architecture memo is per path and per process, never persisted;
    // a rescan is the one moment a bundle on disk may have been replaced
    // (a universal update over an Intel-only install), so it is re-judged
    // from here on. NOTHING is filtered here: the scan enumerates every
    // bundle and the cache below keeps all of them (ArchVerdict, header).
    {
        std::lock_guard<std::mutex> lk(archMutex_);
        archCache_.clear();
    }
    juce::StringArray withheldByBlacklist;
    auto* vst3Fmt = getFormatByName("VST3");
    if (vst3Fmt)
    {
        auto paths = vst3Fmt->getDefaultLocationsToSearch();
        for (int pi = 0; pi < paths.getNumPaths(); ++pi)
        {
            if (cancelFlag_.load()) break;
            auto dir = paths[pi];
            if (!dir.isDirectory()) continue;

            // Recursive since 18 Aug 2026 (was top level only): vendor
            // subfolders (UA, Melda, Kilohearts, Soundtoys, Slate) hold most
            // of the library. Depth 4 covers the measured nesting of 2 with room.
            juce::Array<juce::File> found;
            collectVst3BundlesRecursively(dir, found, 4);

            for (auto& f : found)
            {
                if (cancelFlag_.load()) break;
                juce::String path = f.getFullPathName();
                // Recorded, NOT dropped (15 Aug 2026, evening). The row is
                // kept in entries_ and in the shared cache like any other and
                // withheld at the feed sites through
                // WithholdReason::CrashBlacklisted, the same relocation the
                // dedupe (1069ffe) and the architecture gate (037c36d) had:
                // a scan-time drop decided what the OTHER host could see and
                // made CrashBlacklisted unreachable after any rescan. This
                // list is only what the scan SAW on the blacklist, for the
                // log line below.
                if (isBlacklisted(path))
                    withheldByBlacklist.add(f.getFileNameWithoutExtension());

                bool usedCache = false;
                {
                    std::lock_guard<std::mutex> lk(pluginsMutex_);
                    for (const auto& d : knownPlugins_.getTypes())
                    {
                        if (d.fileOrIdentifier == path)
                        {
                            vst3Entries.add(d);
                            usedCache = true;
                        }
                    }
                }

                if (!usedCache)
                {
                    juce::PluginDescription thin;
                    thin.name             = f.getFileNameWithoutExtension();
                    thin.pluginFormatName = "VST3";
                    thin.fileOrIdentifier = path;
                    thin.category         = "Effect";
                    vst3Entries.add(thin);
                }
            }
        }
    }

    {
        int thin = 0;
        for (const auto& d : vst3Entries)
            if (d.version.isEmpty()) ++thin;
        EchoJay_NSLog(("EJScan: VST3 folders read, " + juce::String(vst3Entries.size())
                       + " row(s), " + juce::String(vst3Entries.size() - thin)
                       + " from the validated cache, " + juce::String(thin)
                       + " thin (unvalidated)").toRawUTF8());
    }
    // Logged EVERY scan, including when empty: the blacklist is a permanent
    // per-path exclusion with no Settings UI yet, and this line is its only
    // discoverability. A plugin silently missing here is a plugin silently
    // deleted from someone's catalogue. Since 15 Aug 2026 (evening) the
    // rows named here are KEPT in the list and the cache and withheld at
    // feed time (WithholdReason::CrashBlacklisted); the count and the names
    // are what the scan saw on the blacklist, unchanged in meaning for the
    // reader of this line.
    EchoJay_NSLog(("EJScan: " + juce::String(withheldByBlacklist.size())
                   + " entr(ies) withheld by crash blacklist"
                   + (withheldByBlacklist.isEmpty()
                          ? juce::String()
                          : ": " + withheldByBlacklist.joinIntoString(", "))).toRawUTF8());
    scanProgress_.store(0.9f);

    // Both formats are kept. The cache is shared between the AU and VST3
    // instances (see the FULL-list comment on the write below), so a
    // name-based drop here decides what the OTHER host can see: a VST3 row
    // discarded because an AU shares its name is then removed again by
    // chainFormatFilter_ under VST3 hosting, and the plugin vanishes.
    // Deduplication belongs at feed-build time, where the format filter
    // already runs (collapseAuPreferring, empty-filter branch). Measured
    // 15 Aug 2026: the old drop cost 261 of 449 VST3 rows, leaving a
    // Reaper user 18 loadable plugins out of 181 offered.
    juce::Array<juce::PluginDescription> collected;
    for (auto& d : auEntries)   collected.add(d);
    for (auto& d : vst3Entries) collected.add(d);

    // stable_sort: equal-name rows (now one AU + one VST3) keep a
    // deterministic AU-then-VST3 order across scans.
    std::stable_sort(collected.begin(), collected.end(),
                     [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
                         return a.name.compareIgnoreCase(b.name) < 0;
                     });

    {
        std::lock_guard<std::mutex> lk(pluginsMutex_);
        entries_ = collected;
    }
    computeSupersessions();   // mark older-version duplicates now the rows are final

    // Persist the FULL entries list so the other host (main plugin / Link)
    // resolves against the same list without running its own scan. The
    // write is CHECKED and stamped (13 Aug 2026): this file's mtime is the
    // only completion signal the scan has, and its date going stale is how
    // a July snapshot served five weeks of sessions unnoticed.
    {
        const juce::int64 nowMs = juce::Time::currentTimeMillis();
        auto root = std::make_unique<juce::XmlElement>("CHAIN_ENTRIES");
        root->setAttribute("scannedAt", juce::String(nowMs));
        for (auto& d : collected)
            if (auto x = d.createXml())
                root->addChildElement(x.release());
        appSupportDir().createDirectory();
        auto ecFile = getEntriesCacheFile();
        if (root->writeTo(ecFile))
        {
            entriesCacheTime_   = ecFile.getLastModificationTime();
            entriesScannedAtMs_ = nowMs;
            EchoJay_NSLog(("EJScan: cache written, " + juce::String(collected.size())
                           + " entr(ies) (" + juce::String(auEntries.size()) + " AU + "
                           + juce::String(vst3Entries.size()) + " VST3), "
                           + juce::String((int) ecFile.getSize())
                           + "b -> " + ecFile.getFullPathName()).toRawUTF8());
        }
        else
        {
            // The in-memory list is still fresh (entries_ was replaced
            // above), so this session works; the OTHER host and the next
            // session keep reading the old file. Say so, loudly.
            entriesScannedAtMs_ = nowMs;
            EchoJay_NSLog(("EJScan: CACHE WRITE FAILED -> " + ecFile.getFullPathName()
                           + " -- this session's list is fresh but the file "
                             "still holds the OLD scan; the Link host and the "
                             "next session will read stale entries.").toRawUTF8());
        }
    }
    EchoJay_NSLog(("EJScan: scan complete, " + juce::String(collected.size())
                   + " entr(ies) in " + juce::String((juce::Time::currentTimeMillis()
                                                      - scanStartedAtMs_) / 1000) + "s").toRawUTF8());

    scanProgress_.store(1.0f);
    setScanStatus({});

    // NO in-DAW fingerprint pass here. Bulk fingerprinting is the opt-in
    // post-install background mapper's job (tools/ejextract --bootstrap,
    // installed as a launchd LaunchAgent): one plugin per PROCESS, outside
    // any DAW. Its results arrive via mergeBootstrapMaps(). In-DAW the only
    // instantiations are real slot loads (completeLoad fingerprints those),
    // and the lazy per-slot map fetch covers them immediately.
}

// ---------------------------------------------------------------------------
// Plugin list queries
// ---------------------------------------------------------------------------

// AU-preferring name collapse for the EMPTY format filter (AAX/Standalone
// wrapper "show all", and the empty-filter resolveByName fallbacks). The
// entries cache keeps BOTH formats of a name since 15 Aug 2026 (see the
// collection comment in the scan), so the dedupe that used to run at scan
// time now runs here, at presentation time, with the same comparison, the
// same lowercasing and the same AU-first-wins semantics: a VST3 row is
// skipped when any AU shares its lowercased name. Empty-filter callers
// therefore see a list identical to the pre-relocation one.
static // Collect .vst3 bundles under a directory, RECURSIVELY (18 Aug 2026). The old
// scan used findChildFiles(..., false, "*.vst3"), top level only, so 412
// bundles inside vendor subfolders (Universal Audio, MeldaProduction,
// Kilohearts, Soundtoys, Slate Digital) were never enumerated: 448 -> 860
// here. A .vst3 is itself a directory, so this treats one as a LEAF and never
// descends into it (a bundle's Contents never holds another plugin). Bounded
// depth guards a pathological tree; measured nesting is 2.
static void collectVst3BundlesRecursively(const juce::File& dir,
                                          juce::Array<juce::File>& out,
                                          int depthLeft)
{
    if (depthLeft < 0 || ! dir.isDirectory()) return;
    for (auto& child : dir.findChildFiles(juce::File::findDirectories | juce::File::findFiles, false))
    {
        if (child.getFileName().endsWithIgnoreCase(".vst3"))
            out.add(child);                                   // a bundle: a leaf, do not descend
        else if (child.isDirectory())
            collectVst3BundlesRecursively(child, out, depthLeft - 1);
    }
}

juce::Array<juce::PluginDescription>
collapseAuPreferring(const juce::Array<juce::PluginDescription>& rows)
{
    std::unordered_set<std::string> auNames;
    for (const auto& d : rows)
        if (d.pluginFormatName == "AudioUnit")
            auNames.insert(d.name.toLowerCase().toStdString());

    juce::Array<juce::PluginDescription> out;
    out.ensureStorageAllocated(rows.size());
    for (const auto& d : rows)
        if (d.pluginFormatName != "VST3"
            || auNames.find(d.name.toLowerCase().toStdString()) == auNames.end())
            out.add(d);
    return out;
}

int ChainHost::getNumPlugins() const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    return entries_.size();
}

juce::Array<juce::PluginDescription> ChainHost::getFilteredPlugins(
    const juce::String& filter,
    const juce::String& formatFilter,
    bool collapseTwins) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    juce::Array<juce::PluginDescription> result;
    juce::String lf = filter.toLowerCase();

    // Built-ins are pinned to the top and are NOT subject to the format filter:
    // they are compiled into the plugin, so they are equally available whether
    // this instance is running as an AU or a VST3. They still honour the search
    // text so typing narrows the list as expected.
    //
    // Generated from the registry, already ordered by category then name — so a
    // new device appears in the add-menu, in its category group, with no edit
    // here (BUILTIN_SUITE_PLAN.md §1).
    for (const auto& d : BuiltinDeviceRegistry::instance().descriptions())
    {
        if (lf.isEmpty()
            || d.name.toLowerCase().contains(lf)
            || d.manufacturerName.toLowerCase().contains(lf)
            || d.category.toLowerCase().contains(lf))
            result.add(d);
    }

    juce::Array<juce::PluginDescription> collapsed;
    if (formatFilter.isEmpty() && collapseTwins)
        collapsed = collapseAuPreferring(entries_);
    const auto& rows = (formatFilter.isEmpty() && collapseTwins) ? collapsed : entries_;

    for (auto& d : rows)
    {
        if (formatFilter.isNotEmpty() && d.pluginFormatName != formatFilter) continue;
        // Crash-blacklisted and architecture-incompatible rows are hidden
        // from the browser too, not only refused at load: entries_ can
        // still carry them between the blacklist growing and the next
        // rescan, and the arch answer belongs to this process (see
        // WithholdReason in the header). One function decides, here and
        // at the other two feed sites. pluginsMutex_ is already held.
        if (isWithheld(withholdReasonLocked(d))) continue;
        if (lf.isEmpty()
            || d.name.toLowerCase().contains(lf)
            || d.manufacturerName.toLowerCase().contains(lf))
            result.add(d);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Chain slot management
// ---------------------------------------------------------------------------

int ChainHost::getNumSlots() const noexcept
{
    return (int)slots_.size();
}

std::vector<ChainHost::SlotInfo> ChainHost::getAllSlotInfos() const
{
    // ONE projection builder (2 Sep 2026): this used to brace-initialise
    // five fields itself, so when SlotInfo gained `manufacturer` the sixth
    // field silently defaulted to "" here while getSlotInfo carried it —
    // the main's panel AND the sidecar publish both feed from THIS
    // accessor, so the main never floated Waves and remote racks published
    // blank identities, while the Link (getSlotDescription, same member)
    // worked. Positional-init trap, fourth visit. Delegating means the
    // two projections can never diverge again.
    std::vector<SlotInfo> result;
    result.reserve(slots_.size());
    for (int i = 0; i < (int) slots_.size(); ++i)
        result.push_back(getSlotInfo(i));
    return result;
}

ChainHost::SlotInfo ChainHost::getSlotInfo(int i) const
{
    // ASSIGNED BY NAME, not by position (1 Sep 2026, resolving the merge that
    // grew SlotInfo on BOTH sides at once). The note above getAllSlotInfos
    // calls positional init a trap on its "fourth visit"; a merge in which two
    // branches each append a field is precisely the fifth. Named assignment
    // means a future field cannot silently land in the wrong slot, and the
    // out-of-range return states what it returns instead of counting braces.
    SlotInfo info;
    info.bypassed = false;
    if (i < 0 || i >= (int)slots_.size()) return info;
    const auto& s = slots_[(size_t)i];
    info.name             = s.desc.name;
    info.bypassed         = s.bypassed;
    info.settings         = s.settings;
    info.format           = s.desc.pluginFormatName;
    info.wet              = s.wet;
    info.manufacturer     = s.desc.manufacturerName;   // remote, 27 Aug
    info.settingsForModel = modelSettingsForSlot(i);   // local, 24 Aug
    info.hasLiveReads     = slotHasLiveReads(i);       // local, 24 Aug
    return info;
}

ChainHost::SlotIdentity ChainHost::getSlotIdentity(int slot) const
{
    SlotIdentity id;
    if (slot < 0 || slot >= (int) slots_.size()) return id;
    const auto& s = slots_[(size_t) slot];
    id.fp = s.fp;
    if (s.desc.uniqueId != 0) id.uid = juce::String::toHexString(s.desc.uniqueId);
    id.version = s.desc.version;
    return id;
}

void ChainHost::setSlotSettings(int i, const juce::String& settings)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    slots_[i].settings = settings;
    // New prose for this slot: any dial echo it had describes an older
    // request. Clear it rather than let it outlive its map (see the field).
    clearModelTiers(slots_[i]);
}

// ---------------------------------------------------------------------------
// Structural edit operations (Phase 1c) — parse, describe, apply
// ---------------------------------------------------------------------------
std::vector<ChainHost::ChainEditOp> ChainHost::parseChainEditOps(
    const juce::String& editJson,
    juce::StringArray* baseSlotsOut,
    juce::String* explanationOut)
{
    std::vector<ChainEditOp> out;
    auto v = juce::JSON::parse(editJson);
    auto* o = v.getDynamicObject();
    if (o == nullptr) return out;
    if (baseSlotsOut != nullptr)
        if (auto* bs = o->getProperty("baseSlots").getArray())
            for (auto& bv : *bs) baseSlotsOut->add(bv.toString().trim());
    if (explanationOut != nullptr)
        *explanationOut = o->getProperty("explanation").toString().trim();
    auto* arr = o->getProperty("edit").getArray();
    if (arr == nullptr) return out;
    for (auto& ev : *arr)
    {
        auto* eo = ev.getDynamicObject();
        if (eo == nullptr) continue;
        ChainEditOp op;
        op.op       = eo->getProperty("op").toString().trim().toLowerCase();
        // ===== THE 1-based -> 0-based BOUNDARY (the only one) =====
        // The model and the user count slots from 1 (the [CURRENT CHAIN]
        // injection numbers them 1-based); ChainHost counts from 0. This
        // parse is the single conversion point — every consumer downstream
        // (pre-flight dry-run, staleness checks, the original->current map,
        // the sequencer) sees internal 0-based indices, and display-side
        // code (describeEditOp, result strings, the web card) converts back
        // to 1-based only when printing. "after": 0 means insert FIRST and
        // maps to the internal -1 convention.
        op.slot     = eo->hasProperty("slot")  ? (int)eo->getProperty("slot")  - 1 : -1;
        op.to       = eo->hasProperty("to")    ? (int)eo->getProperty("to")    - 1 : -1;
        op.after    = eo->hasProperty("after") ? juce::jmax(-1, (int)eo->getProperty("after") - 1) : -2;
        op.on       = (bool)eo->getProperty("on");
        op.name     = eo->getProperty("name").toString().trim();
        op.settings = eo->getProperty("settings").toString().trim();
        // OP TARGETS v1: the op's own words for what it is aiming at. Read
        // unconditionally, like every other field here; absent keys leave
        // them empty and the op then behaves exactly as it did before the
        // fields existed, which is what lets an older server keep working.
        op.slotName  = eo->getProperty("slot_name").toString().trim();
        op.afterName = eo->getProperty("after_name").toString().trim();
        // Machine-readable dial values on add/replace: the server sends the
        // same settings_structured object a chain entry carries, so this is the
        // same key the build path reads. Only add/replace consume it (they are
        // the only ops that produce a slot to dial); reading it unconditionally
        // costs nothing and keeps this parse a straight field copy. Read as a
        // raw var — setSlotStructuredSettings ignores a void/non-object, so ops
        // without it behave exactly as before.
        op.structuredSettings = eo->getProperty("settings_structured");
        // Slot wet/dry from the model: "wet_pct" 0..100. Numbers clamp;
        // anything else is absent (the knob is left alone) and logged, so
        // a wrong type never silently reads as 0% or 100%.
        if (eo->hasProperty("wet_pct"))
        {
            const auto wv = eo->getProperty("wet_pct");
            if (wv.isDouble() || wv.isInt() || wv.isInt64())
                op.wetPct = juce::jlimit(0.0f, 100.0f, (float)(double) wv);
            else
                EchoJay_NSLog(("EJEdit: wet_pct on op \"" + op.op + "\" is not a number ("
                               + wv.toString() + "); ignored").toRawUTF8());
        }
        if (auto* nsObj = eo->getProperty("no_such").getDynamicObject())
        {
            op.noSuchTerm = nsObj->getProperty("term").toString();
            op.noSuchTier = nsObj->getProperty("tier").toString();
        }
        if (op.op.isNotEmpty()) out.push_back(std::move(op));
    }
    return out;
}

juce::String ChainHost::describeEditOp(const ChainEditOp& op,
                                       const juce::StringArray& baseSlots)
{
    // Display is 1-BASED (matches the [CURRENT CHAIN] injection and the
    // model's numbering); op fields are internal 0-based post-parse.
    //
    // OP TARGETS v1 (4 Sep 2026): THE WORDS COME FROM THE OP WHERE THE OP HAS
    // WORDS. Every line here used to render slotName(op.slot), a lookup of the
    // op's own NUMBER in baseSlots, so the card could not contradict the op:
    // it WAS the op's number read back through a table. A user reading
    // "replace FG-X 2 (slot 3)" was reading the client's opinion of slot 3,
    // not the model's claim about what it was removing. On 4 September that
    // opinion was correct and the action was still wrong, and Apply was
    // tapped on it. The card is where consent is given, so it has to show the
    // claim being consented to. The rack lookup remains the fallback for an
    // op that carries no name, which is every op from a pre-v1 server.
    auto rackAt = [&baseSlots](int i) -> juce::String {
        return (i >= 0 && i < baseSlots.size())
            ? baseSlots[i] : ("slot " + juce::String(i + 1));
    };
    auto targetWords = [&rackAt](const juce::String& fromOp, int i) -> juce::String {
        return fromOp.trim().isNotEmpty() ? fromOp.trim() : rackAt(i);
    };
    auto slotName = [&targetWords, &op](int i) -> juce::String {
        return targetWords(op.slotName, i);
    };
    // What a dial will ACTUALLY write, rendered from the STRUCTURED payload
    // (9 Aug 2026): the card showed the op's PROSE ("ratio 4") while the
    // payload dialled XT Mode 1 - Apply is consent, and it was consenting
    // to a different action than the one performed. Controls print by
    // exact name and value, flat semantics as key value, bands as a count.
    auto structuredSummary = [](const juce::var& ss) -> juce::String {
        auto* o = ss.getDynamicObject();
        if (o == nullptr) return {};
        // Display rounding: float32-crossed values must not print as
        // "0.050000000745058" on the consent card.
        auto fmtVal = [](const juce::var& v) -> juce::String {
            return v.isDouble()
                ? juce::String ((double) v, 4).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                : v.toString();
        };
        juce::StringArray parts;
        if (auto* co = o->getProperty("controls").getDynamicObject())
            for (const auto& kv : co->getProperties())
                parts.add(kv.name.toString() + " " + fmtVal(kv.value));
        for (const auto& kv : o->getProperties())
        {
            const auto k = kv.name.toString();
            if (k == "controls" || k == "bands" || k == "dropped_controls") continue;
            parts.add(k + " " + fmtVal(kv.value));
        }
        if (auto* ba = o->getProperty("bands").getArray())
            parts.add(juce::String(ba->size()) + (ba->size() == 1 ? " band move" : " band moves"));
        return parts.joinIntoString(", ");
    };
    const juce::String payload = structuredSummary(op.structuredSettings);
    if (op.op == "add")
        // The anchor's words come from after_name where the op carries it.
        // "after slot 2 (Newfangled Elevate)" was the whole of failure B: the
        // name in the parentheses was baseSlots[2], so it agreed with the
        // number by construction and could not report that the model meant a
        // different plugin. An op naming its anchor prints the anchor.
        return juce::String::fromUTF8("+ add ") + op.name
             + (op.after <= -1 ? juce::String(" first")
                : (op.after >= baseSlots.size() && op.afterName.trim().isEmpty())
                    ? juce::String(" at the end of the chain")
                    : " after " + targetWords(op.afterName, op.after)
                      + " (slot " + juce::String(op.after + 1) + ")")
             + (payload.isNotEmpty() ? " - sets " + payload : juce::String());
    if (op.op == "remove")
        return juce::String::fromUTF8("\xe2\x88\x92 remove ") + slotName(op.slot)
             + " (slot " + juce::String(op.slot + 1) + ")";
    if (op.op == "replace")
        return juce::String::fromUTF8("\xe2\x87\x84 replace ") + slotName(op.slot)
             + " (slot " + juce::String(op.slot + 1) + ") with " + op.name
             + (payload.isNotEmpty() ? " - sets " + payload : juce::String());
    if (op.op == "move")
        return juce::String::fromUTF8("\xe2\x86\x95 move ") + slotName(op.slot)
             + " (slot " + juce::String(op.slot + 1) + ") to position " + juce::String(op.to + 1);
    if (op.op == "bypass")
        return juce::String(op.on ? "\xe2\x8f\xbb bypass " : "\xe2\x8f\xbb un-bypass ")
             + slotName(op.slot) + " (slot " + juce::String(op.slot + 1) + ")";
    if (op.op == "set_wet")
        return juce::String::fromUTF8("\xe2\x97\x90 set wet ")
             + juce::String(juce::roundToInt(juce::jmax(0.0f, op.wetPct))) + "% on "
             + slotName(op.slot) + " (slot " + juce::String(op.slot + 1) + ")";
    if (op.op == "set")
    {
        // A set op without structured settings dials NOTHING - it puts the
        // prose values on the card for hand-dialing. Its line must say so
        // (9 Aug 2026: "dial X: ratio 4" over a write that never happened
        // was one of three surfaces contradicting the honest bubble).
        // A DIAL line renders the STRUCTURED payload, never the prose: the
        // payload is what Apply will write.
        const bool dials = op.structuredSettings.getDynamicObject() != nullptr;
        const juce::String detail = dials ? payload : op.settings;
        // The WHY rides the line (9 Aug 2026): three of five honest turns
        // said "dial by hand" with no reason, which reads as arbitrary.
        // The reason is the server's tiered decision on the op, composed
        // here like every other honest surface - the model's prose is
        // optional garnish on top.
        juce::String why;
        if (!dials && op.noSuchTerm.isNotEmpty())
        {
            if (op.noSuchTier == "deferred")
                why = " - \"" + op.noSuchTerm + "\" is not yet mapped: set it by hand on the plugin";
            else if (op.noSuchTier == "complete")
                why = " - \"" + op.noSuchTerm + "\" is not exposed for remote control: set it on the plugin face";
            else
                why = " - \"" + op.noSuchTerm + "\" is not in this plugin's map: set it by hand";
        }
        return juce::String::fromUTF8(dials ? "\xe2\x9a\x99 dial " : "\xe2\x9c\x8e suggest for ")
             + slotName(op.slot) + " (slot " + juce::String(op.slot + 1) + ")"
             + (detail.isNotEmpty() ? ": " + detail : juce::String()) + why;
    }
    return "? unknown op: " + op.op;
}

// Sequencer state — shared_ptr-threaded through the async load callbacks,
// exactly the restoreNextSlot / buildChainFromSpec pattern.
namespace {
struct EditSeqState
{
    std::vector<ChainHost::ChainEditOp> ops;
    size_t idx = 0;
    std::vector<int> map;            // original slot index -> current index
    int applied = 0;
    juce::StringArray results;
    std::function<void(const juce::StringArray&, int, bool)> onDone;
    std::function<void(const juce::String&)> onProgress;   // 1d stage labels
};
} // namespace

void ChainHost::applyChainEdits(std::vector<ChainEditOp> ops,
                                int expectedRevision,
                                const juce::StringArray& baseSlots,
                                std::function<void(const juce::StringArray&,
                                                   int, bool)> onDone,
                                std::function<void(const juce::String&)> onProgress)
{
    auto abort = [&onDone](const juce::String& why)
    { if (onDone) onDone(juce::StringArray{ why }, 0, true); };

    // ---- Pre-flight guard 1: revision (same-session staleness) ----
    // EACH guard names what it actually compared, in the log AND in the
    // user-visible message (15 Aug 2026). The three used to share one
    // string, so a baseSlots mismatch reported itself as "chain changed" -
    // a cause that guard never checked, and in the 14 Aug live incident a
    // false one: the rack had NOT changed since the proposal, the proposal
    // was written for a chain that never loaded. A message asserting an
    // unchecked cause is worse than a vague one, because it gets believed.
    // ASCII punctuation only in these strings: they travel as char*
    // literals into juce::String, which reads bytes, not UTF-8 (the em
    // dash drew as mojibake).
    if (expectedRevision >= 0 && expectedRevision != getChainRevision())
    {
        EchoJay_NSLog(("EJEdit: preflight REFUSED guard=revision expected="
                       + juce::String(expectedRevision) + " live="
                       + juce::String(getChainRevision())).toRawUTF8());
        return abort("the rack was modified after this edit was proposed"
                     " - ask again");
    }

    // ---- Pre-flight guard 2: baseSlots vs live rack ----
    const int n = getNumSlots();
    if (baseSlots.size() != n)
    {
        EchoJay_NSLog(("EJEdit: preflight REFUSED guard=baseSlots-count base="
                       + juce::String(baseSlots.size()) + " [" + baseSlots.joinIntoString(", ")
                       + "] live=" + juce::String(n)).toRawUTF8());
        return abort("this edit was written for a "
                     + juce::String(baseSlots.size()) + "-slot chain, but the rack "
                     + (n == 0 ? juce::String("is empty")
                               : "has " + juce::String(n) + " slot"
                                 + (n == 1 ? "" : "s"))
                     + " - ask again");
    }
    for (int i = 0; i < n; ++i)
        if (!namesMatchLoose(baseSlots[i], slots_[(size_t)i].desc.name))
        {
            EchoJay_NSLog(("EJEdit: preflight REFUSED guard=baseSlots-name slot="
                           + juce::String(i) + " base=\"" + baseSlots[i] + "\" live=\""
                           + slots_[(size_t)i].desc.name + "\"").toRawUTF8());
            return abort("this edit expected \"" + baseSlots[i] + "\" at slot "
                         + juce::String(i + 1) + ", but the rack has \""
                         + slots_[(size_t)i].desc.name + "\" there - ask again");
        }
    EchoJay_NSLog(("EJEdit: staleness guards passed rev=" + juce::String(getChainRevision())
                   + " slots=" + juce::String(n)
                   + " ops=" + juce::String((int) ops.size())).toRawUTF8());

    if (ops.empty()) return abort("no operations in this edit");

    // ---- Pre-flight guard 3: dry-run every op against a simulated rack ----
    // After this, the only possible runtime failure is an async plugin load.
    {
        std::vector<bool> alive((size_t)n, true);
        int simCount = n;
        for (size_t k = 0; k < ops.size(); ++k)
        {
            const auto& op = ops[k];
            auto bad = [&](const juce::String& d) {
                return abort("op " + juce::String((int)k + 1) + " invalid: " + d);
            };
            // Slot numbers in error text are 1-BASED (the numbering the
            // model and user see); s itself is internal 0-based
            auto slotLabel = [](int s) { return "slot " + juce::String(s + 1); };
            // ONE author for the "why this name did not resolve" clause, so
            // the add and replace arms cannot drift. THREE outcomes, not two:
            // a name nothing has, a name this host withholds, and a name that
            // several DIFFERENT products answer to -- the last names all of
            // them, because a refusal the reader cannot act on is barely a
            // refusal.
            auto missText = [](WithholdReason why, const juce::StringArray& ambiguous)
            {
                if (why == WithholdReason::Ambiguous) return ambiguousNameText(ambiguous);
                if (why == WithholdReason::None)      return juce::String("not in the loadable plugin list");
                return withholdReasonText(why);
            };
            auto validSlot = [&](int s) {
                return s >= 0 && s < n && alive[(size_t)s];
            };
            if (op.op == "add")
            {
                if (op.name.isEmpty()) return bad("add without a plugin name");
                {
                    auto why = WithholdReason::None;
                    juce::StringArray ambiguous;
                    if (resolveOfferedName(op.name, &why, &ambiguous).name.isEmpty())
                        return bad("\"" + op.name + "\" " + missText(why, ambiguous));
                }
                // Positional target: out-of-range/removed "after" CLAMPS to
                // append-at-end (the runtime add path already does this) —
                // aborting the whole batch over a position was worse than an
                // append landing one slot off. Identity refs (slot on
                // remove/replace/bypass/move) still abort: clamping those
                // would mutate the WRONG plugin.
                ++simCount;
            }
            else if (op.op == "remove")
            {
                if (!validSlot(op.slot)) return bad(slotLabel(op.slot) + " does not exist");
                alive[(size_t)op.slot] = false;
                --simCount;
                if (simCount < 0) return bad("removes more slots than exist");
            }
            else if (op.op == "replace")
            {
                if (!validSlot(op.slot)) return bad(slotLabel(op.slot) + " does not exist");
                if (op.name.isEmpty()) return bad("replace without a plugin name");
                {
                    auto why = WithholdReason::None;
                    juce::StringArray ambiguous;
                    if (resolveOfferedName(op.name, &why, &ambiguous).name.isEmpty())
                        return bad("\"" + op.name + "\" " + missText(why, ambiguous));
                }
            }
            else if (op.op == "move")
            {
                if (!validSlot(op.slot)) return bad(slotLabel(op.slot) + " does not exist");
                // move.to is positional: clamps at runtime, never aborts
            }
            else if (op.op == "bypass")
            {
                if (!validSlot(op.slot)) return bad(slotLabel(op.slot) + " does not exist");
            }
            else if (op.op == "set")
            {
                // Settings-only op (1 Aug 2026): touches parameters on an
                // EXISTING slot, never the instance. Born from the replace
                // footgun: a settings request on a racked plugin had no op
                // that was not destructive.
                if (!validSlot(op.slot)) return bad(slotLabel(op.slot) + " does not exist");
                if (op.settings.isEmpty() && op.structuredSettings.getDynamicObject() == nullptr)
                    return bad("set without any settings");
            }
            else if (op.op == "set_wet")
            {
                // Slot wet/dry only (16 Aug 2026): EchoJay's own blend on an
                // EXISTING slot, never a plugin parameter, never the instance.
                if (!validSlot(op.slot)) return bad(slotLabel(op.slot) + " does not exist");
                if (op.wetPct < 0.0f) return bad("set_wet without a wet_pct (0 to 100)");
            }
            else return bad("unknown operation \"" + op.op + "\"");
        }
    }

    auto st = std::make_shared<EditSeqState>();
    st->ops = std::move(ops);
    st->onDone = std::move(onDone);
    st->onProgress = std::move(onProgress);
    st->map.resize((size_t)n);
    for (int i = 0; i < n; ++i) st->map[(size_t)i] = i;
    runNextEditOp(st);
}

// Walk a slot from its current index to a target index via the existing
// single-step moveSlot swaps, keeping the original->current map true after
// every swap. Message thread; synchronous.
void ChainHost::walkSlotTo(std::vector<int>& map, int fromCur, int toCur)
{
    while (fromCur != toCur)
    {
        const int step = toCur > fromCur ? 1 : -1;
        const int other = fromCur + step;
        // The map entry (if any) pointing at `other` swaps with `fromCur`
        for (auto& m : map)
            if (m == other) { m = fromCur; break; }
        moveSlot(fromCur, step);
        fromCur = other;
    }
}

void ChainHost::runNextEditOp(std::shared_ptr<void> stateErased)
{
    auto st = std::static_pointer_cast<EditSeqState>(stateErased);
    if (st->idx >= st->ops.size())
    {
        if (st->onDone) st->onDone(st->results, st->applied, false);
        return;
    }
    const auto op = st->ops[st->idx];

    // 1d working-state label: present-tense attempt, fired before execution
    if (st->onProgress)
    {
        juce::String label;
        if (op.op == "remove")
        {
            const int cur = (op.slot >= 0 && op.slot < (int)st->map.size())
                              ? st->map[(size_t)op.slot] : -1;
            label = "Removing "
                  + (cur >= 0 && cur < (int)slots_.size()
                       ? slots_[(size_t)cur].desc.name : juce::String("a slot"))
                  + "...";
        }
        else if (op.op == "add" || op.op == "replace")
            label = "Loading " + op.name + "...";
        else if (op.op == "move")
            label = "Reordering the chain...";
        else if (op.op == "bypass")
            label = op.on ? "Bypassing a slot..." : "Un-bypassing a slot...";
        else if (op.op == "set_wet")
            label = "Setting wet/dry...";
        else if (op.op == "set")
        {
            const int cur = (op.slot >= 0 && op.slot < (int)st->map.size())
                              ? st->map[(size_t)op.slot] : -1;
            label = "Dialling "
                  + (cur >= 0 && cur < (int)slots_.size()
                       ? slots_[(size_t)cur].desc.name : juce::String("a slot"))
                  + "...";
        }
        if (label.isNotEmpty()) st->onProgress(label);
    }

    auto finishOpAndContinue = [this, st](const juce::String& line)
    {
        st->results.add(line);
        ++st->applied;
        ++st->idx;
        // Let the message loop breathe between ops (paints, host callbacks)
        auto self = st;
        juce::Timer::callAfterDelay(30, [this, self] { runNextEditOp(self); });
    };
    // Expected runtime failure (a plugin load): the op was a clean no-op
    // (load-before-destroy), the rack and the original->current map are
    // exactly as if the op never existed — so INDEPENDENT later ops stay
    // valid and we continue. Only invariant violations (a map entry that
    // pre-flight should have made impossible) still hard-stop: if the map
    // is wrong, continuing is what would be unsafe.
    auto failButContinue = [this, st](const juce::String& line)
    {
        st->results.add(line);
        ++st->idx;
        auto self = st;
        juce::Timer::callAfterDelay(30, [this, self] { runNextEditOp(self); });
    };
    auto failAndStop = [st](const juce::String& line)
    {
        st->results.add(line);
        for (size_t r = st->idx + 1; r < st->ops.size(); ++r)
            st->results.add("not attempted");
        if (st->onDone) st->onDone(st->results, st->applied, false);
    };

    // Defensive current-index lookup (pre-flight guarantees validity, but a
    // stale map entry must fail the op loudly, never index out of range)
    auto curOf = [&st](int orig) -> int {
        return (orig >= 0 && orig < (int)st->map.size()) ? st->map[(size_t)orig] : -1;
    };

    // OP TARGETS v1: does the plugin the op NAMED sit where the op's number
    // points? Empty means proceed (including the pre-v1 case where the op
    // named nothing at all). Checked in the SEQUENCER and not in the
    // pre-flight dry run, deliberately: the dry run's verb is abort-the-batch,
    // and a wrong target on one op is no reason to throw away the others.
    auto targetRefusal = [this, &op](int cur) -> juce::String {
        if (cur < 0 || cur >= (int) slots_.size()) return {};
        return identityTargetMismatch(op.slotName, slots_[(size_t)cur].desc.name, op.slot);
    };

    if (op.op == "remove")
    {
        const int cur = curOf(op.slot);
        if (cur < 0) return failAndStop("remove failed: slot no longer present");
        {
            const auto why = targetRefusal(cur);
            if (why.isNotEmpty()) return failButContinue("remove refused: " + why);
        }
        const juce::String nm = slots_[(size_t)cur].desc.name;
        removeSlot(cur);
        for (auto& m : st->map) { if (m == cur) m = -1; else if (m > cur) --m; }
        finishOpAndContinue("removed " + nm);
        return;
    }
    if (op.op == "bypass")
    {
        const int cur = curOf(op.slot);
        if (cur < 0) return failAndStop("bypass failed: slot no longer present");
        {
            const auto why = targetRefusal(cur);
            if (why.isNotEmpty()) return failButContinue("bypass refused: " + why);
        }
        setSlotBypassed(cur, op.on);
        finishOpAndContinue(juce::String(op.on ? "bypassed " : "un-bypassed ")
                            + slots_[(size_t)cur].desc.name);
        return;
    }
    if (op.op == "set_wet")
    {
        const int cur = curOf(op.slot);
        if (cur < 0) return failAndStop("set_wet failed: slot no longer present");
        {
            const auto why = targetRefusal(cur);
            if (why.isNotEmpty()) return failButContinue("set_wet refused: " + why);
        }
        setSlotWet(cur, op.wetPct / 100.0f);
        EchoJay_NSLog(("EJEdit: set_wet slot=" + juce::String(cur + 1) + " \""
                       + slots_[(size_t)cur].desc.name + "\" wet=" + juce::String(op.wetPct, 1) + "%").toRawUTF8());
        finishOpAndContinue("set wet " + juce::String(juce::roundToInt(op.wetPct)) + "% on "
                            + slots_[(size_t)cur].desc.name);
        return;
    }
    if (op.op == "move")
    {
        const int cur = curOf(op.slot);
        if (cur < 0) return failAndStop("move failed: slot no longer present");
        {
            const auto why = targetRefusal(cur);
            if (why.isNotEmpty()) return failButContinue("move refused: " + why);
        }
        // Target: current position of the original occupant of `to`, else
        // clamp into the current rack
        int target = curOf(op.to);
        if (target < 0) target = juce::jlimit(0, getNumSlots() - 1, op.to);
        const juce::String nm = slots_[(size_t)cur].desc.name;
        walkSlotTo(st->map, cur, target);
        // walkSlotTo fixed every displaced entry; the moved original now
        // lives at target
        if (op.slot >= 0 && op.slot < (int)st->map.size())
            st->map[(size_t)op.slot] = target;
        finishOpAndContinue("moved " + nm + " to position " + juce::String(target + 1));
        return;
    }
    if (op.op == "set")
    {
        // Settings-only: parameters change through the ONE map-gated apply
        // pipeline; the instance, its position and all its other state are
        // untouched. This is what a "change the settings" request routes to
        // instead of the destructive replace.
        const int cur = curOf(op.slot);
        if (cur < 0) return failAndStop("set failed: slot no longer present");
        {
            const auto why = targetRefusal(cur);
            if (why.isNotEmpty()) return failButContinue("set refused: " + why);
        }
        const juce::String nm = slots_[(size_t)cur].desc.name;
        const bool hasPayload = op.structuredSettings.getDynamicObject() != nullptr;
        // NARROW FIX (5 Sep 2026), and its narrowness is deliberate. This line
        // has NEVER consulted the write outcome: setSlotStructuredSettings
        // below STORES the payload and defers, and applyStructuredIfReady can
        // return without writing on pending, noMap, mapIdentityMismatch,
        // mapNoCoverage and builtinPayloadUnmatched, none of which have
        // happened yet when this runs. So a first-encounter plugin with no
        // cached map has always reported "dialled" having written nothing.
        // That is a real defect with a wider blast radius than this feature
        // (it needs the op's result deferred until the dial settles, which is
        // the sequencer's result contract) and it is filed as its own item.
        //
        // What IS knowable here, synchronously and with certainty, is the
        // mode: when writes are blocked the guard is unconditional, so no
        // write will happen and this line must not say one did.
        const bool dials = hasPayload && ! echojay::dialWritesBlocked();
        if (op.settings.isNotEmpty())
            setSlotSettings(cur, op.settings);
        if (hasPayload)
            setSlotStructuredSettings(cur, op.structuredSettings);   // fills the card either way
        // The result line is what every downstream summary reads: a
        // prose-only set "dialled" nothing and must not say it did.
        finishOpAndContinue(hasPayload && ! dials
                                ? "settings on the card for " + nm
                                    + " (not dialled: dialling is off in Settings)"
                                : (dials ? "dialled " : "suggested settings for ") + nm);
        return;
    }
    if (op.op == "add" || op.op == "replace")
    {
        // Fallible work FIRST: load (appends at the end). Only on success do
        // we touch existing slots, so a failed load is a clean no-op and the
        // sequence CONTINUES with the remaining independent ops.
        // Honest miss (WithholdReason): a plugin this host withholds is
        // reported as such, not as "not resolvable".
        // OP TARGETS v1: the target is settled BEFORE the load, so a refused
        // op never leaves a plugin in the rack that nothing asked for. Both
        // arms are pure reads at this point; nothing has moved yet.
        int  addInsertAt = -1;
        bool addFromName = false;
        if (op.op == "add")
        {
            juce::StringArray rackNames;
            for (const auto& sl : slots_) rackNames.add(sl.desc.name);
            // -1 = "insert first", -2 = the index did not resolve (the slot it
            // named was removed earlier in this batch, or is past the end).
            const int anchorCur = op.after <= -1
                ? -1
                : (op.after < (int)st->map.size() && st->map[(size_t)op.after] >= 0
                       ? st->map[(size_t)op.after] : -2);
            const auto anchor = resolveAddAnchor(op.afterName, anchorCur, rackNames);
            if (anchor.refused) return failButContinue("add refused: " + anchor.why);
            addInsertAt = anchor.insertAt;
            addFromName = anchor.fromName;
        }
        else
        {
            const int curTgt = curOf(op.slot);
            const auto whyT = targetRefusal(curTgt);
            if (whyT.isNotEmpty()) return failButContinue("replace refused: " + whyT);
        }

        WithholdReason why = WithholdReason::None;
        juce::StringArray ambiguous;
        // resolveOfferedName, not resolveByName: the SAME rule the pre-flight
        // dry run above used. A name that passes validation and then misses
        // here aborts a batch halfway, which is the one failure the dry run
        // exists to prevent.
        auto desc = resolveOfferedName(op.name, &why, &ambiguous);
        if (desc.name.isEmpty())
            return failButContinue(op.op + " failed: \"" + op.name + "\" "
                                   + (why == WithholdReason::Ambiguous
                                          ? ambiguousNameText(ambiguous)
                                      : why == WithholdReason::None
                                          ? juce::String("not resolvable")
                                          : withholdReasonText(why)));
        desc = preferInlineHostableDesc(desc);
        auto self = st;
        const auto theOp = op;
        loadPluginAsync(desc, LoadOrigin::Assistant,
                        [this, self, theOp, desc, addInsertAt, addFromName](const juce::String& err)
        {
            // Re-enter the sequencer context manually (we are mid-op)
            auto finish = [this, self](const juce::String& line)
            {
                self->results.add(line);
                ++self->applied;
                ++self->idx;
                auto keep = self;
                juce::Timer::callAfterDelay(30, [this, keep] { runNextEditOp(keep); });
            };
            auto failContinue = [this, self](const juce::String& line)
            {
                self->results.add(line);
                ++self->idx;
                auto keep = self;
                juce::Timer::callAfterDelay(30, [this, keep] { runNextEditOp(keep); });
            };
            auto failStop = [self](const juce::String& line)
            {
                self->results.add(line);
                for (size_t r = self->idx + 1; r < self->ops.size(); ++r)
                    self->results.add("not attempted");
                if (self->onDone) self->onDone(self->results, self->applied, false);
            };
            if (err.isNotEmpty())
                return failContinue(theOp.op + " failed: " + theOp.name
                                    + " would not load (" + err + ")");

            int newCur = getNumSlots() - 1;   // appended by completeLoad

            // Dial the placed slot through the ONE map-gated apply pipeline, the
            // same one loadChainFromJson uses on the build path. Until now the
            // edit path only wrote the prose display string (setSlotSettings)
            // and left every parameter to hand-dialing, so an add/replace that
            // carried settings_structured silently dropped it. Pending settings
            // live on the slot object and survive the walkSlotTo renumber below
            // (storeParamMaps re-scans all slots when the map arrives).
            // On replace this runs AFTER the same-plugin state restore below,
            // never before: settings dialled into an instance whose state is
            // about to be overwritten would be dialled into the void.
            //
            // It routes through applyStructuredIfReady, so a built-in device
            // gets its exact schema write and third-party slots take the anchor
            // path exactly as they already do: no new apply logic, just the same
            // payload reaching the same consumer. The index passed is the one
            // the slot occupies at call time; the slot struct carries the
            // pending settings with it through any later walkSlotTo shuffle.
            auto applyOpSettings = [this, &theOp](int slotIdx)
            {
                if (theOp.settings.isNotEmpty())
                    setSlotSettings(slotIdx, theOp.settings);
                if (theOp.structuredSettings.getDynamicObject() != nullptr)
                    setSlotStructuredSettings(slotIdx, theOp.structuredSettings);
                // wet_pct riding an add/replace: EchoJay's own blend on the
                // slot that just loaded; absent leaves the default (1.0).
                if (theOp.wetPct >= 0.0f)
                    setSlotWet(slotIdx, theOp.wetPct / 100.0f);
            };

            if (theOp.op == "add")
            {
                applyOpSettings(newCur);
                // Settled before the load by resolveAddAnchor. Computed on the
                // pre-load rack, which is still true here: the load APPENDS,
                // so every index below the new slot is unchanged.
                const int target = juce::jlimit(0, newCur, addInsertAt);
                walkSlotTo(self->map, newCur, target);
                // Entries at/after the insert point were fixed by walkSlotTo's
                // swap bookkeeping; nothing else to update (new slot is not
                // addressable by original numbering).
                //
                // The result line says WHICH anchor, not which number, when
                // the position came from the name: "after slot 2" is exactly
                // the sentence that was true and useless in failure B.
                finish("added " + theOp.name
                       + (theOp.after <= -1 ? juce::String(" first")
                          : addFromName     ? " after " + theOp.afterName
                                            : " after slot " + juce::String(theOp.after + 1)));
            }
            else // replace
            {
                const int oldCur = (theOp.slot >= 0 && theOp.slot < (int)self->map.size())
                                     ? self->map[(size_t)theOp.slot] : -1;
                if (oldCur < 0)
                    return failStop("replace failed: slot no longer present");
                const juce::String oldName = slots_[(size_t)oldCur].desc.name;

                // Same-plugin replace preserves state (1 Aug 2026, live
                // defect): a model asked to change settings on a racked
                // plugin sometimes reaches for replace-with-itself, and a
                // fresh default instance silently discarded everything the
                // user had dialled. When the replacement resolves to the
                // SAME plugin (same format too - state blobs do not cross
                // formats), the outgoing instance's full state is carried
                // into the new one, and the op's settings then apply on top
                // as deltas. A DIFFERENT plugin still starts from defaults.
                juce::MemoryBlock oldState;
                const bool samePlugin = desc.isDuplicateOf(slots_[(size_t)oldCur].desc);
                if (samePlugin)
                    if (auto* oldNode = slots_[(size_t)oldCur].node.get())
                        if (auto* oldProc = oldNode->getProcessor())
                            try { oldProc->getStateInformation(oldState); }
                            catch (...) { oldState.reset(); }

                removeSlot(oldCur);
                // MOVE LOG: one act, one entry. The load above and this
                // removal each recorded themselves; collapse them so the log
                // says what left and what arrived in the same line.
                collapseLastPairIntoSwap(theOp.slot, theOp.name, oldName);
                for (auto& m : self->map) { if (m == oldCur) m = -1; else if (m > oldCur) --m; }
                int fromCur = getNumSlots() - 1;   // new slot after the removal shift
                walkSlotTo(self->map, fromCur, oldCur);
                // The replacement now answers to the original slot number
                self->map[(size_t)theOp.slot] = oldCur;

                if (samePlugin && oldState.getSize() > 0)
                    if (auto* newNode = slots_[(size_t)oldCur].node.get())
                        if (auto* newProc = newNode->getProcessor())
                        {
                            const int mark = pushDeathMark("state restore", desc);
                            try { newProc->setStateInformation(oldState.getData(),
                                                               (int)oldState.getSize()); }
                            catch (...) {}
                            popDeathMark(mark);
                        }

                applyOpSettings(oldCur);
                finish(samePlugin
                       ? "updated " + theOp.name + " (settings preserved)"
                       : "replaced " + oldName + " with " + theOp.name);
            }
        });
        return;
    }
    failAndStop("unknown operation \"" + op.op + "\"");
}

// ---------------------------------------------------------------------------
// Wet/dry setters — knob-drag safe: pure value writes, never a graph rebuild
// ---------------------------------------------------------------------------
void ChainHost::setMasterWet(float wet01)
{
    masterWet_.store(juce::jlimit(0.0f, 1.0f, wet01), std::memory_order_relaxed);
    bumpChainRevision();
}

void ChainHost::setSlotWet(int i, float wet01, WetSource src)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    // ===== DO NOT DIAL (5 Sep 2026) =====
    // Not a plugin parameter, but it CHANGES THE SOUND, and the mode means
    // EchoJay does not change the sound. Guarded here rather than at the ops
    // that reach it (set_wet, and wet_pct riding an add or replace) so a
    // fourth caller cannot arrive without it.
    //
    // ONLY EchoJay's OWN writes. The user dragging the wet knob reaches this
    // same function, and blocking that would lock the user out of the hand
    // control the mode exists to hand back to them. A Restore is the session's
    // saved value and is not a change either.
    if (src == WetSource::Assistant && echojay::dialWritesBlocked()) return;
    auto& s = slots_[(size_t)i];
    s.wet = juce::jlimit(0.0f, 1.0f, wet01);
    bumpChainRevision();
    if (!s.wetShared)   // slot not rebuilt yet (e.g. restore) — value rides in s.wet
        s.wetShared = std::make_shared<std::atomic<float>>(s.wet);
    else
        s.wetShared->store(s.wet, std::memory_order_relaxed);
}

float ChainHost::getSlotWet(int i) const
{
    if (i < 0 || i >= (int)slots_.size()) return 1.0f;
    return slots_[(size_t)i].wet;
}

ChainHost::SlotLevels ChainHost::getSlotLevels(int i) const
{
    SlotLevels out;
    if (i < 0 || i >= (int)slots_.size()) return out;
    const auto& s = slots_[(size_t)i];
    if (s.bypassed || !s.blendNode) return out;   // not in circuit: not measured
    if (auto* b = dynamic_cast<SlotWetBlend*>(s.blendNode->getProcessor()))
    {
        out.in  = b->inTally().snapshot();
        out.out = b->outTally().snapshot();
        out.measured = true;
    }
    return out;
}

void ChainHost::resetAllLevels()
{
    chainInTally_.reset();
    chainOutTally_.reset();
    for (auto& s : slots_)
        if (s.blendNode)
            if (auto* b = dynamic_cast<SlotWetBlend*>(s.blendNode->getProcessor()))
                b->resetTallies();
    pendingSlotLevels_.clear();
    EchoJay_NSLog("EJLevels: all level tallies reset");
}

void ChainHost::setHostTrackName(const juce::String& nameIn)
{
    const juce::String name = nameIn.trim();
    const juce::String was  = hostTrackName_;
    hostTrackName_ = name;
    if (name.isEmpty()) return;
    // Two orderings of the same guard: the name changed under a live
    // instance (copied to another track, or the track renamed: a rename
    // costs one reset, which is the cheap direction), or the host named the
    // track AFTER a restore that carried another track's tally.
    if (was.isNotEmpty() && was != name)
    {
        EchoJay_NSLog(("EJLevels: host track name changed \"" + was + "\" -> \"" + name
                       + "\": level tallies reset").toRawUTF8());
        resetAllLevels();
    }
    else if (was.isEmpty() && restoredLevelsTrack_.isNotEmpty() && restoredLevelsTrack_ != name)
    {
        EchoJay_NSLog(("EJLevels: restored tally was measured on \"" + restoredLevelsTrack_
                       + "\", host now names this track \"" + name + "\": level tallies reset").toRawUTF8());
        resetAllLevels();
    }
    else if (was.isEmpty())
        EchoJay_NSLog(("EJLevels: host track name \"" + name + "\"").toRawUTF8());
    restoredLevelsTrack_.clear();
}

// ---------------------------------------------------------------------------
// Pre-chain gain (headroom + operating level)
// ---------------------------------------------------------------------------
void ChainHost::setPreGainDb(float db, bool userSet)
{
    const float g = juce::jlimit(kPreGainMinDb, kPreGainMaxDb, db);
    preGainDb_.store(g, std::memory_order_relaxed);
    if (userSet) { preGainUserSet_ = true; preGainState_ = PreGainState::UserSet; }
    else if (! preGainUserSet_)
        preGainState_ = (std::abs(g) > 1.0e-3f) ? PreGainState::Auto : PreGainState::Off;
    EchoJay_NSLog(("EJPreGain: set " + juce::String(g, 2) + " dB ("
                   + juce::String(userSet ? "by the user" : "auto") + ")").toRawUTF8());
}

void ChainHost::resetPreGainToAuto()
{
    // Clear the hand-set flag, then recompute the auto value NOW (target
    // minus the measured input) rather than zeroing: "reset to auto" means
    // the auto headroom, not 0. computePreGainAtBuild leaves it at 0 with
    // state NoLevel when no level is known, which is the honest unset.
    preGainUserSet_ = false;
    preGainDb_.store(0.0f, std::memory_order_relaxed);
    preGainState_ = PreGainState::Off;
    computePreGainAtBuild();   // sets Auto / Off / NoLevel from the current input
    EchoJay_NSLog("EJPreGain: reset to auto");
}

void ChainHost::computePreGainAtBuild()
{
    // The user's hand-set value is never overwritten by a build.
    if (preGainUserSet_)
    {
        EchoJay_NSLog(("EJPreGain: build kept the user's pre-gain "
                       + juce::String(preGainDb_.load(std::memory_order_relaxed), 2) + " dB").toRawUTF8());
        return;
    }
    const auto in = chainInTally_.snapshot();
    if (! in.known)
    {
        // No level known: not set, no guess (the same rule as everything on
        // the tally). Visible through the readout and the CHAIN LEVELS line.
        preGainState_ = PreGainState::NoLevel;
        EchoJay_NSLog("EJPreGain: build with no input level known, pre-gain not set");
        return;
    }
    const float g = juce::jlimit(kPreGainMinDb, kPreGainMaxDb, kPreGainTargetLufs - in.levelDb);
    preGainDb_.store(g, std::memory_order_relaxed);
    preGainState_ = (std::abs(g) > 1.0e-3f) ? PreGainState::Auto : PreGainState::Off;
    EchoJay_NSLog(("EJPreGain: build set " + juce::String(g, 2) + " dB to reach "
                   + juce::String(kPreGainTargetLufs, 0) + " LUFS from input "
                   + juce::String(in.levelDb, 2) + " LUFS (operating "
                   + juce::String(in.levelDb + g, 2) + ")").toRawUTF8());
}

float ChainHost::getOperatingLevelLufs() const
{
    const auto in = chainInTally_.snapshot();
    if (! in.known) return std::numeric_limits<float>::quiet_NaN();
    return in.levelDb + preGainDb_.load(std::memory_order_relaxed);
}

ChainHost::PreGainReadout ChainHost::getPreGain() const
{
    PreGainReadout r;
    r.db      = preGainDb_.load(std::memory_order_relaxed);
    r.state   = preGainState_;
    r.userSet = preGainUserSet_;
    switch (preGainState_)
    {
        case PreGainState::Off:     r.text = std::abs(r.db) > 1.0e-3f
                                        ? "Pre-gain: " + juce::String(r.db, 1) + " dB"
                                        : "Pre-gain: 0.0 dB"; break;
        case PreGainState::Auto:    r.text = "Pre-gain: " + juce::String(r.db, 1) + " dB (auto, to "
                                        + juce::String(kPreGainTargetLufs, 0) + " LUFS)"; break;
        case PreGainState::UserSet: r.text = "Pre-gain: " + juce::String(r.db, 1) + " dB (set by you)"; break;
        case PreGainState::NoLevel: r.text = "Pre-gain: not set (input level not known yet)"; break;
    }
    return r;
}

juce::var ChainHost::getLevelsStateVar(const juce::String& trackName) const
{
    auto* o = new juce::DynamicObject();
    o->setProperty("v", 1);
    o->setProperty("trackName", trackName);
    o->setProperty("in",  chainInTally_.toVar());
    o->setProperty("out", chainOutTally_.toVar());
    juce::Array<juce::var> arr;
    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        const auto& s = slots_[(size_t)i];
        auto* b = s.blendNode ? dynamic_cast<SlotWetBlend*>(s.blendNode->getProcessor()) : nullptr;
        if (b == nullptr) continue;   // never in circuit: nothing measured, nothing saved
        auto* so = new juce::DynamicObject();
        so->setProperty("n",   i + 1);   // the slot number chainSlotsXml writes it as
        so->setProperty("in",  b->inTally().toVar());
        so->setProperty("out", b->outTally().toVar());
        arr.add(juce::var(so));
    }
    o->setProperty("slots", arr);
    return juce::var(o);
}

void ChainHost::setPendingLevelsState(const juce::var& v, const juce::String& currentTrackName)
{
    pendingSlotLevels_.clear();
    auto* o = v.getDynamicObject();
    if (o == nullptr) return;
    const juce::String savedTrack = o->getProperty("trackName").toString().trim();
    const juce::String nowTrack   = currentTrackName.trim();
    // THE GUARD (load-bearing, not defensive): a level tally describes a
    // source. If this host names tracks and the names differ, the saved
    // tally is somebody else's channel (the plugin was copied) and starts
    // empty rather than inheriting a confidently wrong level. Names that
    // are both empty (a host that names no track) let it through; the
    // heard/window figures and the decay bound the damage there.
    if (savedTrack.isNotEmpty() && nowTrack.isNotEmpty() && savedTrack != nowTrack)
    {
        EchoJay_NSLog(("EJLevels: saved tally discarded, it was measured on track \"" + savedTrack
                       + "\" and this is \"" + nowTrack + "\"").toRawUTF8());
        return;
    }
    if (savedTrack.isEmpty() && nowTrack.isEmpty())
    {
        // Neither side can name the track, so nothing can say whether this
        // is the channel the tally was measured on: DISCARDED, not restored.
        // Bounded-wrong is still wrong, and a copied plugin inheriting a
        // level is the exact failure this instrument exists to prevent. The
        // cost is three seconds of playing after a reopen, which the feature
        // asks for anyway. An Ableton user reading "no level known" after a
        // reopen finds the reason here.
        EchoJay_NSLog("EJLevels: saved tally DISCARDED: neither the session nor the host names this "
                      "track (this host reports no track name), so the tally cannot be tied to a "
                      "source; it restarts on the next few seconds of playing");
        return;
    }
    // Remember what the restored tally was measured on: if the host names
    // this track later and it differs, setHostTrackName resets.
    restoredLevelsTrack_ = nowTrack.isEmpty() ? savedTrack : juce::String();
    int slotsPending = 0;
    if (auto* arr = o->getProperty("slots").getArray())
        for (auto& sv : *arr)
            if (auto* so = sv.getDynamicObject())
            {
                const int n = (int) so->getProperty("n");
                if (n >= 1) { pendingSlotLevels_[n] = { so->getProperty("in"), so->getProperty("out") }; ++slotsPending; }
            }
    const bool inOk  = chainInTally_.fromVar(o->getProperty("in"));
    const bool outOk = chainOutTally_.fromVar(o->getProperty("out"));
    EchoJay_NSLog(("EJLevels: pending restore, chain in=" + juce::String(inOk ? "y" : "n")
                   + " out=" + juce::String(outOk ? "y" : "n") + " slots=" + juce::String(slotsPending)
                   + " track=\"" + savedTrack + "\"").toRawUTF8());
}

std::vector<ChainHost::ApplyReport>
ChainHost::applyStructuredSettings (int slotIndex,
                                    const juce::var& structuredSettings,
                                    const juce::var& map)
{
    std::vector<ApplyReport> out;
    if (slotIndex < 0 || slotIndex >= (int) slots_.size()) return out;

    auto& slot = slots_[slotIndex];
    if (slot.node == nullptr) return out;

    auto* proc = slot.node->getProcessor();
    if (proc == nullptr) return out;

    auto* instance = dynamic_cast<juce::AudioPluginInstance*> (proc);
    if (instance == nullptr) return out;

    // Bridged-AU report-only (10 Aug 2026, DEFECT_BRIDGED_READBACK option
    // a): on an instance whose serving binary was MEASURED bridged, the
    // in-stack display read is pre-write, so display verification demotes
    // to norm round-trip instead of reverting correct work. Unknown or
    // unreadable components read native (EchoJayBridgedAU.h).
    const bool staleDisplayReads = echojay::auComponentIsBridged (slot.desc);
    if (staleDisplayReads)
        EchoJay_NSLog(("EJDial: \"" + slot.desc.name
                       + "\" is a bridged AU (no arm64 slice); display readback "
                         "demoted to norm round-trip, mismatches reported not reverted").toRawUTF8());

    auto results = echojay::applySettings (*instance, map, structuredSettings, staleDisplayReads);
    for (auto& r : results)
        out.push_back ({ r.semantic, r.applied, r.normalized, r.note,
                         r.landedText, r.displayVerified, r.readbackMismatch,
                         r.staleDisplayKept, r.requestedValue, r.outOfRange,
                         r.index, r.anchorsUnverified, r.beforeText });

    return out;
}

void ChainHost::removeSlot(int i)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    // MOVE LOG: what left. Recorded before the slot goes, while its name is
    // still in hand.
    recordStructural(MoveLogEntry::Kind::Remove, i, juce::String(),
                     slots_[(size_t) i].desc.name, juce::String());
    // Before the instance goes to the graveyard, where it stays ALIVE for
    // the session: a parked plugin with a leaked UI timer must not be able
    // to call a listener on a ChainHost that has since gone away.
    detachHostedListener(i);
    for (auto& c : graph_->getConnections()) graph_->removeConnection(c);
    if (slots_[i].node)
    {
        // Disconnect from the graph but DO NOT destroy the instance: some
        // plugins (AMEK EQ 250) leak repeating UI timers that keep firing
        // after their editor is gone. The timers are harmless while the
        // AudioUnit is alive, but disposing it turns the next tick into a
        // use-after-free (confirmed SIGSEGV in the crash log). Removed
        // instances are parked in the graveyard for the session instead.
        graveyard_.push_back(slots_[i].node);
        graph_->removeNode(slots_[i].node->nodeID);
    }
    // The wet-blend node is OURS (no third-party UI timers) — destroy for real
    if (slots_[i].blendNode)
        graph_->removeNode(slots_[i].blendNode->nodeID);
    slots_.erase(slots_.begin() + i);
    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
}

void ChainHost::moveSlot(int i, int direction)
{
    int j = i + direction;
    if (i < 0 || i >= (int)slots_.size()) return;
    if (j < 0 || j >= (int)slots_.size()) return;
    std::swap(slots_[i], slots_[j]);
    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
}

void ChainHost::setSlotBypassed(int i, bool bypassed)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    slots_[i].bypassed = bypassed;
    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
}

juce::AudioProcessorEditor* ChainHost::createEditorForSlot(int i)
{
    if (i < 0 || i >= (int)slots_.size()) return nullptr;
    if (!slots_[i].node) return nullptr;
    try
    {
        auto* proc = slots_[i].node->getProcessor();
        if (proc == nullptr) return nullptr;
        const int mark = pushDeathMark("editor creation", slots_[i].desc);
        auto* ed = proc->createEditor();
        popDeathMark(mark);
        return ed;
    }
    catch (...) { return nullptr; }
}

// ---------------------------------------------------------------------------
// Async load (appends to chain)
// ---------------------------------------------------------------------------
void ChainHost::asyncCreatePlugin(const juce::PluginDescription& d,
    std::function<void(std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)> cb)
{
    // Death mark up for the instantiate, and held THROUGH the caller's
    // callback: completeLoad inserts the node and prepares the graph in
    // there, and a plugin that dies in prepareToPlay died at instantiate for
    // every purpose the blacklist serves.
    const int mark = pushDeathMark("instantiate", d);
    formatManager_.createPluginInstanceAsync(d, sampleRate_, blockSize_,
        [mark, cb = std::move(cb)](std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
        {
            if (cb) cb(std::move(inst), err);
            popDeathMark(mark);
        });
}

void ChainHost::completeLoad(std::unique_ptr<juce::AudioPluginInstance> inst,
                              const juce::PluginDescription& desc,
                              LoadOrigin origin)
{
    // Any successful load clears a stale session load-failure mark
    sessionLoadFailed_.removeString(sessionLoadKey(desc.name, desc.pluginFormatName));

    if (mode_ == Mode::Borrowed) ++borrowFresh_;   // the gate's counter

    ChainSlot slot;
    slot.node     = graph_->addNode(std::move(inst));
    slot.desc     = desc;
    // Backfill from the INSTANCE (see the twin note at the plan-attach
    // sites): a restore's request desc lacks the manufacturer the
    // float-by-identity placement keys on.
    if (slot.desc.manufacturerName.isEmpty() && slot.node != nullptr)
        if (auto* pi = dynamic_cast<juce::AudioPluginInstance*>(slot.node->getProcessor()))
            slot.desc.manufacturerName = pi->getPluginDescription().manufacturerName;
    // Discriminator log (2 Sep): one side of the write; the projection's
    // EJPane line is the other. The timestamps say which candidate holds.
    EchoJay_NSLog(("EJPlace: stored \"" + slot.desc.name + "\" mfr=\""
                   + slot.desc.manufacturerName + "\" (async load)").toRawUTF8());
    slot.bypassed = false;
    const auto arrivedName = slot.desc.name;
    slots_.push_back(std::move(slot));
    // MOVE LOG: a slot arriving, and ONLY where the origin licenses a claim.
    // Restore records nothing: reopening a session or recalling a saved chain
    // is not something EchoJay did, and the first version wrote a BUILT line
    // per slot at turn 0 for both. User records a different kind, because the
    // user putting a plugin in is a fact EchoJay needs and an act it did not
    // perform. The decision is the caller's, made in ONE place below.
    recordLoadIfLicensed(origin, (int) slots_.size() - 1, arrivedName);
    // Pristine default, captured BEFORE any seed or dial (borrow reset).
    captureBorrowDefaultState((int) slots_.size() - 1);
    // A VST3 build inside an AU host is told in the rack, on every route
    // (session restore, shared chain, picker, model): the chain that comes
    // back is the chain that was saved, and it is not silent about it.
    if (hostPluginFormat_ == "AudioUnit" && desc.pluginFormatName == "VST3")
        addStateNote(desc.name + ": the VST3 build, hosted inside this AU host."
                     " Experimental; the chain list offers VST3s here only while"
                     " vst3_in_au_host is on");
    // Second net for SettingsTooLarge (the fingerprint pass is the first):
    // a plugin fingerprinted before this measurement existed is measured at
    // its first rack. Default state, right after instantiate; the slot stays
    // racked for this session and is withheld from the list from now on.
    if (!isBuiltinDescription(desc))
        if (auto* p = slots_.back().node ? slots_.back().node->getProcessor() : nullptr)
        {
            juce::MemoryBlock st;
            try { p->getStateInformation(st); } catch (...) { st.reset(); }
            if ((int) st.getSize() > kSessionStateMaxSlotBytes)
            {
                recordStateOversize(desc.fileOrIdentifier, (int) st.getSize(), desc.name, "first rack");
                addStateNote(desc.name + ": its settings are " + juce::File::descriptionOfSizeInBytes((juce::int64) st.getSize())
                             + " at their defaults, over the " + juce::File::descriptionOfSizeInBytes((juce::int64) kSessionStateMaxSlotBytes)
                             + " a session can save per plugin, so a chain holding it cannot be saved with the project;"
                               " it stays racked now and is withheld from the chain list from here on");
            }
        }
    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }

    // Auto-parameter-mapping: fingerprint the freshly loaded instance
    // (format|uidHex|version|param_count — the extractor's exact scheme;
    // param_count only exists on a LOADED instance, which is why fps are
    // computed here and not at scan time), register identity -> fp in the
    // persistent index so scan-time prefetch covers this plugin from now
    // on, then apply any pending structured settings the moment a map is
    // available.
    const int newSlotIdx = (int)slots_.size() - 1;
    if (auto* newProc = slots_[(size_t)newSlotIdx].node ? slots_[(size_t)newSlotIdx].node->getProcessor() : nullptr)
    {
        // Fingerprint from the INSTANCE's own filled-in description, not the
        // desc that loaded it: registry-enumerated AU descs carry the AU
        // component triple in `version` (e.g. "aufx,Ni$6,-NI-"), which can
        // never match the extractor's real version string, so seeded AU maps
        // were unreachable from a live session. The loaded instance knows its
        // true identity in every format; for VST3 the fields are identical
        // either way, so existing VST3 fingerprints do not change.
        juce::PluginDescription liveDesc;
        if (auto* inst = dynamic_cast<juce::AudioPluginInstance*>(newProc))
            liveDesc = inst->getPluginDescription();
        if (liveDesc.pluginFormatName.isEmpty() || liveDesc.version.isEmpty())
            liveDesc = desc;   // never fingerprint from a blank description
        slots_[(size_t)newSlotIdx].fp =
            echojay::fingerprintForDescription(liveDesc, newProc->getParameters().size());
        // Stale-map ladder (12 Aug 2026). This is the ONLY point where index
        // staleness is detectable: a live fp differing from the indexed fp
        // proves the index described a binary that is no longer installed.
        // The decisions are echojay::staleLadderAtLoad (pure, pinned by
        // mapfps_test); the side effects happen here. On divergence the
        // index self-corrects as before, the slot is marked as having loaded
        // against a fingerprint the server did not have, and the live fp's
        // map is fetched NOW through the existing prefetch path (the
        // corrected index makes the sweep want it). The apply for this slot
        // then holds naturally: paramMaps_ has nothing under the live fp, so
        // applyStructuredIfReady parks it pending until storeParamMaps
        // answers, where settleStaleRung names the rung.
        const auto ik = echojay::identityKeyForDescription(liveDesc);
        auto idxIt = identityToFp_.find(ik);
        const juce::String indexedFp =
            (idxIt != identityToFp_.end()) ? idxIt->second : juce::String();
        const juce::String liveFp = slots_[(size_t)newSlotIdx].fp;
        const auto step = echojay::staleLadderAtLoad(
            indexedFp, liveFp, paramMaps_.find(liveFp) != paramMaps_.end());
        if (step.correctIndex && liveDesc.uniqueId != 0)
        {
            // The uid guard's write side: a zero-uid identity key (VST3|0|)
            // is the shared prefix of every thin scan row, so indexing one
            // would poison the whole zero-uid population. fpForIdentity
            // refuses zero uids at read; this keeps the index clean at the
            // source too.
            identityToFp_[ik] = liveFp;
            saveParamMapsToDisk();
        }
        if (step.markSlot)
            slots_[(size_t)newSlotIdx].staleIndexedFp = indexedFp;
        if (step.kickRefetch)
        {
            requestMapPrefetch();
            // Honest status while the answer is in flight: the prefetch path
            // marks requested but not pending, and pending is what keeps
            // applyStructuredIfReady saying "pending" instead of "noMap".
            // Only marked when the request actually left (editor wired).
            if (mapsRequested_.contains(liveFp))
                pendingMapFps_.addIfNotAlreadyThere(liveFp);
        }
        EchoJay_NSLog(("EJStaleMap: slot=" + juce::String(newSlotIdx)
                       + " \"" + slots_[(size_t)newSlotIdx].desc.name + "\""
                       + " indexed=" + (indexedFp.isEmpty() ? juce::String("(none)")
                                                            : indexedFp.substring(0, 12))
                       + " live=" + liveFp.substring(0, 12)
                       + " rung=" + echojay::staleRungName(step.rung)).toRawUTF8());
        // VST3 identity capture, option 1 (13 Aug 2026): moduleinfo.json
        // measured ZERO of 189 on this machine (FabFilter, iZotope, Waves
        // and TR5 all absent), so scan-time identity for VST3 is not
        // recoverable from bundles. The load IS the measurement: persist
        // the validated description into knownPlugins_ (chain_plugins.xml),
        // which the doRefresh join was built to consume, fill the thin
        // in-memory entry now, and unlatch the resolver so the plugin the
        // user just loaded is identifiable THIS session, not after the
        // next Scan Now. Coverage grows with use, the AU model's shape.
        if (liveDesc.pluginFormatName == "VST3" && liveDesc.uniqueId != 0)
        {
            {
                std::lock_guard<std::mutex> lk(pluginsMutex_);
                knownPlugins_.addType(liveDesc);
            }
            saveToDisk();
            const int filled = enrichThinVst3EntriesFromKnown();
            if (filled > 0) hasResolved_ = false;
            EchoJay_NSLog(("EJScan: VST3 identity captured at load, \""
                           + liveDesc.name + "\" uid=" + juce::String(liveDesc.uniqueId)
                           + " version=" + liveDesc.version
                           + ", " + juce::String(filled)
                           + " thin entr(ies) enriched").toRawUTF8());
        }
        applyStructuredIfReady(newSlotIdx, DialTrigger::slotLoaded);
    }

    // Hosted settings cache. No-op unless enabled (it is not in EchoJay
    // Link, which captures live in chainModelToVar). The first capture is
    // taken NOW rather than waiting for the debounce: a plugin added a
    // second before the user hits Cmd-S would otherwise save as null, which
    // reads as "your settings were dropped" for a slot nothing was wrong
    // with. Every later capture for this slot goes through the timer.
    if (stateCacheEnabled_)
    {
        attachHostedListener(newSlotIdx);
        captureSlotState(newSlotIdx, juce::Time::getMillisecondCounterHiRes());
    }
}

// ---------------------------------------------------------------------------
// Auto-parameter-mapping pipeline (the ONE apply path)
// ---------------------------------------------------------------------------
void ChainHost::setSlotStructuredSettings(int i, const juce::var& structured)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    if (structured.isVoid()) return;
    // Normally settings_structured is a flat object. A STRUCTURED built-in (the
    // EQ's eq_bands, a future 4-band comp's comp_bands) also accepts a bare
    // ARRAY, so a caller can hand it the list directly without wrapping it.
    if (structured.getDynamicObject() == nullptr
        && ! (structured.isArray() && isBuiltinSlot(i)))
        return;

    slots_[(size_t)i].structuredSettings = structured;
    slots_[(size_t)i].structuredApplied  = false;
    // New request, new denominator: a lingering count from the previous
    // apply must not travel with this request's declines (dial-3 A3).
    slots_[(size_t)i].dialRequestedCount = -1;
    applyStructuredIfReady(i, DialTrigger::settingsAttached);

    // Map not cached yet (first-ever encounter of this plugin): fetch just
    // this fingerprint; storeParamMaps applies the pending slot on arrival.
    auto& fp = slots_[(size_t)i].fp;
    if (!slots_[(size_t)i].structuredApplied && fp.isNotEmpty()
        && paramMaps_.find(fp) == paramMaps_.end()
        && !mapsRequested_.contains(fp) && onNeedParamMaps)
    {
        mapsRequested_.add(fp);
        pendingMapFps_.addIfNotAlreadyThere(fp);
        onNeedParamMaps(juce::StringArray(fp));
    }
    // PRODUCT FALLBACK, TRIGGERED HERE (27 Aug 2026) and not on the exact
    // fetch's completion. THIS is the event that needs it: a dial resolving
    // against a racked slot whose fingerprint has no map. The slot exists, so
    // its live param_count and param_names are readable now, which is what the
    // server's guards require.
    //
    // fallbackRequested_ is its OWN set. Keying off mapsRequested_ was the
    // defect: the prefetch adds every known fp to that set before the rack
    // exists, which then suppressed the only fetch the fallback could have
    // hung off. Separate question, separate ledger, and it still stops the same
    // fp being re-asked in a loop.
    //
    // structuredSettings and structuredApplied=false were set above and are not
    // touched here, so the request survives the round trip and storeFallbackMaps
    // has something to apply when the map lands.
    if (!slots_[(size_t)i].structuredApplied && fp.isNotEmpty()
        && paramMaps_.find(fp) == paramMaps_.end()
        && !fallbackRequested_.contains(fp) && onNeedFallbackMaps)
    {
        const auto body = buildFallbackLookupJsonForSlot(i);
        if (body.isNotEmpty())
        {
            fallbackRequested_.add(fp);
            // Rides pendingMapFps_ so the line below marks the slot pending
            // rather than noMap while the answer is in flight, and so the
            // bubble does not claim "no map" about a slot still being asked
            // about. storeFallbackMaps clears it on arrival.
            pendingMapFps_.addIfNotAlreadyThere(fp);
            EchoJay_NSLog(("EJFallback: asking for slot " + juce::String(i + 1)
                           + " (\"" + slots_[(size_t)i].desc.name + "\") fp "
                           + fp.substring(0, 12) + " -- no exact map").toRawUTF8());
            onNeedFallbackMaps(body);
        }
    }
    // The applyStructuredIfReady above ran BEFORE the fetch kicked, so a
    // first-encounter slot got noMap; correct it to pending while the
    // answer is in flight.
    if (!slots_[(size_t)i].structuredApplied && pendingMapFps_.contains(fp))
        slots_[(size_t)i].dialStatus = DialStatus::pending;
    // Stale-map ladder: on the map-held branch the dial verdict lands right
    // here at settings attach, not on any fetch answer, so this is where
    // that rung settles.
    if (settleStaleRung(i) && onSlotSettingsChanged) onSlotSettingsChanged();
}

void ChainHost::storeParamMaps(const juce::var& mapsObj)
{
    auto* o = mapsObj.getDynamicObject();
    if (o == nullptr) return;
    int added = 0, retracted = 0;
    for (auto& p : o->getProperties())
    {
        const auto fp = p.name.toString();
        if (!p.value.isVoid() && p.value.getDynamicObject() != nullptr)
        {
            // TTL: the server just confirmed this fp, so stamp it fresh BEFORE
            // the rev-compare below. An unchanged-rev map must still lose its
            // stale flag, otherwise the rev `continue` would leave the old
            // timestamp and applyStructuredIfReady would refetch it forever.
            fpFetchedAt_[fp] = juce::Time::currentTimeMillis();
            // Rev compare: overwrite only when the map is new or its content
            // revision differs (a cached map without a rev predates the
            // invalidation scheme and always counts as stale once). This is
            // what heals machines that cached a since-corrected map.
            auto it = paramMaps_.find(fp);
            const auto newRev = p.value.getProperty("rev", juce::var()).toString();
            if (it != paramMaps_.end())
            {
                const auto oldRev = it->second.getProperty("rev", juce::var()).toString();
                if (oldRev.isNotEmpty() && oldRev == newRev) continue;   // unchanged
            }
            paramMaps_[fp] = p.value;
            ++added;
        }
        else if (paramMaps_.find(fp) != paramMaps_.end())
        {
            // Explicit null for an fp we asked about and have cached: the
            // server RETRACTED the map (defective, no valid entries). Drop
            // the local copy so it can never be applied again.
            paramMaps_.erase(fp);
            ++retracted;
        }
    }
    if (added > 0 || retracted > 0) saveParamMapsToDisk();
    EchoJay_NSLog(("EJParamMaps: stored/updated " + juce::String(added)
                   + " map(s), retracted " + juce::String(retracted) + " from server").toRawUTF8());
    // Every fp in the response counts as ANSWERED (a map object or an
    // explicit null both resolve the fetch); pending slots re-evaluated
    // below settle to applied/partial/noMap accordingly.
    for (auto& p : o->getProperties())
        pendingMapFps_.removeString(p.name.toString());
    bool changed = false;
    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        const bool wasApplied = slots_[(size_t)i].structuredApplied;
        applyStructuredIfReady(i, DialTrigger::mapArrived);
        if (!wasApplied && slots_[(size_t)i].structuredApplied) changed = true;
        // Stale-map ladder: the apply above ran against whatever this
        // response delivered, so the rung is decidable now.
        if (settleStaleRung(i)) changed = true;
    }
    if (changed && onSlotSettingsChanged) onSlotSettingsChanged();
}

int ChainHost::enrichThinVst3EntriesFromKnown()
{
    std::lock_guard<std::mutex> lk(pluginsMutex_);
    // Bundle paths claimed by more than one captured description are shells
    // (WaveShell): filling the single thin shell row from any one member
    // would assert an arbitrary identity, so those paths are skipped.
    std::map<juce::String, int> pathClaims;
    for (const auto& kd : knownPlugins_.getTypes())
        if (kd.pluginFormatName == "VST3" && kd.uniqueId != 0)
            ++pathClaims[kd.fileOrIdentifier];
    int filled = 0;
    for (const auto& kd : knownPlugins_.getTypes())
    {
        if (kd.pluginFormatName != "VST3" || kd.uniqueId == 0) continue;
        if (pathClaims[kd.fileOrIdentifier] > 1) continue;
        for (auto& d : entries_)
            if (d.pluginFormatName == "VST3"
                && d.fileOrIdentifier == kd.fileOrIdentifier
                && (d.uniqueId == 0 || d.version.isEmpty()))
            {
                d.uniqueId = d.deprecatedUid = kd.uniqueId;
                d.version  = kd.version;
                if (d.manufacturerName.isEmpty())
                    d.manufacturerName = kd.manufacturerName;
                ++filled;
            }
    }
    return filled;
}

bool ChainHost::settleStaleRung(int i)
{
    if (i < 0 || i >= (int)slots_.size()) return false;
    auto& s = slots_[(size_t)i];
    if (s.staleIndexedFp.isEmpty() || s.staleSettled) return false;
    // "Answered" needs the request to have LEFT first: a load with the
    // editor unwired kicks nothing, and without the asked guard the next
    // unrelated map arrival would read the absent pending mark as an answer
    // and declare unmapped without the corpus ever being asked. Residual
    // window: a prefetch-sweep request marks asked but not pending, so a
    // response to a DIFFERENT batch landing while that one is in flight can
    // still settle early; the cost is conservative wording that the
    // mapArrived apply then corrects, never a lost dial.
    const bool asked    = mapsRequested_.contains(s.fp);
    const bool answered = asked && ! pendingMapFps_.contains(s.fp);
    // The dial verdict comes from the SLOT, never from map presence (12 Aug
    // 2026, rung A rehearsal: a held map logged a dial over applied=0
    // unusableMap). wrote means something actually landed: applied, or
    // partial with its manual remainder on the card.
    const bool applyRan = s.structuredApplied;
    const bool wrote    = s.dialStatus == DialStatus::applied
                       || s.dialStatus == DialStatus::partial;
    const auto rung = echojay::staleLadderAtResolution(
        answered, paramMaps_.find(s.fp) != paramMaps_.end(), applyRan, wrote);
    if (rung == echojay::StaleRung::refetch
        || rung == echojay::StaleRung::mapHeld) return false;   // verdict not in yet
    // An unmapped verdict before settings attach would settle with no keys
    // to send manual and no prose on the card; the settings-attach settle
    // completes it instead. A slot that never gets settings never speaks,
    // which is right: there is nothing to hand-dial.
    if (rung == echojay::StaleRung::unmapped && s.structuredSettings.isVoid())
        return false;
    s.staleSettled = true;
    EchoJay_NSLog(("EJStaleMap: slot=" + juce::String(i) + " \"" + s.desc.name + "\""
                   + " indexed=" + s.staleIndexedFp.substring(0, 12)
                   + " live=" + s.fp.substring(0, 12)
                   + " rung=" + echojay::staleRungName(rung)).toRawUTF8());
    if (rung == echojay::StaleRung::dialled
        || rung == echojay::StaleRung::undialled)
    {
        // dialled with nothing refused: the card reads "Applied
        // automatically" through the normal path; nothing to add.
        // undialled without divergence context is the plain unusableMap
        // outcome, worded by the composers.
        //
        // DIVERGENCE + OUT-OF-RANGE REFUSALS is the one combination that
        // earns a card note: those values were computed for the version the
        // server was told about, and refusing a value then telling the user
        // to hand-dial that same value would be worse than writing it. In-
        // range values on the same slot dialled normally (same uid at a
        // different version usually shares names and ranges, which is why
        // the whole-set refusal this replaced was over-calibrated).
        bool changed = false;
        if (! s.dialOutOfRange.isEmpty())
        {
            const juce::String note = s.desc.name
                + " loaded at a different version from the one its settings were "
                  "worked out for. The out-of-range values ("
                + s.dialOutOfRange.joinIntoString(", ")
                + ") were computed for that other version and will not map onto "
                  "this one - use them as intent rather than as numbers.";
            if (! s.settings.startsWith(note))
            {
                s.settings = s.settings.isEmpty() ? note : note + "\n" + s.settings;
                clearModelTiers(s);   // stale-map rung: the echo's map no longer applies
                changed = true;
            }
        }
        s.staleIndexedFp.clear();
        return changed;
    }
    // Unmapped: the corpus does not hold the installed binary's fingerprint.
    // The card speaks, and every requested key goes manual so the card's
    // existing hand-dial guidance (the prose the model wrote, kept below the
    // note) carries the values. Control-level refusal logic is deliberately
    // absent: there is no map to refuse against.
    if (auto* o = s.structuredSettings.getDynamicObject())
        for (auto& kv : o->getProperties())
            s.dialManual.addIfNotAlreadyThere(echojay::semanticLabel(kv.name.toString()));
    const juce::String note = "This version of " + s.desc.name
        + " is newer than any mapping we hold, so these controls need dialling by hand.";
    if (! s.settings.startsWith(note))
    {
        s.settings = s.settings.isEmpty() ? note : note + "\n" + s.settings;
        clearModelTiers(s);   // stale-map rung: no mapping, no tiering
    }
    return true;
}

void ChainHost::loadHelperCatalogue()
{
    // Blocker-1 THIRD option (the design's): the helper writes its OWN files and
    // this is their sole reader; the DAW keeps writing chain_plugins.xml on its
    // validated loads. No shared file, so no write race; addType and the
    // identity-key map both dedup at read, so a plugin present in both files
    // resolves once. Cost is one extra load here.
    auto scanXml = getPluginListFile().getSiblingFile("chain_plugins_scan.xml");
    if (scanXml.existsAsFile())
        if (auto doc = juce::XmlDocument::parse(scanXml))
        {
            juce::KnownPluginList tmp;
            tmp.recreateFromXml(*doc);
            int added = 0;
            {
                std::lock_guard<std::mutex> lock(pluginsMutex_);
                for (const auto& d : tmp.getTypes()) { knownPlugins_.addType(d); ++added; }
            }
            EchoJay_NSLog(("EJScan: helper catalogue unioned, " + juce::String(added)
                           + " validated identit(ies) from chain_plugins_scan.xml").toRawUTF8());
        }

    // identityToFp from the helper: same shape and same read-merge as
    // mergeBootstrapMaps' identityToFp block. First-writer-wins on a key, so the
    // DAW's own load-captured fps are never overwritten by the catalogue.
    auto fpScan = getParamMapsCacheFile().getSiblingFile("chain_fp_scan.json");
    if (fpScan.existsAsFile())
    {
        auto root = juce::JSON::parse(fpScan.loadFileAsString());
        int addedIds = 0;
        if (auto* idx = root.getProperty("identityToFp", juce::var()).getDynamicObject())
            for (auto& p : idx->getProperties())
                if (identityToFp_.find(p.name.toString()) == identityToFp_.end())
                {
                    identityToFp_[p.name.toString()] = p.value.toString();
                    ++addedIds;
                }
        if (addedIds > 0)
            EchoJay_NSLog(("EJScan: " + juce::String(addedIds)
                           + " helper fp(s) unioned from chain_fp_scan.json").toRawUTF8());
    }

    reloadHealthFromDisk();   // chain_health.json, read by the withheld panel
}

namespace {
// Numeric version compare, component by component, a missing component read as
// zero (so 12.0 and 12.0.0 are equal). Returns -1 (a<b), 0 (equal), 1 (a>b).
// bothParsed is false if EITHER side has a non-numeric component or is empty;
// the caller must then NOT order them, never guess. A STRING compare is wrong
// here: lexically "9.6" > "12.6" because '9' > '1', which would supersede the
// 12.x shell by the 9.6 one, exactly backwards.
int compareVersionNumeric (const juce::String& a, const juce::String& b, bool& bothParsed)
{
    auto parse = [] (const juce::String& v, bool& ok)
    {
        std::vector<int> out;
        juce::StringArray parts;
        parts.addTokens (v.trim(), ".", "");
        ok = parts.size() > 0 && v.trim().isNotEmpty();
        for (auto& p : parts)
        {
            const auto t = p.trim();
            if (t.isEmpty() || ! t.containsOnly ("0123456789")) ok = false;
            out.push_back (t.getIntValue());
        }
        return out;
    };
    bool ao = false, bo = false;
    const auto va = parse (a, ao), vb = parse (b, bo);
    bothParsed = ao && bo;
    const size_t n = juce::jmax (va.size(), vb.size());
    for (size_t i = 0; i < n; ++i)
    {
        const int x = i < va.size() ? va[i] : 0;   // missing component = 0
        const int y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;   // equal
}
} // namespace

void ChainHost::reloadHealthFromDisk()
{
    // Sibling of chain_plugins.xml, written by ejextract --catalogue. Absent on
    // a machine that has not swept yet: a no-op, not an error.
    auto f = getPluginListFile().getSiblingFile("chain_health.json");
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    health_.clear();
    if (! f.existsAsFile()) return;
    auto root = juce::JSON::parse(f.loadFileAsString());
    if (auto* o = root.getDynamicObject())
        for (auto& p : o->getProperties())
            if (auto* e = p.value.getDynamicObject())
            {
                HealthEntry h;
                h.state   = e->getProperty("state").toString();
                h.reason  = e->getProperty("reason").toString();
                h.blockMs = (juce::int64) e->getProperty("blockMs");
                health_[p.name.toString()] = h;
            }
}

std::map<juce::String, ChainHost::HealthEntry> ChainHost::getHealthSnapshot() const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    return health_;
}

void ChainHost::computeSupersessions()
{
    std::lock_guard<std::mutex> lock (pluginsMutex_);
    superseded_.clear();
    // Group by format|uid|manufacturer, the SAME key the map lookup and the
    // server tier use, NOT the display name. So a uid match with differing
    // names is the same plugin (renamed across versions) and IS compared; a
    // name match with differing uids lands in different groups and is NEVER
    // collapsed, because it is two different plugins. uid 0 is no identity
    // (every thin VST3 row is VST3|0|) and can never anchor a group.
    std::map<juce::String, std::vector<const juce::PluginDescription*>> groups;
    for (const auto& d : entries_)
    {
        if (d.uniqueId == 0) continue;
        const auto key = d.pluginFormatName + "|"
                       + juce::String::toHexString (d.uniqueId) + "|"
                       + d.manufacturerName;
        groups[key].push_back (&d);
    }
    int marked = 0, ambiguous = 0, multi = 0;
    for (auto& kv : groups)
    {
        auto& g = kv.second;
        if (g.size() < 2) continue;   // present at ONE version: never superseded
        ++multi;
        const juce::PluginDescription* newest = g.front();
        bool comparable = true;
        for (size_t i = 1; i < g.size(); ++i)
        {
            bool ok = false;
            const int c = compareVersionNumeric (g[i]->version, newest->version, ok);
            if (! ok) { comparable = false; break; }   // unparseable: leave the group alone
            if (c > 0) newest = g[i];
        }
        if (! comparable) { ++ambiguous; continue; }
        for (auto* d : g)
        {
            if (d == newest) continue;
            bool ok = false;
            if (compareVersionNumeric (d->version, newest->version, ok) < 0 && ok)   // STRICTLY older only
            {
                superseded_.insert (echojay::identityKeyForDescription (*d));
                ++marked;
                if (! namesMatchLoose (d->name, newest->name))
                    EchoJay_NSLog(("EJScan: superseded \"" + d->name + "\" (" + d->version
                                   + ") by \"" + newest->name + "\" (" + newest->version
                                   + "), same uid renamed across versions").toRawUTF8());
            }
        }
    }
    EchoJay_NSLog(("EJScan: supersession " + juce::String(marked) + " older-version row(s) marked across "
                   + juce::String(multi) + " multi-version plugin(s), "
                   + juce::String(ambiguous) + " left alone (unparseable version)").toRawUTF8());
}

bool ChainHost::isSuperseded (const juce::PluginDescription& d) const
{
    if (d.uniqueId == 0) return false;
    std::lock_guard<std::mutex> lock (pluginsMutex_);
    return superseded_.count (echojay::identityKeyForDescription (d)) > 0;
}

void ChainHost::mergeBootstrapMaps()
{
    // Read-only merge of the background mapper's output. The mapper owns
    // param_maps_bootstrap.json exclusively and writes it atomically
    // (tmp + rename); ChainHost owns param_maps.json exclusively. Neither
    // process ever writes the other's file, so there is no write race.
    auto f = getParamMapsCacheFile().getSiblingFile("param_maps_bootstrap.json");
    if (!f.existsAsFile()) return;
    const auto mtime = f.getLastModificationTime();
    if (mtime == bootstrapMergedMtime_) return;
    bootstrapMergedMtime_ = mtime;

    auto root = juce::JSON::parse(f.loadFileAsString());
    int addedIds = 0, addedMaps = 0;
    if (auto* idx = root.getProperty("identityToFp", juce::var()).getDynamicObject())
        for (auto& p : idx->getProperties())
            if (identityToFp_.find(p.name.toString()) == identityToFp_.end())
            {
                identityToFp_[p.name.toString()] = p.value.toString();
                ++addedIds;
            }
    if (auto* maps = root.getProperty("maps", juce::var()).getDynamicObject())
        for (auto& p : maps->getProperties())
            if (p.value.getDynamicObject() != nullptr
                && paramMaps_.find(p.name.toString()) == paramMaps_.end())
            {
                paramMaps_[p.name.toString()] = p.value;
                ++addedMaps;
            }
    if (addedIds > 0 || addedMaps > 0)
    {
        saveParamMapsToDisk();
        EchoJay_NSLog(("EJParamMaps: bootstrap merge, +" + juce::String(addedIds)
                       + " identities, +" + juce::String(addedMaps) + " map(s)").toRawUTF8());
        bool changed = false;
        for (int i = 0; i < (int)slots_.size(); ++i)
        {
            const bool wasApplied = slots_[(size_t)i].structuredApplied;
            applyStructuredIfReady(i, DialTrigger::mapArrived);
            if (!wasApplied && slots_[(size_t)i].structuredApplied) changed = true;
        }
        if (changed && onSlotSettingsChanged) onSlotSettingsChanged();
    }
}

void ChainHost::requestMapPrefetch()
{
    mergeBootstrapMaps();
    if (!onNeedParamMaps) return;
    juce::StringArray need;
    for (auto& kv : identityToFp_)
        if (paramMaps_.find(kv.second) == paramMaps_.end() && !mapsRequested_.contains(kv.second))
            need.addIfNotAlreadyThere(kv.second);
    // CACHE REVALIDATION, once per session: also re-request every fp we
    // already have cached. storeParamMaps rev-compares the responses, so
    // unchanged maps cost nothing, corrected maps overwrite the stale local
    // copy, and retracted maps (explicit null) get dropped. This is how a
    // machine that cached a since-fixed map heals without reinstalling.
    if (!mapsRevalidated_)
    {
        mapsRevalidated_ = true;
        for (auto& kv : paramMaps_)
            need.addIfNotAlreadyThere(kv.first);
    }
    if (need.isEmpty()) return;
    // Batch 100 fps per request. The endpoint accepts 500, but fps ride in
    // a GET URL and 500 of them is a ~32KB request line that dies at the
    // transport (observed live in the bootstrap harness); 100 is ~6.5KB.
    for (int i = 0; i < need.size(); i += 100)
    {
        juce::StringArray batch;
        for (int j = i; j < juce::jmin(need.size(), i + 100); ++j)
        {
            batch.add(need[j]);
            mapsRequested_.add(need[j]);
        }
        EchoJay_NSLog(("EJParamMaps: prefetching " + juce::String(batch.size()) + " map(s)").toRawUTF8());
        onNeedParamMaps(batch);
    }
}

// TTL-on-use tuning. 6h staleness bound: shorter than a working session so a
// server-side correction lands the same day it ships, longer than repeated use
// within one task so a plugin dialled again and again refetches at most once
// per window. The once-per-session revalidation (requestMapPrefetch) refreshes
// everything at open, so this TTL is the safety bound for long sessions and the
// pre-revalidation window, not the primary refresh. kStaleRefetchMs is the
// brief block: exceed it and the slot falls back to hand-dial, never to the
// stale map.
static constexpr juce::int64 kMapTtlMs       = 6LL * 60 * 60 * 1000;
static constexpr int         kStaleRefetchMs = 1500;

bool ChainHost::mapFresh(const juce::String& fp) const
{
    auto it = fpFetchedAt_.find(fp);
    if (it == fpFetchedAt_.end()) return false;   // never confirmed -> stale
    return (juce::Time::currentTimeMillis() - it->second) <= kMapTtlMs;
}

// Refetch a stale cached fp and, if the answer does not arrive within the brief
// window, fall back to HAND-DIAL for any slot still waiting - never to the
// stale map. A returning fetch (storeParamMaps) re-stamps the fp fresh and the
// re-eval loop dials it; a fetch that fails or hangs leaves the slot un-dialled.
void ChainHost::refetchStale(const juce::String& fp)
{
    if (fp.isEmpty() || pendingMapFps_.contains(fp) || !onNeedParamMaps) return;
    pendingMapFps_.addIfNotAlreadyThere(fp);
    onNeedParamMaps(juce::StringArray(fp));
    std::weak_ptr<int> alive = life_;
    juce::Timer::callAfterDelay(kStaleRefetchMs, [this, alive, fp]()
    {
        if (alive.expired()) return;                 // ChainHost gone
        if (!pendingMapFps_.contains(fp)) return;    // fetch already answered
        pendingMapFps_.removeString(fp);             // stop waiting on it
        bool changed = false;
        for (auto& s : slots_)
            if (s.fp == fp && !s.structuredApplied
                && s.structuredSettings.getDynamicObject() != nullptr)
            { s.dialStatus = DialStatus::noMap; changed = true; }
        if (changed && onSlotSettingsChanged) onSlotSettingsChanged();
    });
}

namespace
{
    // The EQ's frozen identifier. The dev command below is genuinely EQ-specific
    // (it writes eq_bands), so it keys on this rather than on "any built-in".
    // Matching the identifier rather than the display name means a rename cannot
    // quietly break it.
    constexpr const char* kBuiltinEqIdentifier = "echojay:builtin:eq";
}

bool ChainHost::isBuiltinEqSlot(int i) const
{
    return isBuiltinSlot(i)
        && slots_[(size_t)i].desc.fileOrIdentifier == kBuiltinEqIdentifier;
}

int ChainHost::findFirstBuiltinEqSlot() const
{
    for (int i = 0; i < (int)slots_.size(); ++i)
        if (isBuiltinEqSlot(i)) return i;
    return -1;
}

juce::String ChainHost::devApplyEqJson(int slotIndex, const juce::String& json)
{
    if (!isBuiltinEqSlot(slotIndex))
        return "slot " + juce::String(slotIndex + 1) + " is not the EchoJay EQ";

    juce::var parsed;
    const auto res = juce::JSON::parse(json, parsed);
    if (res.failed())
        return "JSON parse error: " + res.getErrorMessage();

    int applied = 0, skipped = 0;
    const auto summary = applyStructuredToBuiltinSlot(slotIndex, parsed, &applied, &skipped);
    if (summary.isEmpty())
        return "nothing the EQ understands - expected a bare [...] eq_bands array, or "
               "{\"eq_bands\":[...], \"eq_settings\":{...}}";

    // Keep the rack card in step with what was just written.
    if (slotIndex >= 0 && slotIndex < (int)slots_.size())
    {
        slots_[(size_t)slotIndex].settings = "Applied automatically\n" + summary;
        // No tiering was computed on this path, so the model must not keep an
        // older one beside a newer card.
        clearModelTiers(slots_[(size_t)slotIndex]);
        slots_[(size_t)slotIndex].dialAppliedCount = applied;
        slots_[(size_t)slotIndex].dialStatus =
            (skipped > 0) ? DialStatus::partial : DialStatus::applied;
        if (onSlotSettingsChanged) onSlotSettingsChanged();
    }
    return summary;
}

juce::String ChainHost::applyStructuredToBuiltinSlot(int slotIndex, const juce::var& structured,
                                                     int* appliedOut, int* skippedOut,
                                                     bool* deviceMissingOut)
{
    if (appliedOut != nullptr) *appliedOut = 0;
    if (skippedOut != nullptr) *skippedOut = 0;
    if (deviceMissingOut != nullptr) *deviceMissingOut = false;
    if (slotIndex < 0 || slotIndex >= (int)slots_.size())
    {
        if (deviceMissingOut != nullptr) *deviceMissingOut = true;
        return {};
    }

    // ANY built-in, not just the EQ. The cast is to the shared device base, so
    // this one call site serves all 19 devices and a Wave 1 session adds nothing
    // here (BUILTIN_SUITE_PLAN.md §1).
    auto* device = dynamic_cast<EedDeviceProcessor*>(getSlotProcessor(slotIndex));
    if (device == nullptr)
    {
        if (deviceMissingOut != nullptr) *deviceMissingOut = true;
        return {};
    }

    // One call, whole value. The chain deliberately does NOT reach in for
    // .eq_bands or .params: which keys exist and in what order they resolve is
    // the DEVICE's schema. A structured device resolves its array form and the
    // flat `params` map; a flat device handles `params` alone. A semantic bag
    // meant for the anchor path carries neither and comes back empty.
    return device->applyStructured(structured, appliedOut, skippedOut);
}

// The terminal per-slot verdict. Everything the per-call lines cannot say,
// because each of those is a snapshot taken mid-sequence while the benign
// cases outnumber the real one. Here every slot has had its chance, so
// "nothing dialled" is a fact that can be read off rather than inferred.
//
// Prints for EVERY slot, including the ones that worked, because a summary
// that only lists failures cannot distinguish "all fine" from "never ran" --
// the silent-success trap the register already carries.
// Poll until no slot is still pending (a map fetch in flight), then report
// once. Bounded, and the terminal line says WHICH it was, so an exhausted
// budget never reads as a settled build.
void ChainHost::reportDialWhenSettled(const juce::String& reason, int attemptsLeft)
{
    if (!dialStateSettled() && attemptsLeft > 0)
    {
        auto life = life_;   // weak guard: the host may go away mid-poll
        juce::Timer::callAfterDelay(250, [this, life, reason, attemptsLeft]
        {
            if (life.use_count() <= 1) return;   // owner destroyed
            reportDialWhenSettled(reason, attemptsLeft - 1);
        });
        return;
    }
    logDialSummary(reason + (dialStateSettled() ? ", dial settled"
                                                : ", dial NOT settled (retry budget exhausted)"));
}

// requested, counted in the SAME UNIT as applied (11 Aug 2026).
//
// EchoJay Reverb reported requested=1 applied=7, which is not a near miss, it
// is two different units side by side. `requested` counted TOP-LEVEL KEYS of
// settings_structured, and every payload the model actually sends is a single
// WRAPPER: built-ins get {"params":{...}}, third-party slots get
// {"controls":{...}}, the EQ gets {"eq_bands":[...]}. So requested was almost
// always 1 no matter how much was asked for, and applied>requested read as a
// counting bug on exactly the slots that worked.
//
// A wrapper is a container, not a request. Count what is inside it, name the
// SHAPE, and print the leaf names, because the top-level key alone cannot tell
// a correct payload from a wrong-shaped one -- which is the confusion this
// whole line exists to end.
static int countRequestedSettings (const juce::var& structured,
                                   juce::StringArray& keys,
                                   juce::String& shape)
{
    shape = "none";
    if (structured.isVoid()) return 0;

    if (structured.isArray())
    {
        // A bare array is the EQ's band form arriving without its wrapper.
        shape = "array";
        const int n = structured.size();
        keys.add("(bare array of " + juce::String(n) + ")");
        return n;
    }

    auto* obj = structured.getDynamicObject();
    if (obj == nullptr) { shape = "scalar"; return 0; }

    const auto& props = obj->getProperties();
    if (props.size() == 0) { shape = "empty"; return 0; }

    int requested = 0, wrappers = 0, flat = 0;
    for (const auto& kv : props)
    {
        const juce::String key = kv.name.toString();
        const juce::var& val  = kv.value;

        if ((key == "params" || key == "controls") && val.getDynamicObject() != nullptr)
        {
            ++wrappers;
            juce::StringArray inner;
            for (const auto& leaf : val.getDynamicObject()->getProperties())
                inner.add(leaf.name.toString());
            requested += inner.size();
            keys.add(key + "{" + inner.joinIntoString(", ") + "}");
        }
        else if (val.isArray())
        {
            ++wrappers;
            requested += val.size();
            keys.add(key + "[" + juce::String(val.size()) + "]");
        }
        else
        {
            // A flat semantic key at the top level. Legitimate on the
            // third-party path and the ONLY shape an old prompt produced, so
            // it stays countable rather than being treated as malformed.
            ++flat;
            ++requested;
            keys.add(key);
        }
    }

    shape = (wrappers > 0 && flat > 0) ? "mixed"
          : (wrappers > 0)             ? "wrapped"
                                       : "flat";
    return requested;
}

void ChainHost::logDialSummary(const juce::String& reason) const
{
    const auto statusName = [] (DialStatus st) -> const char*
    {
        switch (st)
        {
            case DialStatus::none:        return "none";
            case DialStatus::pending:     return "pending";
            case DialStatus::applied:     return "applied";
            case DialStatus::partial:     return "partial";
            case DialStatus::noMap:       return "noMap";
            case DialStatus::mapNoCoverage:           return "mapNoCoverage";
            case DialStatus::writesRejected:          return "writesRejected";
            case DialStatus::mapIdentityMismatch:     return "mapIdentityMismatch";
            case DialStatus::builtinPayloadUnmatched: return "builtinPayloadUnmatched";
        }
        return "?";
    };

    int dialled = 0, noSettings = 0;
    EchoJay_NSLog(("EJDialSummary: " + reason + ", " + juce::String((int) slots_.size())
                   + " slot(s)").toRawUTF8());
    for (int i = 0; i < (int) slots_.size(); ++i)
    {
        const auto& s = slots_[(size_t) i];
        const bool hasSettings = ! s.structuredSettings.isVoid();
        const bool builtin = isBuiltinSlot(i);
        if (! hasSettings) ++noSettings;
        if (s.dialAppliedCount > 0) ++dialled;

        // requested = what the model asked for on this slot. Compared against
        // applied, it is the difference between "asked for nothing" and
        // "asked and got nothing", which is the whole question.
        // The KEYS VERBATIM, not just a count. `settings` (the display string
        // on the card) and `settings_structured` (the dial payload) are
        // different fields, and a card full of settings says nothing about
        // whether the dialable ones arrived -- that confusion cost a whole
        // diagnosis pass. Printing the keys also makes a wrong-shape payload
        // self-evident, since flat keys and a "params" wrapper are otherwise
        // the same words.
        juce::StringArray keys;
        juce::String shape;
        const int requested = countRequestedSettings(s.structuredSettings, keys, shape);

        EchoJay_NSLog(("EJDialSummary:   slot " + juce::String(i)
                       + " (\"" + s.desc.name + "\")"
                       + (builtin ? " builtin" : "")
                       + "  settings_structured=" + (hasSettings ? "y" : "n")
                       + "  shape=" + shape
                       + "  keys=[" + keys.joinIntoString(", ") + "]"
                       + "  requested=" + juce::String(requested)
                       + "  applied=" + juce::String(s.dialAppliedCount)
                       // manual and readbackMiss TOGETHER, because manual alone
                       // conflates two opposite failures: a semantic the map
                       // never carried (readbackMiss 0 -> the map is the gap)
                       // and one that was written and disagreed on read-back so
                       // the value was reverted (readbackMiss > 0 -> the map is
                       // wrong, or the plugin cannot be read in-stack). The
                       // fixes point in different directions and the counts are
                       // the only thing that separates them.
                       + "  manual=" + juce::String(s.dialManual.size())
                       + "  readbackMiss=" + juce::String(s.dialReadbackMiss.size())
                       + "  status=" + statusName(s.dialStatus)
                       + "  fp=" + (s.fp.isEmpty() ? juce::String("(none)") : s.fp.substring(0, 12))
                       + "  map=" + (s.fp.isNotEmpty() && paramMaps_.find(s.fp) != paramMaps_.end() ? "y" : "n")).toRawUTF8());
    }
    // The headline, so the common question is answered without reading rows.
    EchoJay_NSLog(("EJDialSummary: " + juce::String(dialled) + "/"
                   + juce::String((int) slots_.size()) + " slot(s) dialled something"
                   + (noSettings > 0 ? ("; " + juce::String(noSettings)
                                        + " carried NO settings from the server") : juce::String())).toRawUTF8());
}

const char* ChainHost::dialTriggerName(DialTrigger t)
{
    switch (t)
    {
        case DialTrigger::slotLoaded:       return "slot-loaded";
        case DialTrigger::settingsAttached: return "settings-attached";
        case DialTrigger::mapArrived:       return "map-arrived";
    }
    return "?";
}

void ChainHost::applyStructuredIfReady(int slotIndex, DialTrigger trigger)
{
    if (slotIndex < 0 || slotIndex >= (int)slots_.size()) return;
    auto& s = slots_[(size_t)slotIndex];
    if (s.structuredApplied) return;

    // THE SILENT RETURNS IN THIS FUNCTION NOW SAY WHICH ONE FIRED (8 Aug 2026).
    // A mapper racked SSL Blitzer, asked for a faster attack, got prose advice,
    // and two hours of unified log said nothing about why. The map was on the
    // server, its fingerprint recomputed exactly, and it held the attack_ms the
    // request needed -- everything downstream was already excluded and the
    // failure still could not be named.
    //
    // Four different failures with four different owners, previously identical
    // from outside: the model was never OFFERED the option, the model chose not
    // to, the slot has no fingerprint, the map is not here.
    if (s.structuredSettings.isVoid())
    {
        // THIS LINE USED TO CLAIM A FAULT ON EVERY HEALTHY BUILD (corrected 10
        // Aug 2026). It fired unconditionally, and both routine callers reach
        // it with void settings by design:
        //   - slot-loaded: completeLoad and the built-in add run BEFORE the
        //     caller attaches settings in the load callback. Every slot of
        //     every build passes through here exactly once, void, always.
        //   - map-arrived: the storeParamMaps sweeps walk EVERY slot, so a
        //     slot that legitimately carries no settings is re-reported once
        //     per map that arrives for some other plugin.
        // Read as a fault, that produced "NO SETTINGS prints for every slot"
        // on a build that may have dialled perfectly, and it cost a whole
        // diagnosis pass. An instrument that cannot be wrong is worth less
        // than no instrument, because it is believed.
        //
        // So the trigger decides the verdict. Only settings-attached can be a
        // genuine void here, and that one is unreachable (setSlotStructured-
        // Settings returns early on void), which is asserted rather than
        // assumed. The real "never dialled" question is terminal, not
        // per-call: logDialSummary answers it once the build is done.
        if (trigger == DialTrigger::slotLoaded || trigger == DialTrigger::mapArrived)
        {
            EchoJay_NSLog(("EJDial: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                           + "\") no settings yet [" + dialTriggerName(trigger)
                           + "] -- EXPECTED ORDERING, not a fault. Settings are attached "
                             "after load; see the EJDialSummary line for what actually dialled.")
                              .toRawUTF8());
            return;
        }

        // settings-attached with nothing attached: a real contradiction.
        jassertfalse;
        EchoJay_NSLog(("EJDial: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                       + "\") NO SETTINGS at dial time [" + dialTriggerName(trigger)
                       + "] -- settings were attached and are not here. This IS the fault."
                         "  appVersion=" + juce::String(JucePlugin_VersionString))
                          .toRawUTF8());
        return;
    }

    // ---- Built-in devices: the exact path --------------------------------
    // This is the entire reason built-ins exist. A move becomes a direct typed
    // write: no fingerprint, no map lookup, no anchor-table interpolation, no
    // write-then-read-back-and-revert. It MUST be handled before the fp/map
    // gates below, which would otherwise park the slot in "pending" forever —
    // a built-in deliberately never gets a fingerprint.
    if (isBuiltinSlot(slotIndex))
    {
        int applied = 0, skipped = 0;
        bool deviceMissing = false;
        const auto summary = applyStructuredToBuiltinSlot(slotIndex, s.structuredSettings,
                                                          &applied, &skipped, &deviceMissing);
        s.structuredApplied = true;
        s.dialAppliedCount  = applied;

        // The cast failure gets its OWN line and its own status. Previously it
        // returned the same empty summary as an unrecognised payload and was
        // reported as unusableMap -- a slot that is a built-in by isBuiltinSlot
        // but holds no EedDeviceProcessor is a routing bug, and reporting it as
        // "the device did not understand the settings" points every reader at
        // the payload, which would be intact.
        if (deviceMissing)
        {
            s.dialStatus = DialStatus::none;
            EchoJay_NSLog(("EJDial: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                           + "\") BUILT-IN WITH NO DEVICE -- isBuiltinSlot is true but the slot "
                             "holds no EedDeviceProcessor. This is a routing fault, NOT a payload "
                             "one; the settings were never offered to anything.").toRawUTF8());
            if (onSlotSettingsChanged) onSlotSettingsChanged();
            return;
        }

        // The summary, not the applied count, is the verdict. A move can be
        // entirely device-global ({"eq_settings":{"auto_gain":true}}) — it
        // applies zero BANDS and is a complete success.
        if (summary.isNotEmpty())
        {
            s.settings   = "Applied automatically\n" + summary;
            clearModelTiers(s);   // built-in: no map, no tiering
            // Honest verdict, same contract as the mapped path: anything the
            // device could not place (the EQ was full, an id was unknown) is
            // partial, not success.
            s.dialStatus = (skipped > 0) ? DialStatus::partial : DialStatus::applied;
        }
        else
        {
            // The device exists and resolved neither of its two accepted
            // shapes. Name the keys it was actually handed, because the whole
            // failure is a shape mismatch and the keys ARE the diagnosis: a
            // flat semantic bag ({"low_cut_freq_hz":80,...}) is the anchor-path
            // shape and the device wants {"params":{...}} or its array form.
            // A9 step 3: its OWN value. No map exists on this path at all
            // (a built-in deliberately never gets a fingerprint), so neither
            // "the map covered nothing" nor "the writes were rejected" is a
            // true sentence about it. Enum value only — no wire reason.
            s.dialStatus = DialStatus::builtinPayloadUnmatched;
            juce::StringArray got;
            if (auto* o = s.structuredSettings.getDynamicObject())
                for (auto& kv : o->getProperties()) got.add(kv.name.toString());
            else if (s.structuredSettings.isArray())
                got.add("(bare array)");
            EchoJay_NSLog(("EJDial: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                           + "\") BUILT-IN PAYLOAD NOT UNDERSTOOD -- device present, resolved "
                             "neither accepted shape. got keys: [" + got.joinIntoString(", ")
                           + "]  wanted: \"params\":{...}" + (isBuiltinSlot(slotIndex)
                               ? juce::String(" (or the device's own array form, e.g. \"eq_bands\")")
                               : juce::String())).toRawUTF8());
        }

        EchoJay_NSLog(("EJParamApply: slot " + juce::String(slotIndex)
                       + " (\"" + s.desc.name + "\") EXACT built-in apply, "
                       + juce::String(applied) + " band(s), "
                       + juce::String(skipped) + " skipped").toRawUTF8());

        if (onSlotSettingsChanged) onSlotSettingsChanged();
        return;
    }

    if (s.structuredSettings.getDynamicObject() == nullptr)
    {
        EchoJay_NSLog(("EJDial: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                       + "\") SETTINGS NOT AN OBJECT -- present but unusable")
                          .toRawUTF8());
        return;
    }
    if (s.fp.isEmpty())
    {
        // Settings arrived for a slot with no fingerprint. The fp is computed
        // at load, so this says the LOAD did not complete -- not that the
        // corpus is missing anything.
        EchoJay_NSLog(("EJDial: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                       + "\") NO FINGERPRINT -- settings present but the slot never learned "
                         "its fp at load; no lookup is possible").toRawUTF8());
        s.dialStatus = DialStatus::pending;
        return;
    }
    auto it = paramMaps_.find(s.fp);
    if (it == paramMaps_.end())
    {
        // Apply-time honesty: never a silent skip any more. Outcome is
        // "pending" while a fetch is in flight, "noMap" once the fetch has
        // answered (or was never possible). The result bubble reads this.
        //
        // NAMES THE FP AND WHETHER A FETCH WAS EVER ASKED FOR: requested-and-
        // unanswered is a transport or corpus problem, never-requested is a
        // wiring one. onNeedParamMaps is installed by the EDITOR and nulled
        // when it closes, so a dial attempted with the window shut reads
        // fetch_wired=n rather than as a missing map.
        const bool inFlight  = pendingMapFps_.contains(s.fp);
        const bool everAsked = mapsRequested_.contains(s.fp);
        s.dialStatus = inFlight ? DialStatus::pending : DialStatus::noMap;
        EchoJay_NSLog(("EJDial: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                       + "\") NO MAP for fp=" + s.fp.substring(0, 12)
                       + "  fetch_requested=" + (everAsked ? "y" : "n")
                       + "  in_flight=" + (inFlight ? "y" : "n")
                       + "  fetch_wired=" + (onNeedParamMaps ? "y" : "n")
                       + "  cached_maps=" + juce::String((int) paramMaps_.size())
                       + " -> " + (inFlight ? "pending" : "noMap")).toRawUTF8());
        return;
    }

    // INTEGRITY: the map's own fp field must equal the slot's live
    // fingerprint, not just the cache key it was stored under. Catches any
    // keying bug (server response, cache merge, disk corruption) before a
    // wrong-layout map can touch a single parameter.
    const auto mapFp = it->second.getProperty("fp", juce::var()).toString();
    // A PRODUCT FALLBACK IS THE ONE LEGITIMATE DISAGREEMENT (26 Aug 2026).
    // The server serves a prior version's map in this identity's place and
    // tags it anchors_unverified + served_from, so its fp field NAMES ANOTHER
    // BINARY by design. Refusing it here would make the fallback unreachable.
    //
    // Narrow on purpose: only a map carrying the tag is exempt. Every other
    // fp disagreement is still the keying bug this check was built to catch,
    // and an untagged mismatch still refuses exactly as before.
    const bool servedAsFallback = (bool) it->second.getProperty("anchors_unverified", false);
    if (mapFp != s.fp && ! servedAsFallback)
    {
        EchoJay_NSLog(("EJParamApply: map fp mismatch for slot " + juce::String(slotIndex)
                       + " (\"" + s.desc.name + "\"): key " + s.fp.substring(0, 12)
                       + " vs map.fp " + (mapFp.isEmpty() ? juce::String("(missing)")
                                                          : mapFp.substring(0, 12))
                       + ", apply refused").toRawUTF8());
        // A9 step 3: NOT mapNoCoverage. The apply is refused here BEFORE
        // applyStructuredSettings is called, so the map's contents are never
        // read and its coverage is unassessed rather than poor. The owner is
        // keying/transport/cache, which is the one failure on this list that
        // says OUR pipeline is broken; folding it into a corpus-quality or a
        // vendor-behaviour bucket would hide it behind the wrong reader.
        s.dialStatus = DialStatus::mapIdentityMismatch;
        return;
    }

    // Dialable-flag visibility: under the strict default (absent -> not
    // dialable) a wiring bug and "no flag yet" both read false, so log the
    // flag's ACTUAL state on the map this slot holds. "true"/"false" proves the
    // server flag arrived and is readable; "ABSENT" means an old cache or a
    // transport/parse drop - the two now look different in the log.
    {
        auto dv = it->second.getProperty("dialable", juce::var());
        EchoJay_NSLog(("EJDialable: slot " + juce::String(slotIndex) + " (\"" + s.desc.name
                       + "\") fp=" + s.fp.substring(0, 12)
                       + " dialable=" + (dv.isBool() ? (((bool) dv) ? "true" : "false") : "ABSENT")
                       + " category=" + it->second.getProperty("category", juce::var()).toString()
                       + " fresh=" + (mapFresh(s.fp) ? "y" : "n")).toRawUTF8());
    }

    // TTL-on-use: a cached map older than the staleness bound may be a since-
    // corrected map (the AMEK suppression class that wrote Mono Maker). Do NOT
    // apply it. Refetch and block briefly; refetchStale falls back to hand-dial
    // on timeout rather than to the stale map. A confirming fetch re-stamps the
    // fp and the storeParamMaps re-eval dials it.
    if (!mapFresh(s.fp))
    {
        refetchStale(s.fp);
        s.dialStatus = DialStatus::pending;
        return;
    }

    auto report = applyStructuredSettings(slotIndex, s.structuredSettings, it->second);
    s.structuredApplied = true;

    EchoJay_NSLog(("EJParamApply: slot " + juce::String(slotIndex) + " (\"" + s.desc.name + "\"), "
                   + juce::String((int)report.size()) + " result(s)").toRawUTF8());
    juce::StringArray appliedSummary;
    s.dialManual.clear();
    s.dialReadbackMiss.clear();
    s.dialUnconfirmed.clear();
    s.dialApproximate.clear();
    s.dialServedFrom = it->second.getProperty("served_from", juce::var()).toString();
    s.dialOutOfRange.clear();
    // dial-3 denominator (A3): the count of settings the model asked for,
    // stored HERE because appliedCount + manual.size() is not a substitute
    // (both dedupe through semanticLabel).
    s.dialRequestedCount = (int) report.size();
    for (auto& r : report)
    {
        EchoJay_NSLog(("EJParamApply:   " + r.semantic + ": "
                       + (r.applied ? juce::String("APPLIED ") : juce::String("manual  "))
                       + juce::String(r.normalized, 3) + "  (" + r.note + ")"
                       + (r.landedText.isNotEmpty() ? "  landed \"" + r.landedText.trim() + "\""
                                                    : juce::String())).toRawUTF8());
        if (r.applied)
        {
            // The value comes off the RESULT, not a flat lookup on the
            // settings object: band values live in bands[i] and control
            // values in controls["Name"], so the flat lookup returned void
            // and the card printed bare repeated labels ("freq Hz, gain dB,
            // freq Hz, gain dB") - no values, no record of what happened.
            auto line = echojay::formatSemanticSetting(r.semantic, r.requestedValue);
            // A successful write shows NOTHING extra (9 Aug 2026, Sean's
            // call): silence is the signal that it worked, like everything
            // else in the app. The old "(unverified)" suffix surfaced an
            // INTERNAL proof-class distinction (norm round-trip vs display
            // comparison) as user-facing doubt, on every setread map -
            // i.e. the entire campaign corpus, forever. The distinction is
            // not lost: r.note carries it in the EJParamApply log line,
            // and the verification class is static per control
            // (method/trust on the map entry). The dangerous case - the
            // display DISAGREEING - was never silent and still is not: it
            // reverts, lands in dialManual/dialReadbackMiss, and uploads a
            // readback_mismatch dial_miss.
            appliedSummary.add(line);
            // The bridged report-only case is NOT the silent class: the
            // display DISAGREED and the write was kept anyway, on a measured
            // fact about the instance. The 9 Aug silence rule reasoned "the
            // display disagreeing was never silent - it reverts"; with the
            // revert gone, the caveat must surface instead.
            if (r.staleDisplayKept)
                s.dialUnconfirmed.addIfNotAlreadyThere(echojay::semanticLabel(r.semantic));
            // THE 9 AUG SILENCE RULE DOES NOT REACH HERE (26 Aug 2026). That
            // rule says a successful write shows nothing extra, and it was
            // right because silence meant "it landed as asked". On a product
            // fallback the anchors came from another version and drift on
            // ~19% of controls, so silence would be asserting something we
            // measured to be false a fifth of the time. Named here, on the
            // card, and marked to the model.
            if (r.anchorsUnverified)
                s.dialApproximate.addIfNotAlreadyThere(echojay::semanticLabel(r.semantic));
        }
        else
        {
            s.dialManual.addIfNotAlreadyThere(echojay::semanticLabel(r.semantic));
            if (r.readbackMismatch)
                s.dialReadbackMiss.addIfNotAlreadyThere(echojay::semanticLabel(r.semantic));
            if (r.outOfRange)
                s.dialOutOfRange.addIfNotAlreadyThere(echojay::semanticLabel(r.semantic));
        }
    }

    // The range-check counter (12 Aug 2026), printed on EVERY dialled slot
    // in the EJMapFps vocabulary, zero case included: if out-of-range asks
    // turn out to be common on healthy (non-diverged) turns, the exposure
    // is not communicating ranges to the model well enough - a finding
    // nothing else can currently see.
    EchoJay_NSLog(("EJRangeCheck: slot=" + juce::String(slotIndex)
                   + " \"" + s.desc.name + "\""
                   + " requested=" + juce::String((int) report.size())
                   + " outOfRange=" + juce::String(s.dialOutOfRange.size())
                   + (s.dialOutOfRange.isEmpty() ? juce::String()
                        : " [" + s.dialOutOfRange.joinIntoString(", ") + "]")
                   + (s.staleIndexedFp.isNotEmpty() ? " diverged=y" : " diverged=n")).toRawUTF8());

    // Honest per-slot verdict: applied only when EVERY requested semantic
    // was written; anything less is partial (some written) or unusableMap
    // (map exists, nothing written). ">=1 written" reported as success
    // would still overclaim (the spiff class of bug).
    s.dialAppliedCount = (int) appliedSummary.size();
    if (report.empty())
        s.dialStatus = DialStatus::mapNoCoverage; // structured present, nothing requested survived
    else if (s.dialManual.isEmpty())
        s.dialStatus = DialStatus::applied;
    else if (s.dialAppliedCount > 0)
        s.dialStatus = DialStatus::partial;
    else if (echojay::dialWritesBlocked())
        // DO NOT DIAL: nothing was written because the user asked for nothing
        // to be written. Its own status so the bubble can say that instead of
        // the unsupported-plugin sentence, and so the dial-miss emitter can
        // skip it: a deliberate setting is not a miss.
        s.dialStatus = DialStatus::writesBlocked;
    else
        // The map covered it and the writes were ATTEMPTED — this is the only
        // status that wrote anything, which is why it is the only one the
        // emitter lets pair with readback_mismatch (A9 §1c).
        s.dialStatus = DialStatus::writesRejected;

    // Card honesty (20 Aug 2026): the card read "attack 3ms, release 7ms"
    // while the knobs went to positions 3 and 7 on a 1..7 scale — the
    // model's imagined unit, never corrected by what the plugin received.
    // Say what LANDED, in the three tiers the apply already decided, never
    // a fourth:
    //   - display-verified: the plugin's own display text is ground truth.
    //   - bridged (staleDisplayKept): already annotated as unverifiable
    //     upstream; nothing is added here.
    //   - setread / unparseable display / position: the written value, in
    //     the MAP's vocabulary, on its OWN line marked unverified.
    //
    // 6a (24 Aug 2026), and it is NOT the one-line swap the contract
    // imagined. landedText was never "unused" here: the displayVerified
    // branch below has always reported it. What was wrong is that the OTHER
    // three tiers printed r.requestedValue through the same arrow, so a
    // request and a landing were byte-identical to the reader.
    //
    // Those three tiers cannot be fixed by reading landedText instead,
    // because on every one of them the read is known-untrustworthy at the
    // moment it is taken, not merely unverified:
    //   - setread maps EXIST because the plugin's getText ignores its
    //     argument (EchoJayParamApply.h, the method switch). The string is a
    //     lie by the map's own declaration.
    //   - position: "positions carry no display expectation" (:470).
    //   - unparseable: typedReadbackMatch already returned 0 on that text.
    // So the honest report on those tiers is the value we ASKED for, said as
    // a request. Marked, not omitted: a control that was written and then
    // dropped from the card is indistinguishable from one never requested,
    // and an unreadable absence is the defect this contract is about.
    // THE RULE: the card must never restate a unit the map does not
    // declare. Where the map's unit is null or disagrees with the key's
    // suffix, show the range instead. That is the whole of tonight's
    // CLA-76 defect in one sentence.
    {
        auto entryFor = [&](const juce::String& key) -> juce::var
        {
            auto e = it->second.getProperty("params", juce::var())
                               .getProperty(key, juce::var());
            if (! e.isObject())
                e = it->second.getProperty("controls", juce::var())
                              .getProperty(key, juce::var());
            return e;
        };
        auto declaredUnit = [](const juce::var& entry, const juce::String& key) -> juce::String
        {
            const auto u = entry.getProperty("unit", juce::var()).toString().trim();
            if (u.isEmpty()) return {};
            // A flat key's suffix implies a unit; when the map disagrees,
            // the range speaks instead (the rule above).
            struct SufUnit { const char* suf; const char* unit; };
            for (const auto& su : { SufUnit{"_db","db"}, SufUnit{"_ms","ms"},
                                    SufUnit{"_hz","hz"}, SufUnit{"_s","s"} })
                if (key.endsWith(su.suf) && ! u.equalsIgnoreCase(su.unit))
                    return {};
            return u;
        };
        auto rangeOf = [](const juce::var& entry, float& lo, float& hi) -> bool
        {
            auto anchors = echojay::anchorsFromVar(entry);
            if (anchors.isEmpty()) return false;
            auto eff = echojay::dominantMonotonicTable(anchors);
            if (! eff.ok) return false;
            lo = hi = eff.table.getFirst()[0];
            for (auto& a : eff.table) { lo = juce::jmin(lo, a[0]); hi = juce::jmax(hi, a[0]); }
            return hi - lo > 1.0e-6f;
        };
        auto num = [](float v)
        {
            return std::abs(v - std::round(v)) < 1.0e-4f
                       ? juce::String((int) std::lround(v)) : juce::String(v, 2);
        };
        const auto arrow = juce::String::fromUTF8(" \xe2\x86\x92 ");
        juce::StringArray landedBits, askedBits, refusedBits;
        // Index beside each SUPPRESSIBLE entry, so injection-build time can ask
        // "does this control have a live read?" without inverting a lossy
        // label. Refused entries need none: they are never suppressed.
        juce::Array<int> landedIdx, askedIdx;
        for (auto& r : report)
        {
            // Range refusals name the mapped range ON THE CARD, inheriting
            // applyOne's note VERBATIM ("asked 50.00, this control's range
            // is [1.00 .. 7.00], left manual") rather than authoring a
            // second sentence for the same fact — and never a version
            // claim: the range is the MAP's, and updating the plugin would
            // not change it.
            if (r.outOfRange && r.note.isNotEmpty())
            {
                refusedBits.add(echojay::semanticLabel(r.semantic) + ": " + r.note);
                // REFUSED, and recorded as refused. landed=false is the whole
                // point: the model must be able to say "I tried and could
                // not" without that reading as a change it made.
                recordMove(slotIndex, s.desc.name, echojay::semanticLabel(r.semantic),
                           r.beforeText, r.note, juce::String(), /*landed*/ false);
                continue;
            }
            if (! r.applied || r.staleDisplayKept) continue;   // bridged: annotated upstream
            const auto label = echojay::semanticLabel(r.semantic);
            // AN APPROXIMATE VALUE IS NOT A LANDING, whatever the display
            // says. displayVerified means the knob shows what we wrote; on a
            // product fallback the unverified part is whether what we wrote
            // corresponds to what was ASKED, because the anchor mapping came
            // from another version. So the annotation rides the entry rather
            // than the tier: the tier still says how the write went, and the
            // suffix says the number cannot be trusted to the request.
            const juce::String approx = r.anchorsUnverified
                ? juce::String(" (approximate, mapped from ")
                    + (s.dialServedFrom.isNotEmpty() ? s.dialServedFrom : juce::String("another version"))
                    + ")"
                : juce::String();
            if (r.displayVerified && r.landedText.trim().isNotEmpty())
            {
                landedBits.add(label + arrow + "reads \"" + r.landedText.trim() + "\"" + approx);
                landedIdx.add(r.index);
                // LANDED, and only here. beforeText is read inside applyOne
                // immediately before the write, so it is the control's real
                // prior value rather than a sweep that may predate the slot.
                recordMove(slotIndex, s.desc.name, label,
                           r.beforeText, r.landedText.trim(), juce::String(), /*landed*/ true);
                continue;
            }
            // NO VERIFIED LANDING FOR THIS CONTROL. It goes on the asked line,
            // never the landed one: the whole point of 6a is that the reader
            // can tell the two apart, and putting a request behind the same
            // arrow as a landing is the original defect wearing a new name.
            const auto entry = entryFor(r.semantic);
            const auto unit  = declaredUnit(entry, r.semantic);
            float lo = 0.0f, hi = 0.0f;
            if (unit.isNotEmpty())
                { askedBits.add(label + arrow + r.requestedValue.toString() + " " + unit + approx); askedIdx.add(r.index); }
            else if (rangeOf(entry, lo, hi))
                { askedBits.add(label + arrow + r.requestedValue.toString()
                              + " (this knob runs " + num(lo) + ".." + num(hi) + ")" + approx); askedIdx.add(r.index); }
            else
                { askedBits.add(label + arrow + r.requestedValue.toString() + approx); askedIdx.add(r.index); }
        }
        if (! landedBits.isEmpty() || ! askedBits.isEmpty() || ! refusedBits.isEmpty())
        {
            // Idempotent on re-apply (map arrival, re-dial): previous
            // Landed/Asked/Refused lines are replaced, never stacked. The
            // asked prefix MUST be stripped here too, or a re-dial stacks a
            // second unverified line under the first.
            juce::StringArray kept;
            for (auto& line : juce::StringArray::fromLines(s.settings))
                if (! line.startsWith(kLandedPrefix) && ! line.startsWith(kAskedPrefix)
                    && ! line.startsWith(kRefusedPrefix))
                    kept.add(line);
            while (! kept.isEmpty() && kept[kept.size() - 1].trim().isEmpty())
                kept.remove(kept.size() - 1);
            // ONE PARTITION, BUILT ONCE, CONSUMED TWICE. landedBits /
            // askedBits / refusedBits are decided exactly once above; these
            // three lines are composed exactly once here; and both consumers
            // below take THESE lines. Nothing downstream re-decides what counts
            // as landed versus asked, which is the whole point of the split.
            juce::StringArray tierLines;
            if (! landedBits.isEmpty())
                tierLines.add(kLandedPrefix + landedBits.joinIntoString(", "));
            if (! askedBits.isEmpty())
                tierLines.add(kAskedPrefix + askedBits.joinIntoString(", "));
            if (! refusedBits.isEmpty())
                tierLines.add(kRefusedPrefix + refusedBits.joinIntoString("; "));
            kept.addArray(tierLines);
            // The CARD keeps the prose. Unchanged from before the split,
            // including the fact that the "Applied automatically" writer below
            // overwrites it whenever anything applied, so this assignment is
            // observable only on the nothing-applied path (refusals only).
            s.settings = kept.joinIntoString("\n");
            // THE MODEL GETS THE TIERS ONLY, NOT THE PROSE (24 Aug 2026).
            // `kept` opens with whatever setSlotSettings wrote, which is the
            // model's own suggested-settings description. Handing that back
            // inside a field about control values is the same conflation this
            // whole contract removes: description read as state.
            //
            // STORED STRUCTURED, NOT COMPOSED. The entries stay apart until
            // modelSettingsForSlot() assembles them at injection-build time,
            // because that is the only moment both the echo and the live reads
            // are in hand. Composing here and re-splitting there would be the
            // packed-string escaping problem again.
            s.modelLandedBits  = landedBits;   s.modelLandedIdx = landedIdx;
            s.modelAskedBits   = askedBits;    s.modelAskedIdx  = askedIdx;
            s.modelRefusedBits = refusedBits;
            if (onSlotSettingsChanged) onSlotSettingsChanged();
        }
    }

    // SUGGESTED SETTINGS display contract: auto-applied slots show
    // "Applied automatically" + a compact summary of what was set;
    // slots with nothing applied keep the prose guidance unchanged.
    if (!appliedSummary.isEmpty())
    {
        s.settings = "Applied automatically\n" + appliedSummary.joinIntoString(", ");
        if (!s.dialUnconfirmed.isEmpty())
            s.settings += "\n" + s.dialUnconfirmed.joinIntoString(", ")
                        + ": written - display could not be confirmed on this bridged plugin";
        // Approximate values get their own line, for the same reason the
        // bridged caveat has one: the write succeeded and the number is not
        // one we can stand behind. Naming the version it was mapped FROM is
        // the whole point -- it tells the reader why, and that dialling by
        // hand is the remedy.
        if (!s.dialApproximate.isEmpty())
            s.settings += "\n" + s.dialApproximate.joinIntoString(", ")
                        + ": approximate - mapped from "
                        + (s.dialServedFrom.isNotEmpty() ? s.dialServedFrom
                                                         : juce::String("another version"))
                        + ", dial by hand if it matters";
    }
}

void ChainHost::loadParamMapsFromDisk()
{
    auto f = getParamMapsCacheFile();
    if (!f.existsAsFile()) return;
    auto root = juce::JSON::parse(f.loadFileAsString());
    if (auto* idx = root.getProperty("identityToFp", juce::var()).getDynamicObject())
        for (auto& p : idx->getProperties())
            identityToFp_[p.name.toString()] = p.value.toString();
    if (auto* maps = root.getProperty("maps", juce::var()).getDynamicObject())
        for (auto& p : maps->getProperties())
            if (p.value.getDynamicObject() != nullptr)
                paramMaps_[p.name.toString()] = p.value;
    if (auto* att = root.getProperty("fpAttempted", juce::var()).getArray())
        for (auto& v : *att)
            fpAttempted_.addIfNotAlreadyThere(v.toString());
    if (auto* fa = root.getProperty("fpFetchedAt", juce::var()).getDynamicObject())
        for (auto& p : fa->getProperties())
            fpFetchedAt_[p.name.toString()] = (juce::int64)(double) p.value;
    EchoJay_NSLog(("EJParamMaps: cache loaded, " + juce::String((int)identityToFp_.size())
                   + " identities, " + juce::String((int)paramMaps_.size()) + " map(s), "
                   + juce::String(fpAttempted_.size()) + " fp skip marker(s)").toRawUTF8());
}

void ChainHost::saveParamMapsToDisk()
{    // Borrowed mode is READ-ONLY on every shared file (spec §2.2): one
    // writer per file, and the primary host is it.
    if (mode_ == Mode::Borrowed) return;

    juce::DynamicObject::Ptr idx = new juce::DynamicObject();
    for (auto& kv : identityToFp_) idx->setProperty(juce::Identifier(kv.first), kv.second);
    juce::DynamicObject::Ptr maps = new juce::DynamicObject();
    for (auto& kv : paramMaps_) maps->setProperty(juce::Identifier(kv.first), kv.second);
    juce::var att;
    for (auto& s : fpAttempted_) att.append(s);
    juce::DynamicObject::Ptr fetchedAt = new juce::DynamicObject();
    for (auto& kv : fpFetchedAt_) fetchedAt->setProperty(juce::Identifier(kv.first), (double) kv.second);
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("identityToFp", juce::var(idx.get()));
    root->setProperty("maps", juce::var(maps.get()));
    root->setProperty("fpAttempted", att);
    root->setProperty("fpFetchedAt", juce::var(fetchedAt.get()));
    getParamMapsCacheFile().replaceWithText(juce::JSON::toString(juce::var(root.get())));
}

// ---------------------------------------------------------------------------
// Fingerprint pass (scan path). See the header comment for the contract.
// ---------------------------------------------------------------------------
void ChainHost::startFingerprintPass()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (fpPassRunning_.load() || scanning_.load()) return;

    fpQueue_.clear();
    {
        std::lock_guard<std::mutex> lk(pluginsMutex_);
        for (const auto& d : entries_)
        {
            if (d.isInstrument) continue;                       // never a chain slot
            // Withheld rows are never instantiated here: this pass creates
            // the plugin directly (asyncCreatePlugin), not through the
            // loadPluginAsync gate, and a crash-blacklisted row now stays in
            // entries_ after a rescan (WithholdReason, header). Skipping the
            // architecture-incompatible ones as well only saves a load that
            // would fail with "No types found". pluginsMutex_ is held.
            if (isWithheld(withholdReasonLocked(d))) continue;
            // Thin VST3 (unvalidated, version empty): fingerprinting now
            // would hash a wrong basis. completeLoad covers it on first load.
            if (d.pluginFormatName == "VST3" && d.version.isEmpty()) continue;
            const auto ik = echojay::identityKeyForDescription(d);
            if (identityToFp_.find(ik) != identityToFp_.end()) continue;
            if (fpAttempted_.contains(ik)) continue;
            fpQueue_.add(d);
        }
    }
    if (fpQueue_.isEmpty()) return;

    fpQueueTotal_ = fpQueue_.size();
    fpPassRunning_.store(true);
    EchoJay_NSLog(("EJFpPass: start, " + juce::String(fpQueueTotal_)
                   + " plugin(s) to fingerprint").toRawUTF8());
    fingerprintNext();
}

void ChainHost::fingerprintNext()
{
    if (fpQueue_.isEmpty())
    {
        fpPassRunning_.store(false);
        saveParamMapsToDisk();
        EchoJay_NSLog(("EJFpPass: done, index now "
                       + juce::String((int)identityToFp_.size()) + " identities").toRawUTF8());
        // Fetch maps for everything the pass just fingerprinted (batched
        // <=500, cached in memory + param_maps.json by storeParamMaps). If
        // no editor is attached right now, requestMapPrefetch no-ops WITHOUT
        // marking anything requested, so the resolver-rebuild prefetch picks
        // these fps up as soon as an editor exists.
        requestMapPrefetch();
        return;
    }

    const auto desc = fpQueue_.removeAndReturn(0);
    const auto ik   = echojay::identityKeyForDescription(desc);
    const int  n    = fpQueueTotal_ - fpQueue_.size();

    // Death marker BEFORE the load: a plugin that crashes the host during
    // instantiation is silently skipped on every later pass instead of
    // crash-looping the scan. Cleared below when the load survives.
    fpAttempted_.addIfNotAlreadyThere(ik);
    saveParamMapsToDisk();

    asyncCreatePlugin(desc,
        [this, desc, ik, n](std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
        {
            if (inst != nullptr)
            {
                fpAttempted_.removeString(ik);
                const auto fp = echojay::fingerprintForDescription(
                    desc, (int) inst->getParameters().size());
                identityToFp_[ik] = fp;
                EchoJay_NSLog(("EJFpPass: (" + juce::String(n) + "/"
                               + juce::String(fpQueueTotal_) + ") " + desc.name
                               + " -> " + fp.substring(0, 12)).toRawUTF8());
                // The instance is up anyway: measure its default-state size,
                // so a plugin whose settings can never be saved is withheld
                // before it reaches the picker (17 Aug 2026). Default state
                // only: a sampler grows with the content the user loads, and
                // that growth is reported by the capture note at rack time.
                {
                    juce::MemoryBlock st;
                    try { inst->getStateInformation(st); } catch (...) { st.reset(); }
                    if ((int) st.getSize() > kSessionStateMaxSlotBytes)
                        recordStateOversize(desc.fileOrIdentifier, (int) st.getSize(), desc.name, "fingerprint pass");
                }
                inst.reset();   // release immediately; the instance was only for param_count
            }
            else
            {
                // Failed loads keep their marker: no point retrying every scan.
                // NOTE: deliberately NOT fed into the session load-failure
                // set — a fingerprint-pass failure with the iLok unplugged
                // would suppress owned plugins for the whole session.
                EchoJay_NSLog(("EJFpPass: (" + juce::String(n) + "/"
                               + juce::String(fpQueueTotal_) + ") " + desc.name
                               + " FAILED: " + err).toRawUTF8());
            }
            // Unwind before the next load so the message thread breathes
            // between instantiations.
            juce::MessageManager::callAsync([this] { fingerprintNext(); });
        });
}

// Forward declaration for mutual recursion with the polling lambda
static void pollVST3Validation(
    ChainHost* host,
    std::shared_ptr<struct VST3ValState> vs,
    juce::PluginDescription desc,
    int validationMark,
    ChainHost::LoadOrigin origin,
    std::function<void(const juce::String&)> cb,
    int ticksLeft);

struct VST3ValState {
    std::atomic<bool> done { false };
    std::mutex mtx;
    juce::Array<juce::PluginDescription> results;
};

static void pollVST3Validation(
    ChainHost* host,
    std::shared_ptr<VST3ValState> vs,
    juce::PluginDescription desc,
    int validationMark,
    ChainHost::LoadOrigin origin,
    std::function<void(const juce::String&)> cb,
    int ticksLeft)
{
    if (vs->done.load())
    {
        popDeathMark(validationMark);
        juce::PluginDescription fullDesc;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(vs->mtx);
            if (!vs->results.isEmpty()) { fullDesc = vs->results[0]; found = true; }
        }
        if (!found) { cb("No types found in " + desc.name); return; }

        host->saveToDisk();
        host->asyncCreatePlugin(fullDesc,
            [host, cb, fullDesc, origin](std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
            {
                if (!inst) { cb(err.isNotEmpty() ? err : "createPluginInstance failed"); return; }
                host->completeLoad(std::move(inst), fullDesc, origin);
                cb({});
            });
        return;
    }

    if (ticksLeft <= 0)
    {
        // Blacklisted here and now; the mark comes down so the next launch
        // does not record the same event a second time under another reason.
        popDeathMark(validationMark);
        host->addToBlacklist(desc.fileOrIdentifier, "validation timed out");
        cb("Timed out loading \"" + desc.name + "\": added to skip list");
        return;
    }

    juce::Timer::callAfterDelay(100, [host, vs, desc, validationMark, origin, cb, ticksLeft]() mutable {
        pollVST3Validation(host, vs, desc, validationMark, origin, cb, ticksLeft - 1);
    });
}

// ---------------------------------------------------------------------------
// Borrow pool (Borrowed mode only) — RACK_BORROW_IMPLEMENTATION_SPEC §1.
// ---------------------------------------------------------------------------
int ChainHost::hostReportableLatencySamples() const
{
    if (mode_ == Mode::Borrowed)
    {
        // THE HARD BLOCK (spec §2.3): a borrowed chain's latency must never
        // reach setLatencySamples — the host would delay the main's channel
        // by latency it does not experience. Refused, loudly, not advisory.
        EchoJay_NSLog("EJBorrow: latency mirror REFUSED - a borrowed host "
                      "never reaches setLatencySamples");
        return -1;
    }
    return getTotalLatencySamples();
}

void ChainHost::markBorrowPoolIneligible(const juce::PluginDescription& d,
                                         const juce::String& why)
{
    const auto key = borrowPoolKey(d);
    if (borrowPoolIneligible_.contains(key)) return;
    borrowPoolIneligible_.add(key);
    // The named fallback, said out loud (spec §1): this plugin pays today's
    // per-cycle cost for ITSELF only; everything else keeps the bound.
    EchoJay_NSLog(("EJBorrowPool: \"" + d.name + "\" failed reuse verification ("
                   + why + ") - pool-ineligible this session, later borrows "
                   "instantiate fresh").toRawUTF8());
}

void ChainHost::releaseBorrowToPool()
{
    if (mode_ != Mode::Borrowed || !graph_) return;
    for (int i = 0; i < (int) slots_.size(); ++i)
    {
        auto& s = slots_[(size_t) i];
        if (s.node == nullptr) continue;
        // Same discipline as removeSlot: no listener may outlive its slot.
        detachHostedListener(i);
        // Park IN the graph, disconnected (rebuildGraph below only wires
        // slots_) and SUSPENDED — the graph skips suspended nodes, so a
        // parked rack costs one flag check per node per block, not audio.
        if (auto* p = s.node->getProcessor()) p->suspendProcessing(true);
        borrowPool_[borrowPoolKey(s.desc)].push_back({ s.node, s.desc });
        ++borrowPoolTotal_;
        // The wet-blend node is OURS — destroy for real, as removeSlot does.
        if (s.blendNode) graph_->removeNode(s.blendNode->nodeID);
    }
    slots_.clear();
    borrowReusedNodeIds_.clear();
    borrowSeededNodeIds_.clear();
    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
    EchoJay_NSLog(("EJBorrowPool: rack released, " + juce::String((int) borrowPoolTotal_)
                   + " instance(s) parked").toRawUTF8());
}

#if ECHOJAY_DEV_TRANSPORT
namespace {
// DEV-ONLY forced withhold (22 Aug 2026): on this machine the real withhold
// cannot be produced by hand — same plugin collection, identity always
// matches — so ~/.echojay/dev.json may name ONE slot (1-based, key
// "forceWithholdSlot") whose borrow seed is deliberately failed, exercising
// the user-facing withheld path in a DAW: defaults + banner + never written
// back. Read once per process, like the dev transport itself. This symbol
// CANNOT exist in a non-DEV build — the whole block compiles away, and
// borrowhost_test proves the OFF binary carries no trace of it.
int devForceWithholdSlot1()
{
    static const int s = []
    {
        auto f = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile(".echojay").getChildFile("dev.json");
        if (! f.existsAsFile()) return 0;
        auto v = juce::JSON::parse(f.loadFileAsString());
        if (auto* o = v.getDynamicObject())
            return (int) o->getProperty("forceWithholdSlot");
        return 0;
    }();
    return s;
}
} // namespace
#endif

// ===========================================================================
//  Structure plan engine, phase 2 (RACK_STRUCTURE_EDIT_SPEC). Runs on the
//  LINK's Primary host, message thread. Staging and removal parks share one
//  container keyed by the PLAN's identity vocabulary (name|uid-decimal), and
//  everything parked stays alive — §3a everywhere.
// ===========================================================================
namespace
{
    juce::String planKeyOf(const LinkShm::StructureEdit::SlotIdentity& id)
    { return id.name.trim() + "|" + id.uid; }
}

std::vector<LinkShm::StructureEdit::SlotIdentity> ChainHost::liveIdentity() const
{
    std::vector<LinkShm::StructureEdit::SlotIdentity> out;
    for (const auto& s : slots_)
        out.push_back({ s.desc.name,
                        descUid(s.desc) != 0 ? juce::String(descUid(s.desc))
                                             : juce::String(),
                        s.fp });
    return out;
}

LinkShm::StructureEdit::PreImages ChainHost::planCapturePreImages() const
{
    LinkShm::StructureEdit::PreImages pre;
    pre.shape = liveIdentity();
    for (const auto& s : slots_)
    {
        juce::String b64;
        if (s.node != nullptr)
            if (auto* p = s.node->getProcessor())
            {
                juce::MemoryBlock mb;
                try { p->getStateInformation(mb); } catch (...) { mb.reset(); }
                if (mb.getSize() > 0) b64 = LinkShm::stateToB64(mb);
            }
        pre.states.add(b64);
    }
    return pre;
}

void ChainHost::parkSlotReattachable(int i)
{
    if (i < 0 || i >= (int) slots_.size() || !graph_) return;
    auto& s = slots_[(size_t) i];
    detachHostedListener(i);
    if (s.node != nullptr)
    {
        if (auto* p = s.node->getProcessor()) p->suspendProcessing(true);
        planPark_[planKeyOf({ s.desc.name,
                              descUid(s.desc) != 0 ? juce::String(descUid(s.desc))
                                                   : juce::String(), s.fp })]
            .push_back({ s.node, s.desc });
    }
    if (s.blendNode) graph_->removeNode(s.blendNode->nodeID);
    slots_.erase(slots_.begin() + i);
    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
}

bool ChainHost::tryReattachParked(const juce::PluginDescription& d, int insertAt)
{
    const auto key = planKeyOf({ d.name,
                                 descUid(d) != 0 ? juce::String(descUid(d))
                                                 : juce::String(), {} });
    auto it = planPark_.find(key);
    if (it == planPark_.end() || it->second.empty()) return false;
    BorrowPoolEntry entry = std::move(it->second.back());
    it->second.pop_back();
    if (auto* p = entry.node->getProcessor())
    {
        p->suspendProcessing(false);
        p->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
    }
    ChainSlot slot;
    slot.node = std::move(entry.node);
    slot.desc = entry.desc;
    // The stored desc is the REQUEST; the INSTANCE knows its true identity.
    // Backfill what the request lacked — restore paths request by name+uid
    // with a blank manufacturer, fresh adds carry the full catalogue desc —
    // so every reader (editorPlacement's float-by-identity arm above all)
    // sees the same truth regardless of arrival path (2 Sep 2026: a
    // restored Waves slot reached the placement with mfr="" and embedded
    // blank while an added one floated).
    if (slot.desc.manufacturerName.isEmpty() && slot.node != nullptr)
        if (auto* pi = dynamic_cast<juce::AudioPluginInstance*>(slot.node->getProcessor()))
            slot.desc.manufacturerName = pi->getPluginDescription().manufacturerName;
    EchoJay_NSLog(("EJPlace: stored \"" + slot.desc.name + "\" mfr=\""
                   + slot.desc.manufacturerName + "\" (plan attach)").toRawUTF8());
    slot.bypassed = false;
    insertAt = juce::jlimit(0, (int) slots_.size(), insertAt);
    slots_.insert(slots_.begin() + insertAt, std::move(slot));
    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
    return true;
}

bool ChainHost::planStageOne(const juce::PluginDescription& d, juce::String& whyNot,
                             int alreadyClaimed, const juce::String& parkKeyIn)
{
    // Already staged/parked by identity: a retried Apply instantiates zero
    // new (spec amendment 3) — but only BEYOND what this plan has already
    // claimed of the same key, or a plan creating two of one plugin gets
    // one instance and Phase B finds the park empty for the second.
    // parkKeyIn: the caller's identity-vocabulary key (Phase A) — when
    // given it OVERRIDES the derived key, keeping park and reattach
    // symmetric whatever the staging desc resolved to.
    const auto key = parkKeyIn.isNotEmpty() ? parkKeyIn
        : planKeyOf({ d.name,
                      descUid(d) != 0 ? juce::String(descUid(d))
                                      : juce::String(), {} });
    if (auto it = planPark_.find(key); it != planPark_.end()
        && (int) it->second.size() > alreadyClaimed)
        return true;
    if (d.fileOrIdentifier.isNotEmpty() && isBlacklisted(d.fileOrIdentifier))
    { whyNot = d.name + " is on this machine's crash skip list"; return false; }

    // The park stays keyed by the IDENTITY (what the plan speaks); the
    // slot's desc becomes whatever the catalogue resolves — the format the
    // stateFormat honesty guard then compares against.
    juce::PluginDescription staged = d;
    std::unique_ptr<juce::AudioProcessor> proc;
    if (isBuiltinDescription(d))
    {
        if (const auto* dev = BuiltinDeviceRegistry::instance().findForDescription(d);
            dev != nullptr && dev->create)
            proc = dev->create();
        if (proc == nullptr)
        { whyNot = d.name + " is a built-in this build does not carry"; return false; }
    }
    else
    {
        // RESOLVE AGAINST THIS CATALOGUE (25 Aug 2026): a Create identity
        // carries name + uid, no format — and JUCE's createPluginInstance
        // requires an exact format-name match, so a bare identity could
        // NEVER load ("No compatible plug-in format", every third-party
        // Create; every builtin worked because builtins skip this arm).
        // The Link resolves against its OWN catalogue and chooses the
        // format — the picked-plugin principle, decided on the side that
        // has to load it. Tiers: name+uid, then uid alone, then name alone
        // (uid spaces differ across formats, so a name-only hit is the
        // same plugin in another build). The lookup logs BOTH encodings.
        juce::PluginDescription resolved;
        int nameHits = 0, uidHits = 0, hexHits = 0;
        const int wantUid = descUid(d);
        const juce::String wantHex = juce::String::toHexString(wantUid);
        int klN = 0, enN = 0;
        {
            std::lock_guard<std::mutex> lock(pluginsMutex_);
            klN = knownPlugins_.getNumTypes();
            enN = entries_.size();
            juce::PluginDescription byBoth, byUid, byName;
            auto scan = [&](const juce::PluginDescription& e)
            {
                const int eu = descUid(e);
                const bool nm = namesMatchLoose(e.name, d.name);
                const bool um = wantUid != 0 && eu == wantUid;
                if (nm) ++nameHits;
                if (um) ++uidHits;
                if (wantUid != 0
                    && juce::String::toHexString(eu).equalsIgnoreCase(wantHex))
                    ++hexHits;
                if (nm && um && byBoth.name.isEmpty()) byBoth = e;
                else if (um && byUid.name.isEmpty())   byUid  = e;
                else if (nm)
                {
                    // CHANNEL VARIANT AT THE NAME TIER (1 Sep 2026, merging the
                    // reasoning line in). WaveShell registers one AU per channel
                    // configuration and namesMatchLoose strips the suffix, so
                    // "CLA-76" matches CLA-76 (m) AND (s); first-wins took
                    // whichever was scanned first, which is the mono build. That
                    // is the defect 28d3f53 closed in the resolver ladder,
                    // arriving here by a route that did not exist when the six
                    // paths were inventoried. Same rank, same header.
                    //
                    // THE RANK SITS UNDER AN EXACT NAME, NEVER OVER IT. A saved
                    // or borrowed rack carries desc.name, the full registration,
                    // so a plan identity naming "CLA-76 (m)" asked for the mono
                    // build and must keep it; namesMatchLoose would otherwise let
                    // the rank re-point it to (s).
                    //
                    // ONLY THIS TIER. byBoth and byUid are pinned by uid, and
                    // each variant carries its own (Abbey Road Chambers (m)
                    // 59527463, (m->s) 59527476, (s) 5952747d), so where a uid
                    // matched the variant was already chosen by whoever authored
                    // the identity and the rank must not second-guess it.
                    // THE RULE ITSELF IS IN THE HEADER, called not copied, so
                    // the three behaviours above are pinned by calling it.
                    if (echojay::channelVariantShouldReplace(e.name, byName.name, d.name))
                        byName = e;
                }
            };
            for (const auto& e : knownPlugins_.getTypes()) scan(e);
            for (const auto& e : entries_) scan(e);
            resolved = byBoth.name.isNotEmpty() ? byBoth
                     : byUid.name.isNotEmpty()  ? byUid : byName;
        }
        EchoJay_NSLog(("EJPlan: stage lookup \"" + d.name + "\""
            + " uid.dec=" + juce::String(d.uniqueId)
            + " depUid.dec=" + juce::String(d.deprecatedUid)
            + " uid.hex=" + wantHex
            + " catalogue known=" + juce::String(klN)
            + " entries=" + juce::String(enN)
            + " nameMatches=" + juce::String(nameHits)
            + " uidMatches=" + juce::String(uidHits)
            + " hexMatches=" + juce::String(hexHits)
            + (resolved.name.isEmpty()
                   ? juce::String(" -> NOT IN CATALOGUE")
                   : " -> resolved " + resolved.name + "/"
                     + resolved.pluginFormatName + "/uid.dec="
                     + juce::String(descUid(resolved)) + "/uid.hex="
                     + juce::String::toHexString(descUid(resolved))))
            .toRawUTF8());
        if (resolved.name.isEmpty())
        {
            // The REAL missing-plugin case (a rack on a machine without
            // it) reads as exactly that — never as a format error.
            whyNot = "this Link doesn't have " + d.name
                   + " - it is not in its plug-in list";
            return false;
        }
        if (resolved.fileOrIdentifier.isNotEmpty()
            && isBlacklisted(resolved.fileOrIdentifier))
        { whyNot = resolved.name + " is on this machine's crash skip list";
          return false; }
        const int mark = pushDeathMark("plan stage", resolved);
        juce::String err;
        auto inst = formatManager_.createPluginInstance(resolved,
                                                        sampleRate_ > 0 ? sampleRate_ : 44100.0,
                                                        blockSize_ > 0 ? blockSize_ : 512, err);
        popDeathMark(mark);
        if (inst == nullptr)
        { whyNot = d.name + " could not load right now ("
                 + (err.isNotEmpty() ? err : juce::String("no reason given")) + ")";
          return false; }
        staged = resolved;
        proc = std::move(inst);
    }
    proc->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
    proc->suspendProcessing(true);                    // detached: parked, silent
    auto node = graph_ ? graph_->addNode(std::move(proc)) : nullptr;
    if (node == nullptr) { whyNot = d.name + " could not join the graph"; return false; }
    planPark_[key].push_back({ node, staged });
    ++planFresh_;
    return true;
}

void ChainHost::planRestoreFromPreImages(const LinkShm::StructureEdit::PreImages& pre)
{
    // Wholesale: park everything, rebuild the pre-image shape from the park
    // (nothing was freed, so every instance is findable), reseed pre states.
    for (int i = (int) slots_.size() - 1; i >= 0; --i)
        parkSlotReattachable(i);
    for (int i = 0; i < (int) pre.shape.size(); ++i)
    {
        juce::PluginDescription d;
        d.name = pre.shape[(size_t) i].name;
        d.uniqueId = pre.shape[(size_t) i].uid.getIntValue();
        if (! tryReattachParked(d, i))
        {
            // Not parked — the LAUNCH restore path: after a crash nothing
            // is parked, so the restore instantiates from identity and
            // reseeds. Only a plugin gone from the machine is a lost slot,
            // and that is said out loud.
            juce::String why;
            juce::PluginDescription rd = resolveByName(d.name, {});
            if (rd.name.isEmpty()) rd = d;
            if (! planStageOne(rd, why) || ! tryReattachParked(rd, i))
            {
                addStateNote(d.name + ": could not be restored after the "
                             "interrupted restructure ("
                             + (why.isNotEmpty() ? why : juce::String("not loadable"))
                             + ") - this slot was lost");
                EchoJay_NSLog(("EJPlan: restore could not rebuild \""
                               + d.name + "\" - slot lost").toRawUTF8());
                continue;
            }
        }
        const auto& b64 = pre.states[i];
        if (b64.isNotEmpty() && i < (int) slots_.size())
            if (auto* p = getSlotProcessor(i))
            {
                juce::MemoryBlock mb;
                if (LinkShm::stateFromB64(b64, mb) && mb.getSize() > 0)
                { try { p->setStateInformation(mb.getData(), (int) mb.getSize()); }
                  catch (...) {} }
            }
    }
}

ChainHost::PlanResult ChainHost::applyStructurePlan(
    const juce::String& journalDir, const LinkShm::StructureEdit::Plan& plan)
{
    using namespace LinkShm::StructureEdit;
    PlanResult r;

    // The identity guard, first and absolute (spec §5).
    if (verifyBaseIdentity(plan.baseIdentity, liveIdentity()) != BaseCheck::Match)
    { r.failedAt = "base identity"; r.reasons.add(
          "the rack is not the one this plan was made for - nothing was changed");
      return r; }

    // PHASE A: stage every Create, detached. Any failure aborts with ZERO
    // mutations and per-slot named reasons (spec §2). claimed counts per
    // key so a plan creating TWO of one plugin stages two instances — the
    // retry-dedupe inside planStageOne only skips beyond this plan's claims.
    std::map<juce::String, int> claimed;
    for (const auto& op : plan.ops)
        if (op.type == OpType::Create)
        {
            juce::PluginDescription idDesc;
            idDesc.name = op.identity.name;
            idDesc.uniqueId = op.identity.uid.getIntValue();
            // THE PARK KEY IS THE PLAN'S IDENTITY VOCABULARY — the same key
            // Phase B's reattach derives from op.identity. Staging may
            // resolve to a richer desc (builtin registry, catalogue), but
            // the key NEVER follows the resolution (27 Aug: a name-only
            // identity pre-resolved to a builtin's uid-ful desc, Phase A
            // parked under name|uid, Phase B looked up name| — "staged
            // instance vanished", whole-plan rollback).
            const auto key = planKeyOf({ idDesc.name,
                                         descUid(idDesc) != 0
                                             ? juce::String(descUid(idDesc))
                                             : juce::String(), {} });
            auto stageDesc = idDesc;
            if (isBuiltinDescription(resolveByName(op.name, {})))
                stageDesc = resolveByName(op.name, {});
            juce::String why;
            if (! planStageOne(stageDesc, why, claimed[key], key))
                r.reasons.add(why);
            else
                ++claimed[key];
        }
    if (! r.reasons.isEmpty()) { r.failedAt = "stage"; return r; }

    // Journal BEFORE the first mutation (spec §1).
    const auto pre = planCapturePreImages();
    writeJournal(journalDir, plan, pre);

    // PHASE B: mutate, in the plan's order. Any failure restores wholesale.
    // originSim mirrors every mutation so the caller learns, per FINAL slot,
    // which pre-plan index it came from (-1 = created) — the lease's prior
    // remap maps through it and cannot be fooled by shifted indices.
    std::vector<int> originSim;
    for (int i = 0; i < (int) slots_.size(); ++i) originSim.push_back(i);
    for (const auto& op : plan.ops)
    {
        bool ok = true;
        juce::String why;
        switch (op.type)
        {
            case OpType::Remove:
                if (op.from >= 0 && op.from < (int) slots_.size())
                {
                    parkSlotReattachable(op.from);
                    originSim.erase(originSim.begin() + op.from);
                }
                else { ok = false; why = "remove index out of range"; }
                break;
            case OpType::Move:
            {
                int cur = op.from;
                while (ok && cur > op.to)
                { moveSlot(cur, -1);
                  std::swap(originSim[(size_t) cur], originSim[(size_t) cur - 1]);
                  --cur; }
                while (ok && cur < op.to)
                { moveSlot(cur, +1);
                  std::swap(originSim[(size_t) cur], originSim[(size_t) cur + 1]);
                  ++cur; }
                break;
            }
            case OpType::Create:
            {
                juce::PluginDescription d;
                d.name = op.identity.name;
                d.uniqueId = op.identity.uid.getIntValue();
                if (! tryReattachParked(d, op.to))
                { ok = false; why = "staged instance vanished"; break; }
                originSim.insert(originSim.begin()
                                     + juce::jmin(op.to, (int) originSim.size()),
                                 -1);
                // The plan's bypass truth for the created slot (the state the
                // user gave it in the main); under a rack lease the caller
                // reads it into the remapped priors, then re-bypasses.
                setSlotBypassed(juce::jmin(op.to, (int) slots_.size() - 1),
                                op.bypassed);
                if (op.stateB64.isNotEmpty())
                {
                    const int at = juce::jmin(op.to, (int) slots_.size() - 1);
                    // §5c ACROSS FORMATS (24 Aug 2026): plugin state is
                    // format-specific, and the main may have hosted a
                    // SUBSTITUTE build — its blob must never seed a
                    // different format's build here. Mismatch = the slot
                    // arrives at DEFAULTS and SAYS so, in the withheld
                    // voice; only a matching (or unstated) format seeds.
                    const auto stagedFmt = getSlotInfo(at).format;
                    if (op.stateFormat.isNotEmpty()
                        && op.stateFormat != stagedFmt)
                    {
                        addStateNote(op.name + " was added WITHOUT its "
                            "settings (running at defaults): they came from "
                            "a " + op.stateFormat + " build and this rack "
                            "loads the " + stagedFmt + " build - settings "
                            "do not travel across formats");
                        EchoJay_NSLog(("EJPlan: Create \"" + op.name
                            + "\" seeded at DEFAULTS - state format "
                            + op.stateFormat + " != staged " + stagedFmt)
                            .toRawUTF8());
                    }
                    else if (auto* p = getSlotProcessor(at))
                    {
                        juce::MemoryBlock mb;
                        if (LinkShm::stateFromB64(op.stateB64, mb) && mb.getSize() > 0)
                        { try { p->setStateInformation(mb.getData(), (int) mb.getSize()); }
                          catch (...) { ok = false; why = op.name + " refused its seed"; } }
                    }
                }
                break;
            }
            case OpType::Commit:
            {
                auto* p = getSlotProcessor(op.from);
                juce::MemoryBlock mb;
                if (p == nullptr || ! LinkShm::stateFromB64(op.stateB64, mb)
                    || mb.getSize() == 0)
                { ok = false; why = op.name + ": commit state unreadable"; break; }
                const int mark = pushDeathMark("state restore",
                                               slots_[(size_t) op.from].desc);
                try { p->setStateInformation(mb.getData(), (int) mb.getSize()); }
                catch (...) { ok = false; why = op.name + " refused the settings"; }
                popDeathMark(mark);
                break;
            }
        }
        if (! ok)
        {
            planRestoreFromPreImages(pre);
            juce::File(journalPath(journalDir, plan.uid)).deleteFile();
            r.restored = true;
            r.failedAt = why;
            r.reasons.add(why + " - the rack was restored to exactly its "
                          "pre-Apply state");
            return r;
        }
    }
    juce::File(journalPath(journalDir, plan.uid)).deleteFile();
    r.ok = true;
    r.finalOrigin = std::move(originSim);
    return r;
}

bool ChainHost::planJournalRestoreIfPresent(const juce::String& journalDir,
                                            const juce::String& uid)
{
    using namespace LinkShm::StructureEdit;
    Plan p;
    PreImages pre;
    if (! readJournal(journalDir, uid, p, pre)) return false;
    // Divergence: the session snapshot lost to the journal — say so with
    // both truths named (spec amendment 2).
    const bool diverged =
        verifyBaseIdentity(pre.shape, liveIdentity()) != BaseCheck::Match;
    planRestoreFromPreImages(pre);
    juce::File(journalPath(journalDir, uid)).deleteFile();
    addStateNote(diverged
        ? juce::String("a restructure was interrupted; this rack was restored "
                       "from its pre-Apply state (the session had saved a "
                       "different shape)")
        : juce::String("a restructure was interrupted; this rack was restored "
                       "from its pre-Apply state"));
    EchoJay_NSLog(("EJPlan: journal restore ran (diverged="
                   + juce::String(diverged ? "Y" : "N") + ")").toRawUTF8());
    return true;
}

void ChainHost::captureBorrowDefaultState(int slotIdx)
{
    if (mode_ != Mode::Borrowed || slotIdx < 0
        || slotIdx >= (int) slots_.size() || slots_[(size_t) slotIdx].node == nullptr)
        return;
    if (auto* p = slots_[(size_t) slotIdx].node->getProcessor())
    {
        juce::MemoryBlock mb;
        try { p->getStateInformation(mb); } catch (...) { mb.reset(); }
        if (mb.getSize() > 0)
            borrowDefaultStates_[slots_[(size_t) slotIdx].node->nodeID.uid] = std::move(mb);
    }
}

bool ChainHost::borrowTryReuseInto(const juce::PluginDescription& canonicalDesc)
{
    if (mode_ != Mode::Borrowed) return false;
    const auto key = borrowPoolKey(canonicalDesc);
    if (borrowPoolIneligible_.contains(key)) return false;   // fresh, by verdict
    auto it = borrowPool_.find(key);
    if (it == borrowPool_.end() || it->second.empty()) return false;

    BorrowPoolEntry entry = std::move(it->second.back());
    it->second.pop_back();
    --borrowPoolTotal_;
    // RESET BEFORE SEEDING (22 Aug 2026): the parked instance still holds
    // the PREVIOUS borrow's settings, and a failed seed on top of those
    // would leave another channel's sound wearing this rack's name — worse
    // than defaults, because it sounds like a working chain. Reseed the
    // pristine default captured at fresh instantiation; an instance with no
    // default, or one that refuses it, is RETIRED (kept alive, never
    // reused) and the caller instantiates fresh.
    {
        auto* p = entry.node->getProcessor();
        auto dIt = p != nullptr
                     ? borrowDefaultStates_.find(entry.node->nodeID.uid)
                     : borrowDefaultStates_.end();
        bool resetOk = false;
        if (dIt != borrowDefaultStates_.end() && dIt->second.getSize() > 0)
        {
            try { p->setStateInformation(dIt->second.getData(),
                                         (int) dIt->second.getSize());
                  resetOk = true; }
            catch (...) {}
        }
        if (! resetOk)
        {
            EchoJay_NSLog(("EJBorrowPool: \"" + canonicalDesc.name + "\" could "
                           "not be reset to defaults - retired from the pool, "
                           "instantiating fresh").toRawUTF8());
            borrowPoolRetired_.push_back(std::move(entry));
            // Ineligible too: a reset-refuser would otherwise pay the
            // retire dance every cycle; this way later borrows go fresh
            // directly — today's per-cycle cost for ITSELF only.
            markBorrowPoolIneligible(canonicalDesc, "could not be reset to defaults");
            return false;
        }
    }
    if (auto* p = entry.node->getProcessor())
    {
        p->suspendProcessing(false);
        p->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
    }
    ChainSlot slot;
    slot.node = std::move(entry.node);
    slot.desc = entry.desc;
    // The stored desc is the REQUEST; the INSTANCE knows its true identity.
    // Backfill what the request lacked — restore paths request by name+uid
    // with a blank manufacturer, fresh adds carry the full catalogue desc —
    // so every reader (editorPlacement's float-by-identity arm above all)
    // sees the same truth regardless of arrival path (2 Sep 2026: a
    // restored Waves slot reached the placement with mfr="" and embedded
    // blank while an added one floated).
    if (slot.desc.manufacturerName.isEmpty() && slot.node != nullptr)
        if (auto* pi = dynamic_cast<juce::AudioPluginInstance*>(slot.node->getProcessor()))
            slot.desc.manufacturerName = pi->getPluginDescription().manufacturerName;
    EchoJay_NSLog(("EJPlace: stored \"" + slot.desc.name + "\" mfr=\""
                   + slot.desc.manufacturerName + "\" (plan attach)").toRawUTF8());
    slot.bypassed = false;
    slots_.push_back(std::move(slot));
    borrowReusedNodeIds_.insert(slots_.back().node->nodeID.uid);

    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
    const int idx = (int) slots_.size() - 1;
    applyStructuredIfReady(idx, DialTrigger::slotLoaded);
    if (stateCacheEnabled_)
    {
        attachHostedListener(idx);
        captureSlotState(idx, juce::Time::getMillisecondCounterHiRes());
    }
    EchoJay_NSLog(("EJBorrowPool: \"" + canonicalDesc.name + "\" REUSED from the "
                   "pool as slot " + juce::String(idx) + " (0 new instances)").toRawUTF8());
    return true;
}

juce::String ChainHost::loadBuiltinNow(const juce::PluginDescription& desc)
{
    if (!graph_) return "chain graph not ready";

    // Resolved from whatever the description carries — identifier, then uid, then
    // name — because a desc rebuilt from restore XML may be missing fields. One
    // lookup replaces what used to be a per-device if-chain.
    const auto* device = BuiltinDeviceRegistry::instance().findForDescription(desc);
    if (device == nullptr)
        return "unknown built-in device \"" + desc.name + "\"";

    // Borrowed mode: the pool first — a parked identical instance is reused
    // instead of constructing (spec §1: growth bounds at distinct identities).
    if (mode_ == Mode::Borrowed
        && borrowTryReuseInto(BuiltinDeviceRegistry::descriptionFor(*device)))
        return {};

    std::unique_ptr<juce::AudioProcessor> proc = device->create();
    if (!proc)
        return "built-in device \"" + desc.name + "\" failed to construct";
    if (mode_ == Mode::Borrowed) ++borrowFresh_;

    proc->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);

    ChainSlot slot;
    slot.node = graph_->addNode(std::move(proc));
    if (!slot.node) return "could not add the built-in node to the chain graph";

    // Always store the CANONICAL description, never the one we were handed: a
    // desc rebuilt from restore XML can be missing fields, and this is what
    // gets written back out on the next save.
    slot.desc     = BuiltinDeviceRegistry::descriptionFor(*device);
    slot.bypassed = false;
    slots_.push_back(std::move(slot));
    // Pristine default, captured BEFORE any seed or dial (borrow reset).
    captureBorrowDefaultState((int) slots_.size() - 1);

    bumpChainRevision();
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }

    const int idx = (int)slots_.size() - 1;

    // Deliberately NO fingerprint and no identity/param-map registration. A
    // built-in is dialled by direct typed writes; giving it a fingerprint
    // would invite the anchor-table path this device exists to bypass.
    applyStructuredIfReady(idx, DialTrigger::slotLoaded);

    if (stateCacheEnabled_)
    {
        attachHostedListener(idx);
        captureSlotState(idx, juce::Time::getMillisecondCounterHiRes());
    }

    EchoJay_NSLog(("ChainHost: built-in \"" + slot.desc.name + "\" added as slot "
                   + juce::String(idx)).toRawUTF8());
    return {};
}

void ChainHost::loadPluginAsync(const juce::PluginDescription& desc,
                                LoadOrigin origin,
                                std::function<void(const juce::String& error)> callback)
{
    // Built-in device: constructed directly, no format manager, no scan.
    // The callback fires synchronously — every existing caller either just
    // updates UI or re-enters its sequencer through Timer::callAfterDelay,
    // so there is no re-entrancy to guard against here.
    if (isBuiltinDescription(desc))
    {
        const auto err = loadBuiltinNow(desc);
        // MOVE LOG: this arm never reaches completeLoad, so the recorder that
        // sits there cannot see it. A built-in slot arriving is a slot
        // arriving; without this the block would carry a dial on a device with
        // no record of that device ever being added.
        if (err.isEmpty() && ! slots_.empty())
            recordLoadIfLicensed(origin, (int) slots_.size() - 1, desc.name);
        if (callback) callback(err);
        return;
    }

    // Borrowed mode: the pool first, same rule as the builtin branch inside
    // loadBuiltinNow. The key is the resolved description's identity, which
    // arrives through the same resolution path on every borrow.
    if (mode_ == Mode::Borrowed && borrowTryReuseInto(desc))
    {
        // MOVE LOG: the third arm, and the same reason as the builtin one. A
        // reused node is a new slot from the rack's point of view.
        if (! slots_.empty())
            recordLoadIfLicensed(origin, (int) slots_.size() - 1, desc.name);
        if (callback) callback({});
        return;
    }

    // Crash blacklist, consulted at the LOAD and not only at the scan
    // (15 Aug 2026): the entries cache can hold a plugin that was
    // blacklisted AFTER the cache was written (the deadman fires on
    // relaunch, and no rescan runs in between), and that gap loaded
    // Auto-Tune Vocal Compressor a second time one minute after it was
    // blacklisted, crashing the DAW again. Withheld, not deleted:
    // deleting its line from chain_blacklist.txt re-enables the plugin.
    if (desc.fileOrIdentifier.isNotEmpty() && isBlacklisted(desc.fileOrIdentifier))
    {
        if (callback)
            callback("\"" + desc.name + "\" was withheld: it crashed a "
                     "previous load and is on the crash skip list "
                     "(chain_blacklist.txt). Deleting its line there "
                     "re-enables it.");
        return;
    }

    bool needsValidation = (desc.pluginFormatName == "VST3" && desc.version.isEmpty());

    juce::PluginDescription fullDesc = desc;
    if (needsValidation)
    {
        std::lock_guard<std::mutex> lk(pluginsMutex_);
        for (const auto& d : knownPlugins_.getTypes())
        {
            if (d.fileOrIdentifier == desc.fileOrIdentifier)
            {
                fullDesc = d;
                needsValidation = false;
                break;
            }
        }
    }

    if (!needsValidation)
    {
        // Through asyncCreatePlugin, so the death mark covers this branch
        // (AU, and VST3s already validated) and not only the fp pass.
        asyncCreatePlugin(fullDesc,
            [this, callback, fullDesc, origin](std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
            {
                if (!inst)
                {
                    // Session-scoped feed exclusion only (iLok may simply be
                    // absent on this machine); gone on plugin reload.
                    sessionLoadFailed_.addIfNotAlreadyThere(
                        sessionLoadKey(fullDesc.name, fullDesc.pluginFormatName));
                    callback(err.isNotEmpty() ? err : "createPluginInstance returned nullptr");
                    return;
                }
                completeLoad(std::move(inst), fullDesc, origin);
                callback({});
            });
        return;
    }

    // Thin VST3: validate in detached thread, poll on message thread. The
    // death mark stands in for the old single-path deadman file.
    const int validationMark = pushDeathMark("validation", desc);

    auto* vst3Fmt = getFormatByName("VST3");
    if (!vst3Fmt) { callback("VST3 format not available"); return; }

    auto vs   = std::make_shared<VST3ValState>();
    auto path = desc.fileOrIdentifier;
    auto weakVs = std::weak_ptr<VST3ValState>(vs);

    std::thread([vst3Fmt, path, weakVs] {
        juce::OwnedArray<juce::PluginDescription> found;
        vst3Fmt->findAllTypesForFile(found, path);
        if (auto s = weakVs.lock())
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            for (auto* d : found) s->results.add(*d);
            s->done.store(true);
        }
    }).detach();

    pollVST3Validation(this, vs, desc, validationMark, origin, callback, 100);
}

// ---------------------------------------------------------------------------
// Graph wiring
// ---------------------------------------------------------------------------
void ChainHost::rebuildGraph()
{
    if (!graph_) return;
    // Remove all existing connections
    for (auto& c : graph_->getConnections())
        graph_->removeConnection(c);

    // Collect active (non-bypassed) slots in chain order. Every active slot
    // gets a SlotWetBlend node (created lazily here, removed with the slot):
    // ALWAYS in circuit, so wet/dry knob moves are pure atomic writes — no
    // graph rebuild, no dropout mid-drag. At 100% wet the blend node is a
    // settled no-op (see SlotWetBlend::processBlock).
    struct ActivePair { juce::AudioProcessorGraph::NodeID plugin, blend; };
    std::vector<ActivePair> active;
    for (auto& s : slots_)
        if (!s.bypassed && s.node)
        {
            if (!s.wetShared)
                s.wetShared = std::make_shared<std::atomic<float>>(s.wet);
            if (!s.blendNode)
                s.blendNode = graph_->addNode(std::make_unique<SlotWetBlend>(s.wetShared));
            active.push_back({ s.node->nodeID, s.blendNode->nodeID });
        }

    hasActiveSlots_.store(!active.empty());

    // The latencies this build bakes into the dry-leg delays; the runtime
    // latency watch compares against these (rebuildForLatencyIfChanged).
    builtLatencies_.assign(slots_.size(), 0);
    for (size_t si = 0; si < slots_.size(); ++si)
        if (slots_[si].node && slots_[si].node->getProcessor())
            builtLatencies_[si] = slots_[si].node->getProcessor()->getLatencySamples();

    if (active.empty())
    {
        // Pure passthrough — also skip graph in process() for safety
        if (inputNode_ && outputNode_)
        {
            graph_->addConnection({{inputNode_->nodeID, 0}, {outputNode_->nodeID, 0}});
            graph_->addConnection({{inputNode_->nodeID, 1}, {outputNode_->nodeID, 1}});
        }
        if (onChainChanged) onChainChanged();
        return;
    }

    auto channelsOf = [&](juce::AudioProcessorGraph::NodeID id, bool inputs) -> int {
        auto* node = graph_->getNodeForId(id);
        auto* proc = node ? node->getProcessor() : nullptr;
        if (!proc) return 2;
        return inputs ? proc->getTotalNumInputChannels()
                      : proc->getTotalNumOutputChannels();
    };

    // Per-stage wiring: prev → plugin (wet path), prev → blend inputs 2/3
    // (dry tap, latency-aligned by the graph's render sequence), plugin →
    // blend inputs 0/1, and the blend node becomes the stage output. A
    // mono-out plugin's uncovered wet channel gets the prev-stage signal
    // (passthrough) so the blend never mixes against silence — the same
    // intent as the old uncovered-channel passthrough at the chain tail.
    juce::AudioProcessorGraph::NodeID prev = inputNode_->nodeID;   // always 2-out
    // Running-level bookkeeping: a slot whose PREDECESSOR changed since the
    // last build (moved, a slot inserted before it, the previous slot
    // bypassed) now sees a different signal, so its tallies restart; a slot
    // whose input is the same signal keeps its history. Recorded per slot
    // index in slots_ order; a slot that was not active last time has no
    // record and starts fresh when it becomes active.
    std::vector<juce::AudioProcessorGraph::NodeID> preds(slots_.size());
    for (auto& stage : active)
    {
        for (size_t si = 0; si < slots_.size(); ++si)
            if (slots_[si].node && slots_[si].node->nodeID == stage.plugin)
            {
                preds[si] = prev;
                const bool changed = si >= builtPredecessors_.size()
                                  || !(builtPredecessors_[si] == prev);
                if (changed && slots_[si].blendNode)
                    if (auto* b = dynamic_cast<SlotWetBlend*>(slots_[si].blendNode->getProcessor()))
                        b->resetTallies();
                break;
            }
        prev = stage.blend;
    }
    builtPredecessors_ = preds;
    prev = inputNode_->nodeID;
    for (auto& stage : active)
    {
        int nIn  = channelsOf(stage.plugin, true);
        int nOut = channelsOf(stage.plugin, false);

        for (int ch = 0; ch < juce::jmin(2, nIn); ++ch)
            graph_->addConnection({{prev, ch}, {stage.plugin, ch}});

        for (int ch = 0; ch < 2; ++ch)
            graph_->addConnection({{prev, ch}, {stage.blend, ch + 2}});   // dry tap

        for (int ch = 0; ch < juce::jmin(2, nOut); ++ch)
            graph_->addConnection({{stage.plugin, ch}, {stage.blend, ch}});
        for (int ch = nOut; ch < 2; ++ch)
            graph_->addConnection({{prev, ch}, {stage.blend, ch}});       // passthrough

        prev = stage.blend;   // blend output (2ch) feeds the next stage
    }

    // Last blend → output (blend always has 2 outs — no uncovered channels)
    graph_->addConnection({{prev, 0}, {outputNode_->nodeID, 0}});
    graph_->addConnection({{prev, 1}, {outputNode_->nodeID, 1}});

    if (onChainChanged) onChainChanged();
}

// ---------------------------------------------------------------------------
// Shared name resolution + entries cache
// ---------------------------------------------------------------------------
void ChainHost::maybeReloadEntriesCache()
{
    auto ecFile = getEntriesCacheFile();
    if (!ecFile.existsAsFile()) return;
    auto mtime = ecFile.getLastModificationTime();
    if (mtime <= entriesCacheTime_) return;   // ours is current

    if (auto doc = juce::XmlDocument::parse(ecFile);
        doc != nullptr && doc->getTagName() == "CHAIN_ENTRIES")
    {
        juce::Array<juce::PluginDescription> loaded;
        for (auto* c : doc->getChildIterator())
        {
            juce::PluginDescription d;
            if (d.loadFromXml(*c)) loaded.add(d);
        }
        if (!loaded.isEmpty())
        {
            std::lock_guard<std::mutex> lock(pluginsMutex_);
            entries_ = loaded;
        }
        entriesScannedAtMs_ = doc->getStringAttribute("scannedAt").getLargeIntValue();
        EchoJay_NSLog(("EJScan: cache reloaded, " + juce::String(loaded.size())
                       + " entr(ies), scanned "
                       + (entriesScannedAtMs_ > 0
                              ? juce::Time(entriesScannedAtMs_).toString(true, true)
                              : juce::String("UNKNOWN (unstamped cache)"))).toRawUTF8());
        // A reload replaces entries_ wholesale (the other host may have
        // scanned), which would drop same-session captured identities:
        // re-apply them from knownPlugins_.
        if (const int filled = enrichThinVst3EntriesFromKnown(); filled > 0)
        {
            hasResolved_ = false;
            EchoJay_NSLog(("EJScan: " + juce::String(filled)
                           + " thin VST3 entr(ies) enriched from load-captured identities").toRawUTF8());
        }
        computeSupersessions();
    }
    entriesCacheTime_ = mtime;
}

juce::String ChainHost::stripParenthetical(const juce::String& raw)
{
    auto s = raw.trim();
    if (s.endsWithChar(')'))
    {
        int open = s.lastIndexOf(" (");
        if (open > 0) return s.substring(0, open).trim();
    }
    return s;
}

bool ChainHost::namesMatchLoose(const juce::String& incoming,
                                const juce::String& entryName)
{
    auto in = incoming.trim(), en = entryName.trim();
    if (in.equalsIgnoreCase(en)) return true;
    auto inBase = stripParenthetical(in);
    if (inBase.equalsIgnoreCase(en)) return true;
    auto enBase = stripParenthetical(en);
    if (normalizeName(inBase) != normalizeName(enBase)) return false;
    // Model-number guard (same as resolveByName): two names that share the
    // stripped stem but carry DIFFERENT trailing numbers are different models
    // ("AMEK EQ 250" vs "AMEK EQ 200"), not a loose match.
    auto ni = trailingModelNumber(inBase), ne = trailingModelNumber(enBase);
    return ! (ni.isNotEmpty() && ne.isNotEmpty() && ni != ne);
}

juce::PluginDescription ChainHost::resolveByName(const juce::String& rawName,
                                                 const juce::String& formatFilter,
                                                 juce::String* matchLogOut,
                                                 WithholdReason* withheldOut,
                                                 juce::StringArray* ambiguousOut) const
{
    if (withheldOut) *withheldOut = WithholdReason::None;
    auto raw  = rawName.trim();
    auto base = stripParenthetical(raw);

    // Built-ins resolve before (and regardless of) the scanned entries and the
    // format filter: they are compiled into both hosts, so they are available in
    // an AU session and a VST3 session alike.
    if (const auto* device = BuiltinDeviceRegistry::instance().findByName(raw))
    {
        if (matchLogOut) *matchLogOut = "built-in -> \"" + device->name + "\"";
        return BuiltinDeviceRegistry::descriptionFor(*device);
    }

    // Manufacturer from the parenthetical (if any) — disambiguation only
    juce::String manu;
    if (raw.endsWithChar(')') && raw.contains(" ("))
        manu = raw.fromLastOccurrenceOf(" (", false, false)
                  .dropLastCharacters(1).trim();

    // Two pools, one pass under the lock. cands are the rows this host
    // offers; withheld are the rows the format filter admits but the ONE
    // withhold decision (WithholdReason, header) keeps back. The withheld
    // pool never resolves; it exists so a miss can say which it was.
    juce::Array<juce::PluginDescription> cands, withheld;
    juce::Array<WithholdReason>          withheldWhy;   // parallel to withheld
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        for (auto& d : entries_)
        {
            if (formatFilter.isNotEmpty() && d.pluginFormatName != formatFilter)
                continue;
            const auto why = withholdReasonLocked(d);
            if (isWithheld(why)) { withheld.add(d); withheldWhy.add(why); continue; }
            cands.add(d);
        }
    }
    // Empty-filter resolution collapses AU-preferring, so first-match
    // semantics are identical to the old scan-time dedupe.
    if (formatFilter.isEmpty())
        cands = collapseAuPreferring(cands);

    // Kept for the "closest" line at the bottom; the ladder derives its own.
    auto keyIn = normalizeName(base);

    // THE MATCH LADDER IS IN EJNameLadder.h (28 Aug 2026), and it now PREFERS
    // THE STEREO REGISTRATION on its two collapsing rungs. This function used
    // to take whichever row sorted first, so every collapsed base name -- every
    // Waves product, since WaveShell registers one AU per channel configuration
    // -- resolved to the MONO build here while the feed's own table resolved it
    // to the stereo one (9c4f629). Add ops, saved-chain recall with a bare name
    // and every Link path go through this ladder and nothing else, so they all
    // loaded the mono build of a stereo plugin.
    //
    // The EXACT rung is untouched and stays first: a name carrying a suffix
    // means that registration. The rule RANKS, it never filters, so a product
    // registered only in mono still resolves to its mono build. The pool is
    // built above, after the format filter and the withhold gate, so the
    // preference composes with the filter rather than reaching past it.
    auto matchIn = [&](const juce::Array<juce::PluginDescription>& pool,
                       juce::String& how,
                       juce::StringArray* ambig = nullptr) -> int
    {
        return echojay::matchInPool(pool, raw, base, manu, how, ambig);
    };

    juce::String how;
    juce::StringArray ambiguous;
    if (const int i = matchIn(cands, how, &ambiguous); i >= 0)
    {
        const auto& d = cands.getReference(i);
        if (matchLogOut)
            *matchLogOut = how + " -> \"" + d.name + "\" [" + d.pluginFormatName + "]";
        return d;
    }

    // AMBIGUOUS, and it returns HERE rather than falling through (31 Aug
    // 2026). The name was found -- several times over -- so searching the
    // withheld pool next would answer a question nobody asked, and could
    // report "withheld" as the reason a name that resolves twice did not
    // resolve. A refusal must name what it saw, and what this saw is a tie.
    if (ambiguous.size() > 1)
    {
        if (withheldOut)  *withheldOut  = WithholdReason::Ambiguous;
        if (ambiguousOut) *ambiguousOut = ambiguous;
        if (matchLogOut)
            *matchLogOut = how + " -> refused; candidates: " + ambiguous.joinIntoString(", ");
        return {};
    }

    // Honest miss: the name is on this machine, this host keeps it back.
    // Still empty (nothing resolves that cannot load), but the caller can
    // say so instead of "not found".
    if (const int i = matchIn(withheld, how); i >= 0)   // no ambiguousOut: see above
    {
        const auto& d   = withheld.getReference(i);
        const auto  why = withheldWhy[i];
        if (withheldOut) *withheldOut = why;
        if (matchLogOut)
            *matchLogOut = "WITHHELD ("
                         + juce::String(why == WithholdReason::CrashBlacklisted
                                            ? "crash blacklist" : "architecture")
                         + ", " + how + ") -> \"" + d.name + "\" [" + d.pluginFormatName + "]";
        return {};
    }

    if (matchLogOut)
    {
        juce::StringArray close;
        for (auto& d : cands)
        {
            auto keyEn = normalizeName(d.name);
            if (keyEn.contains(keyIn) || keyIn.contains(keyEn))
            {
                close.add(d.name);
                if (close.size() >= 3) break;
            }
        }
        *matchLogOut = "NOT FOUND; closest: "
                     + (close.isEmpty() ? juce::String("(none)")
                                        : close.joinIntoString(", "));
    }
    return {};
}

// Resolve a name the model was OFFERED: the registry first, then the feed's own
// displayName -> desc table. See the header for why the order is not the other
// way round. Message thread, matching loadByRecommendedName's convention for
// touching recommendable_ (buildRecommendable writes it on the same thread).
juce::PluginDescription ChainHost::resolveOfferedName(const juce::String& rawName,
                                                      WithholdReason* withheldOut,
                                                      juce::StringArray* ambiguousOut) const
{
    WithholdReason why = WithholdReason::None;
    juce::StringArray ambiguous;
    auto d = resolveByName(rawName, {}, nullptr, &why, &ambiguous);
    if (d.name.isNotEmpty())
    {
        if (withheldOut) *withheldOut = why;   // None on a hit, by contract
        return d;
    }

    // MISS. The registry does not know this name -- but the feed may have
    // offered it, under a display name the alias resolved at scan time. An
    // exact, case-insensitive displayName match only: recommendable_ is a
    // lookup table of names we ourselves published, not a second fuzzy ladder.
    const auto want = rawName.trim();
    for (const auto& e : recommendable_)
        if (e.displayName.trim().equalsIgnoreCase(want))
        {
            EchoJay_NSLog(("EJChain: [offered-name] \"" + want + "\" -> \""
                           + e.desc.name + "\" (feed displayName; registry missed)")
                              .toRawUTF8());
            // recommendable_ is built from the LOADABLE set, so a row that is
            // in it is not withheld: the earlier reason described a different
            // row the ladder happened to reach, and must not travel with this
            // answer.
            if (withheldOut) *withheldOut = WithholdReason::None;
            return e.desc;
        }

    // The feed table is consulted BEFORE the ambiguity is reported, and that
    // order is deliberate: a name the feed itself published is a name we chose
    // to offer, so it is answerable even when the registry ladder found the
    // request tied. Only a name nothing can answer is refused as ambiguous.
    if (withheldOut)  *withheldOut  = why;
    if (ambiguousOut) *ambiguousOut = ambiguous;
    return {};
}

// ---------------------------------------------------------------------------
// Additive accessors (Link hosting)
// ---------------------------------------------------------------------------
juce::PluginDescription ChainHost::getSlotDescription(int i) const
{
    if (i < 0 || i >= (int)slots_.size()) return {};
    return slots_[(size_t)i].desc;
}

juce::AudioProcessor* ChainHost::getSlotProcessor(int i) const
{
    if (i < 0 || i >= (int)slots_.size() || slots_[(size_t)i].node == nullptr)
        return nullptr;
    return slots_[(size_t)i].node->getProcessor();
}

int ChainHost::getTotalLatencySamples() const
{
    int total = 0;
    for (auto& s : slots_)
        if (!s.bypassed && s.node && s.node->getProcessor())
            total += s.node->getProcessor()->getLatencySamples();
    return total;
}

// ---------------------------------------------------------------------------
// Format lookup
// ---------------------------------------------------------------------------
juce::AudioPluginFormat* ChainHost::getFormatByName(const juce::String& namePart) const
{
    auto formats = formatManager_.getFormats();
    for (auto* f : formats)
        if (f->getName().containsIgnoreCase(namePart))
            return f;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Blacklist
// ---------------------------------------------------------------------------
bool ChainHost::isBlacklisted(const juce::String& path) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    return blacklist_.contains(path);
}

void ChainHost::addToBlacklist(const juce::String& path, const juce::String& reason)
{    // Borrowed mode is READ-ONLY on every shared file (spec §2.2): one
    // writer per file, and the primary host is it.
    if (mode_ == Mode::Borrowed) return;

    juce::String bl;
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        if (!blacklist_.contains(path)) blacklist_.add(path);
        if (blacklistMeta_[path].isEmpty())
            blacklistMeta_.set(path,
                (reason.isNotEmpty() ? reason : juce::String("crashed or hung during load"))
                + "\t" + juce::Time::getCurrentTime().toISO8601(true));
        bl << "# EchoJay crash skip list. A plugin listed here is withheld from\n"
              "# the chain feed and refused at load until its line is removed.\n"
              "# Deleting a line re-enables that plugin on the next scan.\n"
              "# Format: path<TAB>reason<TAB>ISO date. A bare path (no tabs) is\n"
              "# a pre-format entry and stays valid.\n";
        for (auto& p : blacklist_)
        {
            bl << p;
            const auto& meta = blacklistMeta_[p];
            if (meta.isNotEmpty()) bl << "\t" << meta;
            bl << "\n";
        }
    }
    // Persisted HERE and only here, immediately: the deadman consumer runs
    // at construction and a crash can end the session before any later
    // save; and because nothing else writes this file, a user's hand
    // deletion of a line is never rewritten from stale memory.
    appSupportDir().createDirectory();
    getBlacklistFile().replaceWithText(bl);
}

void ChainHost::reloadBlacklistFromDisk()
{
    juce::StringArray lines;
    if (auto blFile = getBlacklistFile(); blFile.existsAsFile())
        lines = juce::StringArray::fromLines(blFile.loadFileAsString());
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    blacklist_.clear();
    blacklistMeta_.clear();
    for (auto& raw : lines)
    {
        auto line = raw.trim();
        if (line.isEmpty() || line.startsWithChar('#')) continue;
        // Tabbed form: path<TAB>reason<TAB>ISO date. Bare paths (the
        // pre-format form, possibly CRLF-terminated) stay valid.
        auto path = line.upToFirstOccurrenceOf("\t", false, false).trim();
        if (path.isEmpty()) continue;
        blacklist_.addIfNotAlreadyThere(path);
        auto meta = line.fromFirstOccurrenceOf("\t", false, false).trim();
        if (meta.isNotEmpty() && blacklistMeta_[path].isEmpty())
            blacklistMeta_.set(path, meta);
    }
}

// ---------------------------------------------------------------------------
// Architecture gate (VST3 rows only; see ArchVerdict in the header)
//
// Reads the Mach-O header of the bundle's executable and asks one question:
// does any slice match the cputype of the process this code is running in?
// Header bytes only. No dlopen, no bundle load, no instantiation, so there
// is nothing here that can crash the host; lipo -archs over the same 449
// bundles takes seconds.
//
// The EJ_ARCH_GATE markers fence the pure part so a harness can compile the
// SHIPPED text (sed between the markers) against the census instead of a
// copy that drifts. Keep the fenced block self-contained: juce::File,
// juce::XmlDocument and the mach-o headers, nothing from ChainHost.
// ---------------------------------------------------------------------------
#if JUCE_MAC
namespace {
// EJ_ARCH_GATE_BEGIN
namespace ejarch {

enum Verdict { Loadable, NotLoadable, Unreadable };

// The bundle's executable: <bundle>/Contents/MacOS/<name>. Name match first
// (394 of 448 bundles on the census machine), then CFBundleExecutable from
// Info.plist (the iZotope PluginHooksVST family, the *_VST_AU_Protect
// wrappers, WaveShell: 54 bundles whose binary is not named after the
// bundle), then the first non-dylib file in MacOS/ (Listento ships four
// dylibs beside its binary). A single-file .vst3 (ChopSuey, a Windows PE)
// is returned as itself and fails the magic check below, which is Unreadable.
inline juce::File bundleBinary(const juce::File& bundle)
{
    if (bundle.existsAsFile()) return bundle;
    auto contents = bundle.getChildFile("Contents");
    auto macos    = contents.getChildFile("MacOS");
    if (! macos.isDirectory()) return {};

    auto named = macos.getChildFile(bundle.getFileNameWithoutExtension());
    if (named.existsAsFile()) return named;

    if (auto plist = juce::XmlDocument::parse(contents.getChildFile("Info.plist")))
        if (auto* dict = plist->getChildByName("dict"))
            for (auto* k = dict->getFirstChildElement(); k != nullptr; k = k->getNextElement())
                if (k->hasTagName("key") && k->getAllSubText().trim() == "CFBundleExecutable")
                {
                    if (auto* v = k->getNextElement())
                    {
                        auto f = macos.getChildFile(v->getAllSubText().trim());
                        if (f.existsAsFile()) return f;
                    }
                    break;
                }

    auto files = macos.findChildFiles(juce::File::findFiles, false);
    for (const auto& f : files)
        if (! f.hasFileExtension(".dylib")) return f;
    return files.isEmpty() ? juce::File() : files.getReference(0);
}

inline uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint32_t le32(const uint8_t* p)
{
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);
}

// The cputype of the process this code is running in, read at runtime from
// the main executable's in-memory Mach-O header. Under Rosetta that is the
// x86_64 slice the loader chose, so the answer follows the PROCESS, not the
// machine, and nothing is hardcoded. 0 if dyld reports no image 0 (never
// seen); the caller treats 0 as "cannot judge", which keeps every row.
inline cpu_type_t processCpuType()
{
    if (const auto* mh = _dyld_get_image_header(0)) return mh->cputype;
    return 0;
}

// Judge one executable against a process cputype. Fat: FAT_MAGIC or
// FAT_MAGIC_64, big-endian on disk, walk the arch table (capped at 32
// entries; a real fat file has 2 or 3, and 0xCAFEBABE is also the Java
// class magic, whose next field is a version well above that). Thin:
// MH_MAGIC / MH_MAGIC_64 native, MH_CIGAM / MH_CIGAM_64 byte-swapped
// (a ppc-only thin binary read on a little-endian host). Anything else,
// including a file too short to hold its own header, is Unreadable.
inline Verdict verdictForBinary(const juce::File& bin, cpu_type_t proc)
{
    if (proc == 0 || ! bin.existsAsFile()) return Unreadable;

    juce::MemoryBlock head;
    {
        juce::FileInputStream in(bin);
        if (! in.openedOk()) return Unreadable;
        in.readIntoMemoryBlock(head, 8 + 32 * (int) sizeof(fat_arch_64));
    }
    const auto* p = static_cast<const uint8_t*>(head.getData());
    const auto  n = head.getSize();
    if (n < 8) return Unreadable;

    const uint32_t magicBE = be32(p);
    if (magicBE == FAT_MAGIC || magicBE == FAT_MAGIC_64)
    {
        const bool     is64  = (magicBE == FAT_MAGIC_64);
        const size_t   entry = is64 ? sizeof(fat_arch_64) : sizeof(fat_arch);
        const uint32_t count = be32(p + 4);
        if (count == 0 || count > 32) return Unreadable;
        if (n < 8 + (size_t) count * entry) return Unreadable;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto cputype = (cpu_type_t) be32(p + 8 + i * entry);   // first field of both fat_arch forms
            if (cputype == proc) return Loadable;
        }
        return NotLoadable;
    }

    const uint32_t magicLE = le32(p);
    if (magicLE == MH_MAGIC || magicLE == MH_MAGIC_64)
        return ((cpu_type_t) le32(p + 4) == proc) ? Loadable : NotLoadable;
    if (magicLE == MH_CIGAM || magicLE == MH_CIGAM_64)
        return ((cpu_type_t) be32(p + 4) == proc) ? Loadable : NotLoadable;

    return Unreadable;
}

inline Verdict verdictForBundle(const juce::File& bundle, cpu_type_t proc)
{
    return verdictForBinary(bundleBinary(bundle), proc);
}

} // namespace ejarch
// EJ_ARCH_GATE_END
} // namespace
#endif

// Human name of the process cputype for the withheld-by-architecture line.
static juce::String processArchName()
{
   #if JUCE_MAC
    switch (ejarch::processCpuType())
    {
        case CPU_TYPE_ARM64:  return "arm64";
        case CPU_TYPE_X86_64: return "x86_64";
        case CPU_TYPE_ARM:    return "arm";
        case CPU_TYPE_X86:    return "i386";
        case 0:               return "unknown, gate open";
        default:              return "cputype " + juce::String((int) ejarch::processCpuType());
    }
   #else
    return "not macOS, gate open";
   #endif
}

ChainHost::ArchVerdict ChainHost::archVerdict(const juce::String& path) const
{
   #if JUCE_MAC
    const auto key = path.toStdString();
    {
        std::lock_guard<std::mutex> lk(archMutex_);
        auto it = archCache_.find(key);
        if (it != archCache_.end()) return it->second;
    }
    // Computed outside the lock: file reads under a mutex the feed builders
    // wait on would serialise the first browser open behind disk. Two
    // threads judging the same path once each is harmless.
    static const cpu_type_t proc = ejarch::processCpuType();
    ArchVerdict v = ArchVerdict::Unreadable;
    switch (ejarch::verdictForBundle(juce::File(path), proc))
    {
        case ejarch::Loadable:    v = ArchVerdict::Loadable;    break;
        case ejarch::NotLoadable: v = ArchVerdict::NotLoadable; break;
        case ejarch::Unreadable:  v = ArchVerdict::Unreadable;  break;
    }
    std::lock_guard<std::mutex> lk(archMutex_);
    archCache_[key] = v;
    return v;
   #else
    juce::ignoreUnused(path);
    return ArchVerdict::Unreadable;   // no gate off macOS: every row kept
   #endif
}

bool ChainHost::archLoadable(const juce::String& path) const
{
    return archVerdict(path) != ArchVerdict::NotLoadable;
}

// ---------------------------------------------------------------------------
// Withhold reason: the ONE decision the three feed sites share
// ---------------------------------------------------------------------------
ChainHost::WithholdReason ChainHost::withholdReasonLocked(const juce::PluginDescription& d) const
{
    // Blacklist first: a row that crashed the host is withheld whatever its
    // slices say, and the reason the user can act on is the blacklist line.
    if (d.fileOrIdentifier.isNotEmpty() && blacklist_.contains(d.fileOrIdentifier))
        return WithholdReason::CrashBlacklisted;
    // Settings too large to save: its own file, its own reason (any format)
    if (d.fileOrIdentifier.isNotEmpty() && stateOversize_.find(d.fileOrIdentifier) != stateOversize_.end())
        return WithholdReason::SettingsTooLarge;
    // VST3 rows only. AU rows are never judged and built-ins never reach
    // entries_ (compiled in, exempt by construction).
    if (d.pluginFormatName == "VST3")
    {
        switch (archVerdict(d.fileOrIdentifier))
        {
            case ArchVerdict::NotLoadable: return WithholdReason::ArchitectureIncompatible;
            case ArchVerdict::Unreadable:  return WithholdReason::Unreadable;   // KEPT
            case ArchVerdict::Loadable:    break;
        }
    }
    return WithholdReason::None;
}

ChainHost::WithholdReason ChainHost::withholdReason(const juce::PluginDescription& d) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    return withholdReasonLocked(d);
}

juce::String ChainHost::withholdReasonText(WithholdReason r)
{
    switch (r)
    {
        case WithholdReason::CrashBlacklisted:
            return "was withheld: it crashed a previous load and is on the "
                   "crash skip list (chain_blacklist.txt); deleting its line "
                   "there re-enables it";
        case WithholdReason::ArchitectureIncompatible:
            return "is installed but its VST3 has no " + processArchName()
                 + " build, so it cannot run in this host";
        case WithholdReason::SettingsTooLarge:
            return "is withheld: its settings at their defaults are larger than the "
                 + juce::File::descriptionOfSizeInBytes((juce::int64) kSessionStateMaxSlotBytes)
                 + " a session can save per plugin, so a chain holding it could not be saved "
                   "(chain_state_oversize.txt; deleting its line there offers it again)";
        case WithholdReason::Ambiguous:
            // The candidates are the reason, and this function does not have
            // them. ambiguousNameText authors it; returning empty here keeps
            // the two from disagreeing about one refusal.
            break;
        case WithholdReason::Unreadable:
        case WithholdReason::None:
            break;
    }
    return {};
}

juce::String ChainHost::ambiguousNameText(const juce::StringArray& candidates)
{
    if (candidates.size() < 2) return {};
    // EVERY candidate, in pool order, no cap: see the header. A refusal that
    // lists three of four names is a refusal the reader cannot act on, and
    // the one it drops is the one they wanted often enough to matter.
    return "matches " + juce::String(candidates.size())
         + " installed plugins and nothing chooses between them: "
         + candidates.joinIntoString(", ")
         + " - name one of them exactly";
}

// ---------------------------------------------------------------------------
// Settings ↔ ChainHost resolver
// ---------------------------------------------------------------------------

// normalizeName and trailingModelNumber are in EJNameLadder.h, beside the
// ladder that uses them; `using` declarations at the top of this file keep every
// call site unqualified.

void ChainHost::buildRecommendable(const std::vector<ScannedPlugin>& allPlugins,
                                    const juce::String& formatFilter)
{
    // Snapshot the loadable entries under lock
    juce::Array<juce::PluginDescription> loadable;
    bool entriesEmpty = false;
    int  withheldByArch = 0, archUnreadableKept = 0;
    {
        std::lock_guard<std::mutex> lk(pluginsMutex_);
        entriesEmpty = entries_.isEmpty();
        for (const auto& d : entries_)
        {
            if (formatFilter.isNotEmpty() && d.pluginFormatName != formatFilter) continue;
            // Session load-failure exclusion (AI feed only, THIS session):
            // a plugin that failed to load since this instance opened is
            // not proposed again until reload (iLok may be absent — see
            // ChainHost.h). Edit/build resolution stays unfiltered.
            if (sessionLoadFailed_.contains(sessionLoadKey(d.name, d.pluginFormatName))) continue;
            // Crash-blacklisted rows never reach the feed: the loader now
            // refuses them (loadPluginAsync gate), and offering a plugin we
            // will refuse is worse than not offering it. entries_ can still
            // carry such a row when the blacklist grew after the entries
            // cache was written (deadman on relaunch, no rescan between).
            // Architecture-incompatible rows (VST3 with no slice for this
            // process) would be offered, chosen by the model, then fail at
            // load with "No types found". ONE function decides both
            // (WithholdReason, header); pluginsMutex_ is already held.
            // Unreadable rows are KEPT and counted, so the fail-open path
            // is visible on the line below rather than assumed.
            switch (withholdReasonLocked(d))
            {
                case WithholdReason::CrashBlacklisted:         continue;
                case WithholdReason::ArchitectureIncompatible: ++withheldByArch; continue;
                case WithholdReason::Unreadable:               ++archUnreadableKept; break;
                case WithholdReason::None:                     break;
            }
            loadable.add(d);
        }
    }
    // Logged on EVERY build, including at zero, for the reason the crash
    // blacklist line in the scan gives: a plugin silently missing from the
    // feed is a plugin silently deleted from someone's catalogue. Zero under
    // AU hosting is the expected reading (no VST3 row reaches the gate);
    // ABSENT is a regression.
    EchoJay_NSLog(("EJScan: " + juce::String(withheldByArch)
                   + " VST3 row(s) withheld by architecture (process "
                   + processArchName() + "), "
                   + juce::String(archUnreadableKept) + " unreadable, kept").toRawUTF8());

    // Build a normalized-name → PluginDescription map from the loadable entries.
    // If multiple entries share the same normalized name, keep the first (alphabetically
    // stable since entries_ is already sorted).
    //
    // MODEL-NUMBER KEYING (20 Aug 2026). normalizeName strips a trailing
    // all-digits token as a version, so "AMEK EQ 200" and "AMEK EQ 250" (and
    // "CLA-76" against any other CLA-<digits>) collapsed to ONE stem here,
    // and first-wins handed every such scanner row whichever entry sorted
    // first — the defect c3ad9be fixed in resolveByName/namesMatchLoose,
    // still live at the site where the collision is actually made. The
    // predicate is REUSED (trailingModelNumber), not redefined: the primary
    // key carries the stripped number back, and the bare stem stays as a
    // fallback slot so a number on only one side still tolerates the strip
    // ("Saturn 2" offered for a "Saturn" row) — the tolerance the resolver
    // ladder keeps. A stem hit is guarded at lookup by the same
    // both-sides-differ rule before it counts.
    std::unordered_map<std::string, juce::PluginDescription> nameMap;
    nameMap.reserve((size_t)loadable.size() * 4);
    auto modelKey = [](const juce::String& n) -> std::string
    {
        auto k = normalizeName(n);
        const auto num = trailingModelNumber(n);
        if (num.isNotEmpty()) k = k + "\n" + num;   // '\n' cannot survive a stem
        return k.toStdString();
    };
    auto insertName = [&nameMap, &modelKey](const juce::String& n,
                                            const juce::PluginDescription& d)
    {
        const std::string keyed = modelKey(n);
        if (nameMap.find(keyed) == nameMap.end())
            nameMap[keyed] = d;
        const std::string stem = normalizeName(n).toStdString();
        if (stem != keyed && nameMap.find(stem) == nameMap.end())
            nameMap[stem] = d;
    };
    // The base-name key is NOT first-wins (27 Aug 2026). Where several
    // registrations collapse to one base name, the best channel variant claims
    // it; see EJVariantPreference.h for the rank and the measurement behind it.
    auto insertPreferredBase = [&nameMap, &modelKey](const juce::String& baseName,
                                                     const juce::PluginDescription& d)
    {
        const std::string keyed = modelKey(baseName);
        auto itK = nameMap.find(keyed);
        if (itK == nameMap.end()
            || echojay::channelVariantIsBetter(d.name, itK->second.name))
            nameMap[keyed] = d;
        const std::string stem = normalizeName(baseName).toStdString();
        if (stem != keyed)
        {
            auto itS = nameMap.find(stem);
            if (itS == nameMap.end()
                || echojay::channelVariantIsBetter(d.name, itS->second.name))
                nameMap[stem] = d;
        }
    };

    // Registry base names, for the Waves marketing-name alias (EJWavesAlias.h).
    // Collected here rather than re-derived, so the alias searches exactly the
    // set the nameMap was built from -- arch-gated, blacklist-gated, and all.
    juce::StringArray registryBaseNames;
    for (const auto& d : loadable)
        registryBaseNames.addIfNotAlreadyThere(stripParenthetical(d.name));

    for (const auto& d : loadable)
    {
        insertName(d.name, d);
        // Variant-suffix secondary key: WaveShell AUs register per-variant
        // component names ("Abbey Road Plates (s)"/"(m)") while the Settings
        // scanner lists the curated suffix-less name ("Abbey Road Plates").
        // Without this key the exact map missed them, the plugin dropped out
        // of the AI feed entirely, and the model told the user a plugin
        // RUNNING IN THEIR RACK was "not in your available plugins".
        const auto base = stripParenthetical(d.name);
        if (base != d.name) insertPreferredBase(base, d);
    }

    // Filter enabled scanner plugins and resolve against the map
    int enabledCount = 0;
    std::set<juce::String> excludedUids;
    int excludedRows = 0;
    // The feed is a NAME list, so this is where a name becomes unique
    // (13 Aug 2026, evening). 77 scanner groups share a name across vendor
    // strings the catalog cannot unify (PA sub-brands under their own
    // labels, spacing and casing variants, folder-layout garbage vendors
    // like 'Se' and 'Pitch Shift'), and each group resolved its name TWICE
    // into the feed - 13 duplicate names in a 65KB payload the model
    // reads. Chain entries are intentionally NOT name-unique since 15 Aug
    // 2026: the cache keeps both the AU and the VST3 row of a name, and
    // collapsing happens at presentation time (collapseAuPreferring in the
    // empty-filter branches). The nameMap above is first-wins, so a
    // format-filtered feed still resolves one row per name; the collapse
    // here stays first-wins, with its own counter so enabled still
    // reconciles:
    //   enabled = resolved + duplicates + unmatched.
    std::set<juce::String> pushedNames;
    int duplicateNames = 0;
    std::vector<RecommendableEntry> resolved;

    // Model-keyed slot first (exact product), then the bare stem guarded by
    // the c3ad9be predicate: a stem hit whose entry carries a DIFFERENT
    // trailing number than this row is a different product, not a resolution.
    //
    // THE SECOND TIER IS EXACTLY ONE SHAPE, and knowing that is what makes the
    // guard below safe (measured 31 Aug 2026). modelKey is normalizeName plus
    // "\n" + trailingModelNumber when there IS a bare trailing number, so when
    // there is none the model key and the bare stem are the SAME STRING: a
    // tier-1 miss is then a tier-2 miss on an identical lookup. Tier 2 is
    // therefore reachable ONLY when the request carries a bare trailing number
    // the entry does not. Verified against all 1,491 scanner rows on this
    // machine: zero counter-examples.
    auto lookupName = [&nameMap, &modelKey](const juce::String& n)
    {
        auto it = nameMap.find(modelKey(n));
        if (it != nameMap.end()) return it;
        it = nameMap.find(normalizeName(n).toStdString());
        if (it != nameMap.end())
        {
            const auto numIn = trailingModelNumber(stripParenthetical(n));
            const auto numEn = trailingModelNumber(stripParenthetical(it->second.name));
            if (numIn.isNotEmpty() && numEn.isNotEmpty() && numIn != numEn)
                return nameMap.end();
            // DIRECTION A: the request carries a number, the entry carries
            // none. THE FEED PATH REFUSES THIS, and EJNameLadder.h's ladder
            // deliberately does not -- see the note beside its tolerance for
            // why the two diverge rather than one of them being wrong.
            //
            // WHAT IT COSTS AND WHAT IT BUYS, MEASURED over this machine's
            // 1,491 scanner rows: tier 2 has exactly ONE member, and it is the
            // defect. Logic Pro's stock "DeEsser 2" reached this branch, the
            // strip took its "2", and it bound to WAVES' "DeEsser (s)" -- a
            // different vendor's plugin, advertised to the model under Logic's
            // name. Logic's DeEsser 2 is not in entries_ at all (stock plugins
            // are not standalone AUs), so the strip was the only reason that
            // name resolved to anything. Refusing costs zero correct bindings
            // here and removes that one.
            //
            // DIRECTION B IS UNTOUCHED and is the useful half: an entry that
            // carries a number the request does not ("Pro-Q" finding "Pro-Q 3",
            // "Saturn" finding "Saturn 2") still binds, because that asymmetry
            // reaches tier 1, not here.
            //
            // NOT KEYED ON THE VENDOR, and that was the measured decision
            // rather than the obvious one. A vendor comparison looks like the
            // natural test and fails on this data: scanner and registry vendor
            // strings disagree outright on 73 of 654 resolved pairs (11.2%) --
            // developer against distributor ("Adptr" / "Se" against "Plugin
            // Alliance"), a CATEGORY sitting in the field ("Pitch Shift" for
            // MAutoPitch), brand renames -- and 851 of 860 VST3 registry rows
            // carry no manufacturer at all, so it fails open exactly where it
            // would be needed. 72 of those 73 disagreements resolve on tier 1
            // where the name matched byte-for-byte and the vendor is beside
            // the point. The number asymmetry tests what actually went wrong.
            if (numIn.isNotEmpty() && numEn.isEmpty())
            {
                // LOGGED, ALWAYS. A refusal that is silent is indistinguishable
                // from one that never fired, and what it removes is a name
                // quietly missing from the feed on someone else's machine.
                EchoJay_NSLog(("EJScan: [name-guard] REFUSED \"" + n + "\" -> \""
                               + it->second.name + "\" [" + it->second.pluginFormatName
                               + "]: direction-A number asymmetry (request carries \""
                               + numIn + "\", entry carries none)").toRawUTF8());
                return nameMap.end();
            }
        }
        return it;
    };

    // ONE RESOLUTION LADDER, ASKED BY BOTH BRANCHES (30 Aug 2026). The disabled
    // branch has to resolve a row exactly as the enabled branch does, or the
    // suppression it derives would answer a different question from the offer it
    // exists to cancel. Factored rather than copied, for the reason the variant
    // rule is reused rather than restated in EJWavesRegistryFeed.h: a second
    // copy of a ladder drifts, and the drift is invisible until a name stops
    // resolving. logAlias is false for disabled rows only so the [waves-alias]
    // stream keeps meaning "a row the model was offered".
    auto resolveScannerRow = [&](const ScannedPlugin& sp, bool logAlias)
    {
        auto it = lookupName(sp.name);
        if (it == nameMap.end() && sp.name.containsChar(':'))
            it = lookupName(sp.name.fromFirstOccurrenceOf(":", false, false).trim());
        if (it == nameMap.end() && sp.manufacturer == "Waves")
        {
            const auto aliased = echojay::wavesAliasFor(sp.name, registryBaseNames);
            if (aliased.isNotEmpty())
            {
                it = lookupName(aliased);
                if (it != nameMap.end() && logAlias)
                    EchoJay_NSLog(("EJScan: [waves-alias] \"" + sp.name + "\" -> \""
                                   + it->second.name + "\"").toRawUTF8());
            }
        }
        return it;
    };

    // What the user UNTICKED, resolved to registrations. This cannot be derived
    // downstream from uids: a scanner uid keys the catalog MARKETING name while
    // these registrations carry the SHELL name -- "API 2500" keys
    // api_2500_waves and "API-2500 (s)" keys api-2500_waves, one hyphen apart
    // and never equal. Measured here: of 289 registry products only 31 produce
    // a uid any scanner row also produces, so a uid test would have missed 258
    // and looked like it worked. The name ladder is the one bridge between the
    // two vocabularies this codebase has, so the untick crosses on it -- and
    // crosses as a REGISTRATION, leaving scope and collapse to the header.
    std::vector<juce::PluginDescription> untickedRegistrations;

    for (const auto& sp : allPlugins)
    {
        if (!sp.enabled)
        {
            ++excludedRows;
            excludedUids.insert(sp.uid);
            // THE UNTICK HAS TO REACH THE REGISTRY PATH, or it does not remove
            // the plugin, it RENAMES it: this row leaving `resolved` is exactly
            // what lets the registry path offer the same product again under
            // its shell base name. Resolved, not classified -- whether this is
            // a Waves row and what product it collapses to are the header's
            // calls, and a copy of them here is the thing wr PIN7 forbids.
            auto dit = resolveScannerRow(sp, false);
            if (dit != nameMap.end()) untickedRegistrations.push_back(dit->second);
            continue;
        }
        ++enabledCount;

        // Exact normalized name (model-keyed, stem-guarded), then the
        // manufacturer-prefix strip, then the WAVES MARKETING-NAME ALIAS
        // (28 Aug 2026) -- the scanner injects Waves' marketing names
        // (expandWavesCatalog) while the shell registers shorter ones, so 35 of
        // 69 ticked Waves rows resolved to nothing and the model called
        // installed plugins missing. All three now live in resolveScannerRow
        // above, which the disabled branch asks too.
        auto it = resolveScannerRow(sp, true);

        if (it != nameMap.end())
        {
            if (! pushedNames.insert(sp.name).second)
            {
                ++duplicateNames;   // second row of a same-name pair; the
                                    // desc resolves identically, so nothing
                                    // is lost, only the repeat.
                continue;
            }
            // TRIPWIRE (20 Aug 2026): a feed row whose display name is not
            // its description's name is a RENAME — the model is offered one
            // name and the loader will be handed another's binary. The AMEK
            // collision shipped exactly this shape for weeks with no line
            // saying so; this is the client analogue of the server's
            // [injected-block-leak] alarm. One line per divergent row, both
            // names, every build. (WaveShell variant-suffix rows trip it by
            // construction — "Abbey Road Plates" -> "... (s)" — which is a
            // rename too; the line names both so the benign shape is
            // recognisable on sight.)
            if (sp.name != it->second.name)
                EchoJay_NSLog(("EJScan: [feed-rename] scanner \"" + sp.name
                               + "\" -> entry \"" + it->second.name + "\" ["
                               + it->second.pluginFormatName + "]").toRawUTF8());
            resolved.push_back({ sp.name, it->second });
        }
    }

    // WAVES CANDIDATES COME FROM THE REGISTRY, NOT THE CATALOG (28 Aug 2026).
    // Everything above walks SCANNER rows, so a Waves product could only enter
    // the feed if the 69-name curated catalog happened to name it. This
    // machine's registry holds 289 Waves products and the feed carried 64 of
    // them, so the gate was hiding 225 installed plugins from the model while
    // the user looked at them in the Chain browser. The catalog is neither
    // deleted nor bypassed: its rows still resolve in the loop above, at their
    // own positions, under the marketing names real sessions use, and they are
    // the only Waves names cold start has. They are simply no longer the
    // membership test. See EJWavesRegistryFeed.h for the scope predicate, the
    // collapse and the two refusals.
    //
    // APPEND-ONLY, AND THAT IS THE WHOLE OF THE ADDITIVE ARGUMENT: nameMap is
    // not touched, the loop above is not touched, and no row already in
    // `resolved` is rewritten, re-pointed or reordered. Every non-Waves
    // resolution is therefore byte-identical to what it was, by construction
    // rather than by inspection. NO DECISION LIVES HERE -- the header owns
    // scope, collapse, variant and both refusals, so the pins can run the
    // shipped bytes against the real registry without a rebuild.
    const int scannerResolved = (int) resolved.size();
    const auto wavesRows = echojay::wavesRegistryFeedRows(loadable, resolved, untickedRegistrations);
    for (const auto& r : wavesRows.rows)
    {
        pushedNames.insert(r.name);
        resolved.push_back({ r.name, r.desc });
    }
    // Logged on EVERY build, including at zero, for the same reason the
    // arch-withheld line above is: a Waves catalogue that silently stops
    // reaching the model looks exactly like a model that stopped naming Waves
    // plugins. Zero under a VST3 format filter is the expected reading (the
    // registrations are AudioUnits), and so is zero with no entries cache.
    EchoJay_NSLog(("EJScan: " + juce::String((int) wavesRows.rows.size())
                   + " Waves product(s) added from the registry, of "
                   + juce::String(wavesRows.products) + " registered ("
                   + juce::String(wavesRows.alreadyOffered)
                   + " already offered under another name, "
                   + juce::String(wavesRows.nameTakenByOther)
                   + " name(s) held by a different plugin, "
                   + juce::String(wavesRows.untickedInSettings)
                   + " unticked in Settings)").toRawUTF8());

    // The resolver coverage triple, relocated from a never-rendered label
    // (13 Aug 2026, the dead-layer sweep) and promoted from DBG to a
    // release-build line: unmatched is the number that would have flagged a
    // starving resolver, and it existed nowhere a release build could see.
    // THE NUMBER THAT MUST EQUAL feed= ON THE EJMapFps LINE IS feed= HERE,
    // NOT resolved= (28 Aug 2026). Both still count recommendable_, but
    // recommendable_ no longer comes from the scanner walk alone: `resolved`
    // is the scanner half and keeps the identity that made this line
    // checkable at a glance,
    //     enabled = resolved + duplicates + unmatched,
    // while the registry-sourced Waves rows are named separately and the two
    // are summed into feed=. A divergence between feed= here and feed= on the
    // EJMapFps line is a FINDING (two counts of one population disagreeing),
    // not a rounding difference.
    // input and excluded ride the line (13 Aug 2026, the Brainworx 56) so
    // the accounting is checkable on ONE line: input - excluded = enabled,
    // and excluded against the EJScan set-size lines is one subtraction in
    // the same log stream. The 56 hid for a day because enabled's
    // reconciliation against the row count lived in nobody's head.
    //
    // "from N uid(s)" (same day, evening): a gap between excluded ROWS and
    // the distinct uids excluding them means one uid is excluding multiple
    // rows - duplicate catalog rows sharing a canonical identity (the 57
    // bx AU/VST3 doubles), or, formerly, a disabled set holding more than
    // one uid vocabulary. Either way the gap is a condition to SEE on the
    // line, not to derive from four terminal commands after the fact.
    // With rows unique, excluded ROWS equals excluded UIDS; a gap means
    // duplicate rows have come back, and the line says so itself instead of
    // waiting for someone to derive it by hand.
    EchoJay_NSLog(("EJScan: resolver rebuilt, " + juce::String(scannerResolved)
                   + " resolved (input=" + juce::String((int) allPlugins.size())
                   + ", enabled=" + juce::String(enabledCount)
                   + ", excluded=" + juce::String(excludedRows)
                   + " from " + juce::String((int) excludedUids.size()) + " uid(s)"
                   + ", duplicates=" + juce::String(duplicateNames)
                   + ", unmatched=" + juce::String(enabledCount - scannerResolved - duplicateNames)
                   + ", wavesFromRegistry=" + juce::String((int) wavesRows.rows.size())
                   + ", feed=" + juce::String((int) resolved.size())
                   + ")"
                   + (excludedRows != (int) excludedUids.size()
                          ? juce::String(" [DUPLICATE ROWS: excluded rows exceed uids]")
                          : juce::String())).toRawUTF8());

    // Cache result (message thread only — no mutex)
    recommendable_          = std::move(resolved);
    recommendableEnabledIn_ = enabledCount;
    recommendableFormat_    = formatFilter;

    // Feed-split mode, logged on every scan so which list the model saw is
    // never a guess after the fact.
    EchoJay_NSLog(("EJFeed: split MODE=" + juce::String(feedSplitEnabled()
                       ? "ON (dialable subset)" : "OFF (full list)")
                   + ", " + juce::String((int) recommendable_.size())
                   + " recommendable").toRawUTF8());

    // Latch only when both inputs were real: a build against an empty entries
    // list (scan still running) or an unloaded scanner cache must not count
    // as resolved, or the retry paths would stop retrying too early.
    hasResolved_ = !entriesEmpty && enabledCount > 0;
}

juce::StringArray ChainHost::getRecommendableNames() const
{
    juce::StringArray names;
    for (const auto& e : recommendable_)
        names.add(e.displayName);
    return names;
}

// So the five non-tiered writers of `settings` invalidate every part of the
// model's copy, not whichever field someone remembered.
// THE TIER PREFIXES, defined once. They are written by the card composer at
// apply time and by modelSettingsForSlot at injection time, and a literal
// duplicated across two sites is a rename waiting to desynchronise them.
const char* const ChainHost::kLandedPrefix  = "Landed: ";
const char* const ChainHost::kAskedPrefix   = "Asked, not verified: ";
const char* const ChainHost::kRefusedPrefix = "Refused: ";

void ChainHost::recordMove(int slot, const juce::String& plugin, const juce::String& param,
                           const juce::String& before, const juce::String& after,
                           const juce::String& reason, bool landed)
{
    // Append, then drop from the FRONT past the bound: the newest moves are
    // the ones a follow-up question is about ("why did you do that"), and an
    // evicted move is one the model can no longer be asked about either way.
    moveLog_.push_back({ moveTurn_, slot, plugin, param, before, after, reason, landed });
    while ((int) moveLog_.size() > kMoveLogMax)
        moveLog_.erase(moveLog_.begin());
}

// ---------------------------------------------------------------------------
// Move log persistence (5 Sep 2026). JSON, into the blob PluginProcessor
// already writes; readable key names because 48 entries is about 11 KB
// against a 16 MB slot-state cap, and a blob somebody can read in a crash
// report is worth more than four saved kilobytes.
// ---------------------------------------------------------------------------
juce::var ChainHost::getMoveLogStateVar() const
{
    if (moveLog_.empty() && moveTurn_ == 0) return {};   // nothing to say, no key

    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty("turn", moveTurn_);
    juce::Array<juce::var> arr;
    for (const auto& e : moveLog_)
    {
        // Boundaries are made at restore time, one per restore. Writing them
        // back would stack a new line every time the session is reopened.
        if (e.kind == MoveLogEntry::Kind::SessionBreak) continue;
        auto eo = std::make_unique<juce::DynamicObject>();
        eo->setProperty("turn",   e.turn);
        eo->setProperty("slot",   e.slot);
        eo->setProperty("kind",   (int) e.kind);
        eo->setProperty("plugin", e.plugin);
        eo->setProperty("param",  e.param);
        eo->setProperty("before", e.before);
        eo->setProperty("after",  e.after);
        eo->setProperty("reason", e.reason);
        eo->setProperty("landed", e.landed);
        arr.add(juce::var(eo.release()));
    }
    o->setProperty("entries", arr);
    return juce::var(o.release());
}

void ChainHost::restoreMoveLogState (const juce::var& v)
{
    auto* o = v.getDynamicObject();
    if (o == nullptr) return;

    moveTurn_ = restoredMoveTurn(moveTurn_, (int) o->getProperty("turn"));

    std::vector<MoveLogEntry> restored;
    if (auto* arr = o->getProperty("entries").getArray())
        for (const auto& ev : *arr)
            if (auto* eo = ev.getDynamicObject())
            {
                MoveLogEntry e;
                e.turn   = (int) eo->getProperty("turn");
                e.slot   = (int) eo->getProperty("slot");
                const int k = (int) eo->getProperty("kind");
                // A kind this build does not know about (a newer build wrote
                // it) reads as a Dial rather than indexing off the end of the
                // renderer's switch. The line is then mislabelled, which is
                // survivable; a bad cast is not.
                e.kind   = (k >= 0 && k <= (int) MoveLogEntry::Kind::SessionBreak)
                             ? (MoveLogEntry::Kind) k : MoveLogEntry::Kind::Dial;
                e.plugin = eo->getProperty("plugin").toString();
                e.param  = eo->getProperty("param").toString();
                e.before = eo->getProperty("before").toString();
                e.after  = eo->getProperty("after").toString();
                e.reason = eo->getProperty("reason").toString();
                e.landed = (bool) eo->getProperty("landed");
                restored.push_back(std::move(e));
            }
    if (restored.empty()) return;   // a turn counter alone needs no boundary

    moveLog_ = mergeRestoredLog(std::move(restored), moveLog_, kMoveLogMax);
    EchoJay_NSLog(("EJMoveLog: restored " + juce::String((int) moveLog_.size())
                   + " entries (incl. session boundary), turn continues at "
                   + juce::String(moveTurn_)).toRawUTF8());
}

// ONE place decides what an origin licenses. completeLoad and the two arms of
// loadPluginAsync that bypass it all call this rather than each testing the
// enum, so a fourth load path cannot quietly grow a fourth opinion.
void ChainHost::recordLoadIfLicensed(LoadOrigin origin, int slot,
                                     const juce::String& arrivedName)
{
    // The verdict is loadRecordFor's, not this function's: the enum is read in
    // exactly one place so the gate can drive the shipped answer.
    const auto v = loadRecordFor(origin);
    if (! v.record) return;   // Restore: not an act of EchoJay's
    recordStructural(static_cast<MoveLogEntry::Kind>(v.kind), slot,
                     arrivedName, juce::String(), juce::String());
}

void ChainHost::recordStructural(MoveLogEntry::Kind kind, int slot,
                                 const juce::String& arrived, const juce::String& gone,
                                 const juce::String& reason)
{
    MoveLogEntry e;
    e.turn   = moveTurn_;
    e.slot   = slot;
    e.plugin = arrived.isNotEmpty() ? arrived : gone;
    e.before = gone;        // what left, for a swap or a removal
    e.after  = arrived;     // what arrived, for a load or a swap
    e.reason = reason;
    e.landed = true;        // structural moves happened or were not recorded
    e.kind   = kind;
    moveLog_.push_back(std::move(e));
    while ((int) moveLog_.size() > kMoveLogMax)
        moveLog_.erase(moveLog_.begin());
}

void ChainHost::annotateLastMove(const juce::String& reason)
{
    if (moveLog_.empty() || reason.trim().isEmpty()) return;
    moveLog_.back().reason = clipMoveReason (reason);
}

// A reason is a ROLE, not a settings dump, and the caller cannot promise that.
// The AI build loop hands over the model's per-slot prose, which is a role
// where the model wrote one ("transient control") and a full sentence of
// settings where it did not. Unclipped, one BUILT line could be longer than
// the twelve around it and the bound's measurement would mean nothing. Clipped
// at a word boundary so the line still reads, with the length in one place so
// the contract's byte table has something to point at.
juce::String ChainHost::clipMoveReason (const juce::String& raw)
{
    auto t = raw.trim();
    // One sentence at most: the first is the role where there is one.
    const int stop = t.indexOfChar ('.');
    if (stop > 0) t = t.substring (0, stop).trim();
    if (t.length() <= kMoveReasonMax) return t;
    auto cut = t.substring (0, kMoveReasonMax);
    const int sp = cut.lastIndexOfChar (' ');
    if (sp > kMoveReasonMax / 2) cut = cut.substring (0, sp);
    return cut.trim() + "...";
}

void ChainHost::collapseLastPairIntoSwap(int slot, const juce::String& arrived,
                                         const juce::String& gone)
{
    if (moveLog_.size() < 2) return;
    auto& last = moveLog_[moveLog_.size() - 1];
    auto& prev = moveLog_[moveLog_.size() - 2];
    const bool pair = (last.kind == MoveLogEntry::Kind::Remove
                       && prev.kind == MoveLogEntry::Kind::Load)
                   || (last.kind == MoveLogEntry::Kind::Load
                       && prev.kind == MoveLogEntry::Kind::Remove);
    if (! pair) return;                       // something else happened between
    const auto reason = prev.reason.isNotEmpty() ? prev.reason : last.reason;
    moveLog_.pop_back();
    moveLog_.pop_back();
    recordStructural(MoveLogEntry::Kind::Swap, slot, arrived, gone, reason);
}

void ChainHost::clearModelTiers(ChainSlot& s)
{
    s.modelLandedBits.clear();  s.modelLandedIdx.clear();
    s.modelAskedBits.clear();   s.modelAskedIdx.clear();
    s.modelRefusedBits.clear();
}

void ChainHost::refreshSlotParamReads()
{
    for (int i = 0; i < (int) slots_.size(); ++i)
    {
        auto& s = slots_[(size_t) i];
        // THE ONE SWEEP, through the shared header read so the pins exercise
        // these bytes rather than a copy of them.
        s.liveReads = echojay::readAllParamDisplays(getSlotProcessor(i),
                                                    s.liveReadFailed, s.liveParamCount);
    }
}

bool ChainHost::slotHasLiveReads(int slot) const
{
    if (slot < 0 || slot >= (int) slots_.size()) return false;
    const auto& s = slots_[(size_t) slot];
    return ! s.liveReadFailed && ! s.liveReads.isEmpty();
}

bool ChainHost::hasLiveReadForIndex(int slot, int paramIndex) const
{
    if (slot < 0 || slot >= (int) slots_.size()) return false;
    if (paramIndex < 0) return false;               // no index, no join, keep the echo
    const auto& s = slots_[(size_t) slot];
    if (s.liveReadFailed) return false;             // the echo is the only source
    if (paramIndex >= s.liveReads.size()) return false;   // beyond the sweep, or none taken
    // An EMPTY read is not a live value. The plugin answered with nothing, so
    // the echo is still the only number anyone has for this control.
    return s.liveReads[paramIndex].trim().isNotEmpty();
}

juce::String ChainHost::modelSettingsForSlot(int slot) const
{
    if (slot < 0 || slot >= (int) slots_.size()) return {};
    const auto& s = slots_[(size_t) slot];

    // SUPPRESSION, and it is conditional on an ACTUAL read. A control whose
    // index has a non-empty live read loses its echoed value entirely: the
    // read is now the better source and two numbers for one control is worse
    // than either alone. Everything else is kept untouched -- a readFailed
    // slot, an index the sweep never reached, a control with no index, and
    // every refused entry.
    auto keep = [this, slot](const juce::StringArray& bits, const juce::Array<int>& idx)
    {
        juce::StringArray out;
        for (int i = 0; i < bits.size(); ++i)
        {
            const int pi = i < idx.size() ? idx[i] : -1;
            if (! hasLiveReadForIndex(slot, pi)) out.add(bits[i]);
        }
        return out;
    };
    const auto landed = keep(s.modelLandedBits, s.modelLandedIdx);
    const auto asked  = keep(s.modelAskedBits,  s.modelAskedIdx);

    juce::StringArray lines;
    if (! landed.isEmpty())              lines.add(kLandedPrefix + landed.joinIntoString(", "));
    if (! asked.isEmpty())               lines.add(kAskedPrefix + asked.joinIntoString(", "));
    // NEVER SUPPRESSED. A refusal is a record of a request that was rejected,
    // not a claim about where the knob sits, and the number it names exists
    // nowhere else.
    if (! s.modelRefusedBits.isEmpty())  lines.add(kRefusedPrefix + s.modelRefusedBits.joinIntoString("; "));
    return lines.joinIntoString("\n");
}

juce::var ChainHost::fallbackEntryForSlot(int slot, juce::String& whyOut) const
{
    whyOut = {};
    if (slot < 0 || slot >= (int) slots_.size()) { whyOut = "no such slot"; return {}; }
    const auto& s = slots_[(size_t) slot];
    if (s.fp.isEmpty())                              { whyOut = "slot has no fingerprint"; return {}; }
    if (paramMaps_.find(s.fp) != paramMaps_.end())   { whyOut = "already mapped exactly"; return {}; }
    auto* proc = getSlotProcessor(slot);
    if (proc == nullptr)                             { whyOut = "no live instance"; return {}; }

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty("ik", echojay::identityKeyForDescription(s.desc));
    o->setProperty("fp", s.fp);
    if (s.desc.manufacturerName.isNotEmpty())
        o->setProperty("manufacturer", s.desc.manufacturerName);
    auto& params = proc->getParameters();
    o->setProperty("param_count", params.size());
    // The name guard's input. Capped for the same reason the reads sweep is:
    // a pathological plugin must not put twenty thousand names in a request
    // body. kParamNameQueryLen, not a literal: the tagged-path assertion in
    // applyOne re-reads with the same constant, so the string it compares is
    // the one the server verified.
    juce::Array<juce::var> names;
    const int n = juce::jmin(params.size(), echojay::kMaxParamReadsPerSlot);
    for (int p = 0; p < n; ++p)
        names.add(params[p] != nullptr ? params[p]->getName(echojay::kParamNameQueryLen)
                                       : juce::String());
    o->setProperty("param_names", names);
    return juce::var(o.get());
}

static juce::String wrapFallbackBody(const juce::Array<juce::var>& plugins)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("mode", "lookup");
    root->setProperty("plugins", juce::var(plugins));
    return juce::JSON::toString(juce::var(root.get()), true);
}

juce::String ChainHost::buildFallbackLookupJsonForSlot(int slot) const
{
    juce::String why;
    auto e = fallbackEntryForSlot(slot, why);
    if (e.getDynamicObject() == nullptr)
    {
        // NEVER SILENT (27 Aug 2026). The previous version returned an empty
        // string here and the caller returned without a word, so "the leg
        // never fired" and "the leg fired and had nothing to ask" were
        // indistinguishable in the log -- which is exactly the pair that cost
        // a live test.
        EchoJay_NSLog(("EJFallback: slot " + juce::String(slot + 1) + " not asked -- "
                       + why).toRawUTF8());
        return {};
    }
    juce::Array<juce::var> one; one.add(e);
    return wrapFallbackBody(one);
}

juce::String ChainHost::buildFallbackLookupJson() const
{
    juce::Array<juce::var> plugins;
    juce::StringArray asked, reasons;
    for (int i = 0; i < (int) slots_.size(); ++i)
    {
        juce::String why;
        auto e = fallbackEntryForSlot(i, why);
        if (e.getDynamicObject() == nullptr) { reasons.addIfNotAlreadyThere(why); continue; }
        const auto fp = slots_[(size_t) i].fp;
        if (asked.contains(fp)) continue;          // one ask per fp
        plugins.add(e);
        asked.add(fp);
    }
    if (plugins.isEmpty())
    {
        EchoJay_NSLog(("EJFallback: nothing to ask across " + juce::String((int) slots_.size())
                       + " slot(s) -- "
                       + (reasons.isEmpty() ? juce::String("no slots") : reasons.joinIntoString("; ")))
                          .toRawUTF8());
        return {};
    }
    return wrapFallbackBody(plugins);
}

void ChainHost::storeFallbackMaps(const juce::var& resultsArray)
{
    auto* arr = resultsArray.getArray();
    if (arr == nullptr) return;
    int stored = 0, refused = 0;
    for (auto& rv : *arr)
    {
        auto* r = rv.getDynamicObject();
        if (r == nullptr) continue;
        const auto ik  = r->getProperty("ik").toString();
        const auto map = r->getProperty("map");
        if (map.getDynamicObject() == nullptr)
        {
            // A miss is a real answer and it is logged, not swallowed: the
            // reason names WHY the newest mapped version refused (names
            // disagreed, fewer params, manufacturer mismatch), which is the
            // difference between "no map exists" and "one exists and is not
            // safe for this binary".
            ++refused;
            EchoJay_NSLog(("EJFallback: no map for " + ik + " -- tier "
                           + r->getProperty("tier").toString() + ", reason "
                           + r->getProperty("reason").toString()).toRawUTF8());
            continue;
        }
        // Store under the fp that ASKED. Every slot sharing that identity
        // finds it, and the exact-fp path is untouched because this only runs
        // for fps that had no map at all.
        juce::String wantFp;
        for (const auto& sl : slots_)
            if (echojay::identityKeyForDescription(sl.desc) == ik) { wantFp = sl.fp; break; }
        if (wantFp.isEmpty()) continue;
        paramMaps_[wantFp] = map;
        fpFetchedAt_[wantFp] = juce::Time::currentTimeMillis();
        ++stored;
        EchoJay_NSLog(("EJFallback: " + ik + " served from "
                       + r->getProperty("served_from").toString() + " (tier "
                       + r->getProperty("tier").toString()
                       + ", anchors_unverified="
                       + juce::String((int) (bool) map.getProperty("anchors_unverified", false))
                       + ")").toRawUTF8());
    }
    if (stored > 0) saveParamMapsToDisk();
    EchoJay_NSLog(("EJFallback: " + juce::String(stored) + " served, "
                   + juce::String(refused) + " refused").toRawUTF8());

    // RE-APPLY, mirroring storeParamMaps (27 Aug 2026). Without this the map
    // landed in the cache and nothing dialled: setSlotStructuredSettings runs
    // applyStructuredIfReady BEFORE kicking any fetch, so the waiting slot has
    // already settled as pending/noMap and only a re-evaluation can move it.
    // The tagged map would have sat unused until some later turn happened to
    // re-trigger a dial, which is indistinguishable from the feature not
    // working.
    for (auto& rv : *arr)
        if (auto* r = rv.getDynamicObject())
            for (const auto& sl : slots_)
                if (echojay::identityKeyForDescription(sl.desc) == r->getProperty("ik").toString())
                    pendingMapFps_.removeString(sl.fp);
    bool changed = false;
    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        const bool wasApplied = slots_[(size_t)i].structuredApplied;
        applyStructuredIfReady(i, DialTrigger::mapArrived);
        if (!wasApplied && slots_[(size_t)i].structuredApplied) changed = true;
        if (settleStaleRung(i)) changed = true;
    }
    if (changed && onSlotSettingsChanged) onSlotSettingsChanged();
}

juce::String ChainHost::buildSlotParamReadsJson() const
{
    juce::Array<juce::var> arr;
    for (int i = 0; i < (int) slots_.size(); ++i)
    {
        const auto& s = slots_[(size_t) i];
        const auto name = s.desc.name;
        // SERIALISES THE CACHED SWEEP, never a fresh read. The suppression at
        // injection time and the values on the wire must come from the SAME
        // sweep, or the two-stores defect this fixes simply moves: the echo
        // would be dropped on one number and a different number shipped.
        //
        // THE SLOT APPEARS EITHER WAY (8d). No instance is readFailed, NOT an
        // absent entry: 8d gives absence its own meaning ("stale client or old
        // build, print exactly as today"), so a slot that exists and could not
        // be read must not borrow it. Logged; the note stays clean.
        if (s.liveReadFailed)
            EchoJay_NSLog(("EJParamReads: slot " + juce::String(i + 1) + " (\"" + name
                           + "\") readFailed -- no hosted instance").toRawUTF8());
        arr.add(echojay::slotParamReadsVar(
            i + 1,                                  // 1-based, as [CURRENT CHAIN] prints
            name,
            s.liveParamCount,                       // TRUE count: drives `truncated`
            [&s](int p) { return s.liveReads[p]; },
            s.liveReadFailed));
    }
    if (arr.isEmpty()) return {};
    return juce::JSON::toString(juce::var(arr), true);
}

juce::String ChainHost::buildMapFpsJson(int maxEntries) const
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    juce::StringArray conflicted;
    auto put = [&o, &conflicted](const juce::String& name, const juce::String& fp)
    {
        if (name.isEmpty() || fp.isEmpty()) return;
        const auto existing = o->getProperty(name).toString();
        if (existing.isEmpty())      o->setProperty(name, fp);
        else if (existing != fp)     conflicted.addIfNotAlreadyThere(name);
    };
    // Rack first: a loaded slot's fp is the exact binary. Recommendable
    // entries never touch a name the rack already claimed - the identity
    // index may legitimately know the OTHER format of a racked plugin, and
    // that is not a conflict, the rack is simply more exact.
    for (const auto& s : slots_)
        put(s.desc.name, s.fp);
    // Every feed entry lands in EXACTLY ONE bucket, in the same priority
    // order the original checks ran (cap, rack, name conflict, then the
    // join), so the counter line below reconciles against the feed count on
    // every turn -- edit turns and capped turns included. Once the cap
    // fires, everything after it counts capped, whatever it would have been.
    // rackWon vs the dup buckets (12 Aug 2026, from the 14:08 turn): the
    // claimed check reads the OUTPUT OBJECT, which the recommendable loop
    // itself fills, so a name already claimed by an EARLIER FEED ENTRY (the
    // feed carries duplicate names) hit the same branch as a rack claim and
    // 33 dup skips printed as rackWon against a rack of one fp-less
    // built-in. Same skip, two owners; rackNames says which.
    //
    // AND THE SKIP IS NOT PROVEN HARMLESS (Sean, same day): because it
    // fires before the join, a feed duplicate never reaches put, so put's
    // conflict detection is DEAD CODE for feed-versus-feed names - it can
    // only be populated by rack slots. A duplicate name is therefore
    // resolved to whichever entry came first in SCAN ORDER, and scan order
    // is not evidence: asserting the wrong binary is worse than asserting
    // none, because the server's sibling merge is the honest serve for an
    // ambiguous name. Measured before fixed: the three dup buckets below
    // say whether first-wins ever disagrees with the duplicate's own fp.
    // If dupDiffFp stays zero on this machine, first-wins is harmless here
    // and this comment is the record; if not, those names get omitted the
    // way conflicted already omits rack conflicts, and nameConfl starts
    // counting something.
    juce::StringArray rackNames;
    for (const auto& s : slots_)
        if (s.desc.name.isNotEmpty() && s.fp.isNotEmpty())
            rackNames.addIfNotAlreadyThere(s.desc.name);
    int exact = 0, uidFb = 0, ambig = 0, miss = 0, noUid = 0,
        rackWon = 0, dupSameFp = 0, dupDiffFp = 0, dupUnresolved = 0,
        nameConfl = 0, capped = 0;
    for (const auto& e : recommendable_)
    {
        if (o->getProperties().size() >= maxEntries)               { ++capped;    continue; }
        const auto storedFp = o->getProperty(e.displayName).toString();
        if (storedFp.isNotEmpty())
        {
            if (rackNames.contains(e.displayName)) { ++rackWon; continue; }
            // Feed duplicate: resolve ITS fp and compare against what the
            // first-wins entry stored. Measurement only - the skip stands
            // until dupDiffFp is shown nonzero live.
            const auto dupFp = echojay::fpForIdentity(identityToFp_, e.desc);
            if (dupFp.isEmpty())          ++dupUnresolved;
            else if (dupFp == storedFp)   ++dupSameFp;
            else                          ++dupDiffFp;
            continue;
        }
        if (conflicted.contains(e.displayName))                    { ++nameConfl; continue; }
        auto outcome = echojay::FpLookup::miss;
        const auto fp = echojay::fpForIdentity(identityToFp_, e.desc, &outcome);
        switch (outcome)
        {
            case echojay::FpLookup::exact:       ++exact; break;
            case echojay::FpLookup::uidFallback: ++uidFb; break;
            case echojay::FpLookup::ambiguous:   ++ambig; break;
            case echojay::FpLookup::miss:        ++miss;  break;
            case echojay::FpLookup::noUid:       ++noUid; break;
        }
        if (fp.isNotEmpty()) put(e.displayName, fp);
    }
    // An ambiguous name (two fps claimed) is omitted entirely; the server's
    // sibling merge is the honest serve for it.
    for (const auto& n : conflicted) o->removeProperty(n);
    // The counter at the decision: the server's fpKnown: 0 has exactly one
    // client-side counterpart, and it is this line. Printed BEFORE the
    // empty-object return so the zero case still measures itself.
    EchoJay_NSLog(("EJMapFps: feed=" + juce::String((int) recommendable_.size())
                   + " exact=" + juce::String(exact)
                   + " uidFb=" + juce::String(uidFb)
                   + " ambig=" + juce::String(ambig)
                   + " miss=" + juce::String(miss)
                   + " noUid=" + juce::String(noUid)
                   + " rackWon=" + juce::String(rackWon)
                   + " dupSameFp=" + juce::String(dupSameFp)
                   + " dupDiffFp=" + juce::String(dupDiffFp)
                   + " dupUnresolved=" + juce::String(dupUnresolved)
                   + " nameConfl=" + juce::String(nameConfl)
                   + " capped=" + juce::String(capped)
                   + " -> " + juce::String(o->getProperties().size()) + " entr(ies)").toRawUTF8());
    if (o->getProperties().size() == 0) return "{}";
    return juce::JSON::toString(juce::var(o.get()), true);
}

std::vector<ChainHost::SlotDialInfo> ChainHost::getDialInfos() const
{
    std::vector<SlotDialInfo> out;
    out.reserve(slots_.size());
    for (int i = 0; i < (int) slots_.size(); ++i)
    {
        const auto& s = slots_[(size_t) i];
        SlotDialInfo di;
        // dial-4 A8.1b: status cannot distinguish a built-in (its apply sets
        // applied/partial/unusableMap too), so the flag travels explicitly.
        di.builtin      = isBuiltinSlot(i);
        di.name         = s.desc.name;
        di.fp           = s.fp;
        di.status       = s.dialStatus;
        di.manual       = s.dialManual;
        di.readbackMiss = s.dialReadbackMiss;
        di.unconfirmed  = s.dialUnconfirmed;
        di.appliedCount = s.dialAppliedCount;
        di.staleIndexedFp = s.staleIndexedFp;
        di.outOfRange   = s.dialOutOfRange;
        // dial-3 key halves + denominator (A2/A3/A7.2). uid rendered
        // exactly as getSlotIdentity renders it, so the two surfaces
        // cannot disagree about the same slot.
        di.format = s.desc.pluginFormatName;
        if (s.desc.uniqueId != 0)
            di.uid = juce::String::toHexString(s.desc.uniqueId);
        // A8.8: unusableMap is keys-sourced BY DESIGNATION, even when the
        // apply loop ran (report empty, or everything manual): its report
        // count is 0 or short on exactly the turns the reason describes, and
        // requested 0 both hid the row from the batch builder and would be
        // barred from every rate by the A7.1 reader rule. The pre-apply entry
        // count is the honest denominator for "map exists, nothing usable".
        // A9 step 3: the exact translation of the old
        // "!= DialStatus::unusableMap" — all four values that name inherited,
        // so the A8.8 designation covers every one of them (§3a).
        const bool wasUnusableMap = (s.dialStatus == DialStatus::mapNoCoverage
                                  || s.dialStatus == DialStatus::writesRejected
                                  || s.dialStatus == DialStatus::mapIdentityMismatch
                                  || s.dialStatus == DialStatus::builtinPayloadUnmatched);
        if (s.dialRequestedCount >= 0 && ! wasUnusableMap)
        {
            di.requestedCount  = s.dialRequestedCount;
            di.requestedSource = "apply";
        }
        else
        {
            // The apply never ran (no_map / fetch timeout / stale rungs), or
            // unusableMap per above: the denominator is the pre-apply count
            // of requested entries — A7.2's entry semantic via the ONE shared
            // implementation (A8.1a: the old top-level property count here
            // made a controls object with five entries count as one, so the
            // tally would inherit a denominator the rows' numerator can
            // structurally exceed).
            di.requestedCount  = echojay::requestedEntryCount(s.structuredSettings);
            di.requestedSource = "keys";
        }
        out.push_back(std::move(di));
    }
    return out;
}

bool ChainHost::dialStateSettled() const
{
    // A failed/never-answered fetch leaves a slot pending forever; the
    // bubble side caps its wait and falls back to conservative wording, so
    // this never needs its own timeout.
    for (const auto& s : slots_)
        if (s.dialStatus == DialStatus::pending) return false;
    return true;
}

// The uid+manufacturer key, version dropped: the same key the server's
// existence index and version-insensitive lookup use. uid 0 is no identity.
bool ChainHost::feedSplitEnabled() const
{
    return feedSplitFlagFile().existsAsFile();
}

std::vector<echojay::IdentityRef> ChainHost::recommendableIdentityRefs() const
{
    std::vector<echojay::IdentityRef> refs;
    std::set<juce::String> seen;
    for (const auto& e : recommendable_)
    {
        if (e.desc.uniqueId == 0) continue;   // no uid: cannot be matched at the server
        const auto ik = echojay::identityKeyForDescription (e.desc);
        if (seen.insert (ik).second)
            refs.push_back ({ ik, e.desc.manufacturerName });
    }
    return refs;
}

void ChainHost::setExistenceDialable (std::set<juce::String> keys)
{
    existenceDialable_ = std::move (keys);
    existenceOk_ = true;
    EchoJay_NSLog(("EJFeed: existence index applied, " + juce::String((int) existenceDialable_.size())
                   + " dialable identit(ies)").toRawUTF8());
}

juce::StringArray ChainHost::getDialableRecommendableNames() const
{
    juce::StringArray out;
    for (const auto& e : recommendable_)
    {
        // LOCAL: an fp resolved (same helper as buildMapFpsJson, so the two
        // sites cannot drift; mapfps_test pins that) and a usable map present.
        bool dialable = false;
        if (const auto fp = echojay::fpForIdentity(identityToFp_, e.desc); fp.isNotEmpty())
            if (auto m = paramMaps_.find(fp); m != paramMaps_.end() && echojay::mapIsDialableForSignals(m->second))
                dialable = true;
        // EXISTENCE INDEX: a map exists on the server for this plugin at SOME
        // version, reachable through the server's version-insensitive lookup
        // even with no local fp. This is what carries a fresh or a V15 machine,
        // where the exact fp never matches the stored version.
        if (! dialable && e.desc.uniqueId != 0)
            if (existenceDialable_.count (echojay::identityKeyForDescription (e.desc)) > 0)
                dialable = true;
        if (dialable) out.addIfNotAlreadyThere (e.displayName);
    }
    return out;
}

void ChainHost::loadByRecommendedName(const juce::String& name,
                                       LoadOrigin origin,
                                       std::function<void(const juce::String&)> callback)
{
    juce::String nameLower = name.toLowerCase().trim();
    for (const auto& e : recommendable_)
    {
        if (e.displayName.toLowerCase().trim() == nameLower)
        {
            // NEW instantiation — popout-only AUs may swap to their VST3 build
            loadPluginAsync(preferInlineHostableDesc(e.desc), origin, std::move(callback));
            return;
        }
    }

    // Loose fallback via the shared resolver — handles "Name (Manufacturer)"
    // strings and punctuation/version drift the exact match above misses.
    // Same resolution both hosts use, honouring the active format filter.
    {
        juce::String matchLog;
        WithholdReason why = WithholdReason::None;
        auto d = resolveByName(name, recommendableFormat_, &matchLog, &why);
        // Log which stage resolved (or failed) so a future mis-resolution -
        // like "AMEK EQ 250" landing on "AMEK EQ 200" - is diagnosable from
        // the unified log instead of invisible.
        EchoJay_NSLog(("EJChain: resolve \"" + name + "\" -> " + matchLog).toRawUTF8());
        if (d.name.isNotEmpty())
        {
            loadPluginAsync(preferInlineHostableDesc(d), origin, std::move(callback));
            return;
        }
        // Honest miss (WithholdReason): the name matched a row this host
        // withholds. Say which, not "not found".
        if (why != WithholdReason::None)
        {
            callback("\"" + name + "\" " + withholdReasonText(why));
            return;
        }
    }

    callback("\"" + name + "\" not found in recommendable list");
}

// ---------------------------------------------------------------------------
// Persistence — plugin list cache
// ---------------------------------------------------------------------------
void ChainHost::saveToDisk() const
{    // Borrowed mode is READ-ONLY on every shared file (spec §2.2): one
    // writer per file, and the primary host is it.
    if (mode_ == Mode::Borrowed) return;

    appSupportDir().createDirectory();
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    auto xml = knownPlugins_.createXml();
    if (xml) xml->writeTo(getPluginListFile());
    // chain_blacklist.txt is deliberately NOT written here: addToBlacklist
    // persists it immediately at add time, and nothing else writes it, so a
    // user's hand deletion of a line (the only recovery path until the
    // Settings UI exists) is never rewritten from stale memory.
}

void ChainHost::loadFromDisk()
{
    auto listFile = getPluginListFile();
    if (listFile.existsAsFile())
    {
        auto xmlDoc = juce::XmlDocument::parse(listFile);
        if (xmlDoc)
        {
            std::lock_guard<std::mutex> lock(pluginsMutex_);
            knownPlugins_.recreateFromXml(*xmlDoc);
        }
    }

    // Full-entries cache: written by whichever host scanned last. Loading it
    // means THIS host can resolve chain names immediately, without its own
    // scan — both hosts share one list.
    {
        auto ecFile = getEntriesCacheFile();
        if (ecFile.existsAsFile())
        {
            if (auto doc = juce::XmlDocument::parse(ecFile);
                doc != nullptr && doc->getTagName() == "CHAIN_ENTRIES")
            {
                juce::Array<juce::PluginDescription> loaded;
                for (auto* c : doc->getChildIterator())
                {
                    juce::PluginDescription d;
                    if (d.loadFromXml(*c)) loaded.add(d);
                }
                if (!loaded.isEmpty())
                {
                    std::lock_guard<std::mutex> lock(pluginsMutex_);
                    entries_ = loaded;
                }
                entriesScannedAtMs_ = doc->getStringAttribute("scannedAt").getLargeIntValue();
                EchoJay_NSLog(("EJScan: cache loaded, " + juce::String(loaded.size())
                               + " entr(ies), scanned "
                               + (entriesScannedAtMs_ > 0
                                      ? juce::Time(entriesScannedAtMs_).toString(true, true)
                                      : juce::String("UNKNOWN (unstamped cache)"))).toRawUTF8());
                // Identity captured in past sessions survives the thin cache:
                // knownPlugins_ loaded above, entries_ just loaded, fill now
                // rather than waiting for a scan.
                if (const int filled = enrichThinVst3EntriesFromKnown(); filled > 0)
                {
                    hasResolved_ = false;
                    EchoJay_NSLog(("EJScan: " + juce::String(filled)
                                   + " thin VST3 entr(ies) enriched from load-captured identities").toRawUTF8());
                }
                computeSupersessions();
            }
            entriesCacheTime_ = ecFile.getLastModificationTime();
        }
    }
    reloadBlacklistFromDisk();
    reloadStateOversizeFromDisk();
}

// ---------------------------------------------------------------------------
// Settings too large to save: chain_state_oversize.txt is the authority
// (path<TAB>bytes<TAB>ISO date). Read at construction and at every scan,
// like the blacklist; deleting a line offers the plugin again on the next
// scan. Written by recordStateOversize only.
// ---------------------------------------------------------------------------
void ChainHost::reloadStateOversizeFromDisk()
{
    juce::StringArray lines;
    if (auto f = getStateOversizeFile(); f.existsAsFile())
        lines = juce::StringArray::fromLines(f.loadFileAsString());
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    stateOversize_.clear();
    for (auto& raw : lines)
    {
        auto line = raw.trim();
        if (line.isEmpty() || line.startsWithChar('#')) continue;
        auto path  = line.upToFirstOccurrenceOf("\t", false, false).trim();
        auto rest  = line.fromFirstOccurrenceOf("\t", false, false).trim();
        const int bytes = rest.upToFirstOccurrenceOf("\t", false, false).trim().getIntValue();
        if (path.isEmpty() || bytes <= 0) continue;
        stateOversize_[path] = bytes;
    }
}

int ChainHost::oversizeStateBytes(const juce::String& path) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    auto it = stateOversize_.find(path);
    return it == stateOversize_.end() ? 0 : it->second;
}

void ChainHost::recordStateOversize(const juce::String& path, int bytes, const juce::String& name, const juce::String& where)
{    // Borrowed mode is READ-ONLY on every shared file (spec §2.2): one
    // writer per file, and the primary host is it.
    if (mode_ == Mode::Borrowed) return;

    if (path.isEmpty() || bytes <= kSessionStateMaxSlotBytes) return;
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        auto it = stateOversize_.find(path);
        if (it != stateOversize_.end() && it->second >= bytes) return;   // already recorded, no smaller
        stateOversize_[path] = bytes;
    }
    auto f = getStateOversizeFile();
    juce::String text = f.existsAsFile() ? f.loadFileAsString() : juce::String();
    if (text.isEmpty())
        text = "# EchoJay: plugins whose settings at their defaults are larger than the per-plugin\n"
               "# session cap, so a chain holding them could not be saved with the project.\n"
               "# They are withheld from the chain list. Deleting a line offers that plugin again\n"
               "# on the next scan. Format: path<TAB>bytes<TAB>ISO date.\n";
    // one line per path: drop an older line for the same path
    juce::StringArray kept;
    for (auto& raw : juce::StringArray::fromLines(text))
        if (raw.trim().isEmpty() || raw.startsWithChar('#') || raw.upToFirstOccurrenceOf("\t", false, false).trim() != path)
            if (raw.trim().isNotEmpty()) kept.add(raw);
    kept.add(path + "\t" + juce::String(bytes) + "\t" + juce::Time::getCurrentTime().toISO8601(true));
    f.getParentDirectory().createDirectory();
    f.replaceWithText(kept.joinIntoString("\n") + "\n");
    EchoJay_NSLog(("EJScan: \"" + name + "\" settings are " + juce::File::descriptionOfSizeInBytes((juce::int64) bytes)
                   + " at their defaults, over the " + juce::File::descriptionOfSizeInBytes((juce::int64) kSessionStateMaxSlotBytes)
                   + " per-plugin session cap (" + where + "); recorded in chain_state_oversize.txt, withheld from the chain list").toRawUTF8());
}

// ---------------------------------------------------------------------------
// Persistence — chain slot state (save/restore all slots in order)
// ---------------------------------------------------------------------------
juce::String ChainHost::getSlotsStateXml() const
{
    auto root = std::make_unique<juce::XmlElement>("CHAIN_SLOTS");
    root->setAttribute("masterWet", (double)getMasterWet());
    for (auto& s : slots_)
    {
        auto* item = root->createNewChildElement("SLOT");
        item->setAttribute("bypassed", s.bypassed ? 1 : 0);
        item->setAttribute("wet", (double)s.wet);
        if (auto descXml = s.desc.createXml())
            item->addChildElement(descXml.release());
    }
    return root->toString();
}

void ChainHost::restoreNextSlot(std::vector<RestoreItem> items, int idx,
                                std::function<void()> onSlotSettled)
{
    if (idx >= (int)items.size()) return;
    bool  wasBypassed = items[idx].bypassed;
    float savedWet    = items[idx].wet;
    // Carried into the callback with the item, never looked up by index
    // afterwards, see the RestoreItem comment in the header.
    juce::String stateB64   = items[idx].stateBase64;
    bool         expectState = items[idx].expectState;
    juce::String slotName   = items[idx].desc.name;
    // The deadman names the plugin by this, so it rides with the item too.
    juce::String identifier = items[idx].desc.fileOrIdentifier;
    bool         withholdState = items[idx].withholdState;
    // The saved identity, carried by value for the apply-time re-check — never
    // looked up by index afterwards, per the comment above.
    juce::String savedFormat  = items[idx].savedFormat;
    juce::String savedVersion = items[idx].savedVersion;
    juce::String savedUid     = items[idx].savedUid;
    juce::var slotParams = items[idx].params;
    // RESTORE, and the same for both callers: a session reload
    // (tryRestoreSlotsFromXml) and a saved-chain recall (restoreSavedChain).
    // Neither is EchoJay building anything, and before this each wrote one
    // BUILT line per slot at turn 0.
    loadPluginAsync(items[idx].desc, LoadOrigin::Restore,
        [this, items = std::move(items), idx, wasBypassed, savedWet,
         stateB64, expectState, slotName, identifier, withholdState,
         savedFormat, savedVersion, savedUid, slotParams,
         onSlotSettled](const juce::String& err) mutable
        {
            if (err.isEmpty())
            {
                int lastSlot = (int)slots_.size() - 1;
                if (lastSlot >= 0)
                {
                    setSlotWet(lastSlot, savedWet, WetSource::Restore);
                    if (wasBypassed) setSlotBypassed(lastSlot, true);
                    // Withheld chunks were already explained by the note that
                    // decided it (restoreSavedChain); no second line here.
                    if (!withholdState)
                        applyRestoredState(lastSlot, stateB64, expectState, slotName,
                                           identifier, savedFormat, savedVersion, savedUid);
                    // Blob first, then the JUCE-side parameter values (VST3
                    // only): see getCachedSlotParamsVar in the header.
                    // applyRestoredParams re-checks identity (uid + format)
                    // itself, so it runs even when the chunk was withheld.
                    applyRestoredParams(lastSlot, slotParams, slotName);
                    // Running level saved for this slot number (session
                    // restore only; a saved-chain load has nothing pending)
                    if (auto it = pendingSlotLevels_.find(idx + 1); it != pendingSlotLevels_.end())
                    {
                        if (auto* b = slots_[(size_t) lastSlot].blendNode
                                        ? dynamic_cast<SlotWetBlend*>(slots_[(size_t) lastSlot].blendNode->getProcessor()) : nullptr)
                            b->restoreTallies(it->second.first, it->second.second);
                        pendingSlotLevels_.erase(it);
                    }
                }
            }
            else
            {
                // A load failure means "can't authorise right now", NEVER
                // "not owned" (an absent iLok fails plugins the user owns).
                // Session-scoped and named, so the gap in the rack is never
                // silent, but nothing is persisted and nothing is excluded.
                addStateNote(slotName + ": could not load on this machine right now,"
                                        " so this slot was left out of the chain");
            }
            if (onSlotSettled) onSlotSettled();
            restoreNextSlot(std::move(items), idx + 1, onSlotSettled);
        });
}

bool ChainHost::stateFitsPlugin(const juce::PluginDescription& found,
                                const juce::String& savedFormat,
                                const juce::String& savedVersion,
                                const juce::String& savedUid,
                                const juce::String& slotName,
                                juce::String* deferredNote,
                                bool noteOnWithhold) const
{
    if (deferredNote != nullptr) *deferredNote = {};

    // ABSENT IS NO OPINION, on EITHER side. A field the saved chain never
    // carried is an empty string; a thin VST3 row carries uid 0 and no version
    // until it validates. Comparing against either would withhold a chain from
    // its own plugin, so a field is only judged when BOTH sides carry it.
    const bool foundUidKnown     = found.uniqueId != 0;
    const bool foundVersionKnown = found.version.isNotEmpty();

    // FORMAT. A chunk is format-bound: a VST3 chunk does not load into the AU
    // build of the same plugin, and pushing it anyway is worse than pushing
    // nothing — best case ignored, realistic case garbage parameters that
    // sound wrong and look deliberate, worst case a dead host.
    if (savedFormat.isNotEmpty() && ! savedFormat.equalsIgnoreCase(found.pluginFormatName))
    {
        if (noteOnWithhold) addStateNote(slotName + ": saved as " + savedFormat + " but loaded here as "
                     + found.pluginFormatName
                     + ", so its settings were not applied (settings do not"
                       " transfer between formats)");
        return false;
    }

    // UID. Same name, same format, different plugin. A rescan can change a uid,
    // but so can two plugins sharing a name, and we cannot tell which from
    // here. Withholding costs one line of text; guessing costs the rack.
    if (savedUid.isNotEmpty() && foundUidKnown && savedUid != juce::String(found.uniqueId))
    {
        if (noteOnWithhold) addStateNote(slotName + ": this is a different build from the one saved,"
                                " so its settings were not applied");
        return false;
    }

    // VERSION. ATTEMPTED, deliberately, and said out loud — most plugins
    // version their own chunks and tolerate an older one; some do not. The
    // deadman in applyRestoredState covers the call, so the bad case is one
    // plugin recorded and withheld on the next launch rather than a session
    // that will not open. The note is DEFERRED to the caller, written only
    // after the chunk applied, because "never claim a dial you did not
    // perform" cuts both ways: we DID perform it, and the user should check it.
    if (savedVersion.isNotEmpty() && foundVersionKnown && savedVersion != found.version)
    {
        if (deferredNote != nullptr)
            *deferredNote = slotName + ": saved from version " + savedVersion
                          + ", this machine has " + found.version
                          + " — settings were applied, worth checking";
    }
    return true;
}

void ChainHost::applyRestoredState(int slotIdx, const juce::String& b64,
                                   bool expectState, const juce::String& slotName,
                                   const juce::String& identifier,
                                   const juce::String& savedFormat,
                                   const juce::String& savedVersion,
                                   const juce::String& savedUid)
{
    if (b64.isEmpty())
    {
        // Nothing saved for this slot. Say so ONLY when the session carried
        // settings for the chain at all: on a session written before hosted
        // settings were persisted there is nothing to have lost, and noting
        // every slot would be noise on every pre-existing project.
        if (expectState)
            addStateNote(slotName + ": loaded at its default settings"
                                    " (no settings were saved for this slot)");
        return;
    }

    juce::MemoryOutputStream mo;
    if (!juce::Base64::convertFromBase64(mo, b64))
    {
        addStateNote(slotName + ": saved settings could not be read,"
                                " so it loaded at its defaults");
        return;
    }

    auto* proc = getSlotProcessor(slotIdx);
    if (proc == nullptr)
    {
        addStateNote(slotName + ": saved settings could not be applied,"
                                " so it loaded at its defaults");
        return;
    }

    // THE AUTHORITATIVE RE-CHECK, against the description the plugin ACTUALLY
    // loaded with — before the death mark, before the call. A thin VST3
    // resolved to uid 0 at restore time, so the resolve-time check had no
    // opinion; by now slots_[slotIdx].desc carries the validated uid, format
    // and version, and this is the only place the saved identity can be judged
    // against the real one. Same policy, one function. The version-differs note
    // is deferred and written below, only once the chunk has applied.
    juce::String versionNote;
    if (slotIdx >= 0 && slotIdx < (int)slots_.size()
        && ! stateFitsPlugin(slots_[(size_t)slotIdx].desc, savedFormat, savedVersion,
                             savedUid, slotName, &versionNote))
        return;   // the withhold note was written by stateFitsPlugin

    // THE DEATH MARK, over the one call in this file that runs third-party
    // code on data we did not author. A try/catch cannot catch a segfault, and
    // a plugin mis-parsing a chunk segfaults — that is the whole failure mode.
    // If this call never returns, the mark survives the crash and the next
    // launch blacklists the plugin naming THIS phase. Per slot, not per chain:
    // the mark has to name the plugin that actually died.
    const int mark = (slotIdx >= 0 && slotIdx < (int)slots_.size())
                       ? pushDeathMark("state restore", slots_[(size_t)slotIdx].desc) : 0;

#if ECHOJAY_DEV_TRANSPORT
    // DEV-ONLY forced withhold: fail this slot's seed exactly as a real
    // refusal would — note written, seed fact NOT recorded, so everything
    // downstream (banner, verdicts, Apply's filter) runs the true path.
    if (mode_ == Mode::Borrowed && devForceWithholdSlot1() == slotIdx + 1)
    {
        popDeathMark(mark);
        addStateNote(slotName + ": [DEV forceWithholdSlot] seed deliberately"
                                " failed - running at defaults");
        EchoJay_NSLog(("EJBorrow: DEV forceWithholdSlot fired on slot "
                       + juce::String(slotIdx + 1)).toRawUTF8());
        return;
    }
#endif

    bool applied = false;
    try
    {
        proc->setStateInformation(mo.getData(), (int)mo.getDataSize());
        applied = true;
    }
    catch (...)
    {
        popDeathMark(mark);
        addStateNote(slotName + ": rejected its saved settings,"
                                " so it loaded at its defaults");
    }
    popDeathMark(mark);

    // Reuse verification, production arm (spec §1): a seed failing on an
    // instance that came FROM the pool is a REUSE failure — that identity
    // goes pool-ineligible for the session and later borrows instantiate
    // fresh, loudly. On a fresh instance the same failure is just a bad
    // state and marks nothing.
    if (mode_ == Mode::Borrowed && slotIdx >= 0 && slotIdx < (int) slots_.size()
        && slots_[(size_t) slotIdx].node != nullptr
        && borrowReusedNodeIds_.count(slots_[(size_t) slotIdx].node->nodeID.uid) > 0)
    {
        bool readbackOk = applied;
        if (applied)
        {
            // Self-check: a reused instance that accepted the seed must be
            // able to read its state back. Empty or throwing readback means
            // reuse left it incoherent even though setState "succeeded".
            juce::MemoryBlock rb;
            try { proc->getStateInformation(rb); }
            catch (...) { rb.reset(); }
            readbackOk = rb.getSize() > 0;
        }
        if (! readbackOk)
            markBorrowPoolIneligible(slots_[(size_t) slotIdx].desc,
                                     applied ? "state readback failed after reuse seed"
                                             : "rejected its state on reuse");
    }

    if (!applied) return;

    // The seed FACT, recorded at the only place it happens (step 3): this
    // slot now carries the restored state. Apply's withheld verdict reads
    // this, never a recomputed policy.
    if (mode_ == Mode::Borrowed && slotIdx >= 0 && slotIdx < (int) slots_.size()
        && slots_[(size_t) slotIdx].node != nullptr)
        borrowSeededNodeIds_.insert(slots_[(size_t) slotIdx].node->nodeID.uid);

    // The version note, NOW: after the chunk applied and none of the four
    // failure returns above (decode, null proc, throw, and the load failure in
    // restoreNextSlot that never reaches here) fired. Written here rather than
    // at resolve time so it can never stand next to a note saying the settings
    // did NOT apply.
    if (versionNote.isNotEmpty()) addStateNote(versionNote);

    // Seed the cache with what we just restored, so a save that happens
    // before the first capture round-trips this slot instead of nulling it.
    {
        std::lock_guard<std::mutex> lock(stateCacheMutex_);
        if (slotIdx >= 0 && slotIdx < (int)slots_.size())
        {
            auto& s = slots_[(size_t)slotIdx];
            s.lastKnownState = b64;
            s.lastKnownBytes = (int)mo.getDataSize();
            s.capturedAtMs   = juce::Time::getMillisecondCounterHiRes();
        }
    }
}

void ChainHost::applyRestoredParams(int slotIdx, const juce::var& params, const juce::String& slotName)
{
    if (params.isVoid()) return;                        // absent: nothing, the old restore
    if (slotIdx < 0 || slotIdx >= (int)slots_.size()) return;

    // Identity gate. The entry must be the {uid,format,plugin,params} object;
    // a bare string (no identity) or a missing / non-integer uid is skipped,
    // never applied. The values apply only when the plugin that actually
    // resolved into this slot IS the one they were saved for.
    auto* entry = params.getDynamicObject();
    if (entry == nullptr) return;                       // a bare string value: no identity, skip
    const juce::var uidVar = entry->getProperty("uid");
    if (! uidVar.isString()) return;                    // missing / non-string uid: no identity, skip
    const juce::String savedUid  = uidVar.toString();
    if (savedUid.isEmpty()) return;                     // empty uid: no identity, skip
    const juce::String savedFmt  = entry->getProperty("format").toString();
    const juce::String savedName = entry->getProperty("plugin").toString();
    const juce::String payload   = entry->getProperty("params").toString();
    if (payload.isEmpty()) return;

    // String compare, byte-identical to the slot uid buildChainSlotsVar writes
    // (juce::String(uniqueId)); the server keys stateParams to slots the same way.
    const auto& slotDesc = slots_[(size_t)slotIdx].desc;
    if (juce::String(slotDesc.uniqueId) != savedUid || slotDesc.pluginFormatName != savedFmt)
    {
        addStateNote(slotName + ": its saved parameter values were for \"" + savedName + "\" ("
                     + savedFmt + "), not the plugin in this slot, so they were not applied");
        return;
    }
    auto* proc = getSlotProcessor(slotIdx);
    if (proc == nullptr) return;
    const juce::String& params2 = payload;   // the "id=value,..." string, below

    // ID -> parameter, once
    std::unordered_map<std::string, juce::AudioProcessorParameter*> byId;
    for (auto* p : proc->getParameters())
        if (auto* hp = dynamic_cast<juce::HostedAudioProcessorParameter*>(p))
            byId.emplace(hp->getParameterID().toStdString(), p);

    int applied = 0, listed = 0, unknown = 0;
    juce::StringArray pairs;
    pairs.addTokens(params2, ",", "");
    for (const auto& pr : pairs)
    {
        const int eq = pr.indexOfChar('=');
        if (eq <= 0) continue;
        ++listed;
        auto it = byId.find(pr.substring(0, eq).toStdString());
        if (it == byId.end()) { ++unknown; continue; }   // a parameter this build no longer has
        const float v = juce::jlimit(0.0f, 1.0f, (float) pr.substring(eq + 1).getDoubleValue());
        // setValueNotifyingHost: JUCE's cache and dispatcher, so the
        // controller sees it now and the processor at the next process call.
        // Only where it differs, so a value the blob already carried is not
        // re-sent (and a plugin that derives a value from its own state keeps
        // it when the cache agrees).
        if (std::abs(it->second->getValue() - v) > 1.0e-6f)
        {
            it->second->setValueNotifyingHost(v);
            ++applied;
        }
    }
    EchoJay_NSLog(("EJChain: \"" + slotName + "\" restored " + juce::String(applied) + " parameter value(s) beside its state ("
                   + juce::String(listed) + " listed, " + juce::String(unknown) + " unknown to this build)").toRawUTF8());
    if (listed > 0 && unknown == listed)
        addStateNote(slotName + ": none of its saved parameter values matched this build of the plugin,"
                                " so only its saved state was applied");
}

void ChainHost::tryRestoreSlotsFromXml(const juce::String& xml,
                                       const juce::var& slotStates,
                                       const juce::var& slotParams)
{
    if (xml.isEmpty()) return;
    auto root = juce::XmlDocument::parse(xml);
    if (!root || root->getTagName() != "CHAIN_SLOTS") return;

    // One session restore, one clean slate. Notes are about THIS session's
    // chain; carrying yesterday's project's notes into today's would name
    // slots that are not on screen.
    clearStateNotes();

    // Master wet applies immediately (independent of the async slot loads);
    // absent attribute (pre-wet/dry sessions) restores as fully wet.
    setMasterWet((float)root->getDoubleAttribute("masterWet", 1.0));

    // Sibling settings object, 1-based keys. Absent on every session written
    // before hosted settings were persisted, which is the common case and
    // restores exactly as it always did.
    auto* statesObj = slotStates.getDynamicObject();
    auto* paramsObj = slotParams.getDynamicObject();   // absent on every session before 17 Aug 2026

    std::vector<RestoreItem> items;
    for (auto* child : root->getChildIterator())
    {
        if (child->getTagName() != "SLOT") continue;
        bool  bypassed = child->getIntAttribute("bypassed", 0) != 0;
        float wet      = (float)child->getDoubleAttribute("wet", 1.0);
        auto* descElem = child->getFirstChildElement();
        if (!descElem) continue;
        juce::PluginDescription desc;
        if (!desc.loadFromXml(*descElem)) continue;
        RestoreItem item { desc, bypassed, wet, {}, statesObj != nullptr };
        // 1-based, matching the shared chain format and the API's `state`
        // object. Position in the document is the slot number: keying by it
        // makes a skipped slot explicit rather than inferred.
        if (statesObj != nullptr)
        {
            const juce::String key ((int)items.size() + 1);
            if (statesObj->hasProperty(key))
                item.stateBase64 = statesObj->getProperty(key).toString();
        }
        if (paramsObj != nullptr)
        {
            const juce::String key ((int)items.size() + 1);
            if (paramsObj->hasProperty(key))
                item.params = paramsObj->getProperty(key);   // the {uid,format,plugin,params} object
        }
        items.push_back(std::move(item));
    }

    if (!items.empty())
        restoreNextSlot(std::move(items), 0);
}

// ---------------------------------------------------------------------------
// Hosted plugin settings: CACHE, not capture
//
// The host's getStateInformation only ever serialises strings already held
// here. Everything expensive (the hosted getStateInformation calls) happens
// ahead of time on the message thread: after a chain edit, on the debounce
// timer, and on editor teardown. See the header for why.
// ---------------------------------------------------------------------------
void ChainHost::setStateCacheEnabled(bool shouldBeEnabled)
{
    stateCacheEnabled_ = shouldBeEnabled;
    if (!shouldBeEnabled)
    {
        if (stateCacheTimer_) stateCacheTimer_->stopTimer();
        return;
    }
    for (int i = 0; i < (int)slots_.size(); ++i)
        attachHostedListener(i);
    if (stateCacheTimer_ == nullptr)
        stateCacheTimer_ = std::make_unique<StateCacheTimer>(*this);
    stateCacheTimer_->startTimer(kStateTickMs);
    noteHostedChange();   // first capture on the next settled tick
}

void ChainHost::onHostedLatencyChanged() noexcept
{
    // Any thread (a plugin may report a latency change from its own UI or
    // from the audio thread): nothing but a thread-safe trigger.
    if (latencyRebuilder_) latencyRebuilder_->triggerAsyncUpdate();
}

void ChainHost::rebuildForLatencyIfChanged()
{
    // Message thread, after the debounce. Rebuild ONLY if some slot's
    // reported latency differs from what the graph was built with: a
    // notification that changes nothing costs nothing, and a burst for one
    // switch already collapsed into this one call.
    bool changed = false;
    juce::String detail;
    for (size_t si = 0; si < slots_.size(); ++si)
    {
        auto* p = slots_[si].node ? slots_[si].node->getProcessor() : nullptr;
        if (p == nullptr) continue;
        const int now  = p->getLatencySamples();
        const int was  = si < builtLatencies_.size() ? builtLatencies_[si] : -1;
        if (now != was)
        {
            changed = true;
            detail << (detail.isEmpty() ? "" : ", ") << slots_[si].desc.name << " " << was << "->" << now;
        }
    }
    if (!changed) return;
    EchoJay_NSLog(("EJChain: hosted latency changed at runtime (" + detail
                   + "); rebuilding the graph so the wet/dry dry legs and the host "
                     "latency follow it").toRawUTF8());
    // Same rebuild every structural op takes: reconnects, the render
    // sequence re-bakes the delays from the new latencies, onChainChanged
    // re-mirrors getTotalLatencySamples into the host. Audible cost at the
    // instant: the new dry-leg delay buffers start empty, so a partially
    // wet slot loses its dry component for the plugin's latency (about
    // 20 ms at 1000 samples), once; a fully wet slot hears nothing.
    rebuildGraph();
}

void ChainHost::noteHostedChange() noexcept
{
    // Reachable from the audio thread during automation: three relaxed atomic
    // stores, nothing else. No container access, no allocation, no lock.
    lastChangeMs_.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    stateDirty_.store(true, std::memory_order_relaxed);
    // The epoch is a SEPARATE counter from stateDirty_ rather than a reuse of
    // it, because the two are consumed differently: the dirty flag is CLEARED
    // by the state cache once it has captured, so a second consumer testing it
    // would silently lose every change the cache happened to service first.
    // A monotonic counter each consumer compares against its own last-seen
    // value cannot be consumed out from under anyone.
    hostedEpoch_.fetch_add(1, std::memory_order_relaxed);
}

bool ChainHost::getBuiltinEqCurveDeciDb(int16_t* out, int n)
{
    if (out == nullptr || n != LinkShm::kEqCurvePoints) return false;

    // Identity by the FROZEN IDENTIFIER, never by display name: see
    // kBuiltinEqIdentifier. findFirstBuiltinEqSlot is deliberately "first":
    // a second EQ in the same rack is NOT drawn and its response is NOT
    // merged in, because combining two responses correctly means replicating
    // the cascade, which is the exact duplication that publishing a
    // precomputed curve exists to avoid.
    const int slot = findFirstBuiltinEqSlot();
    if (slot < 0) return false;

    auto* eq = dynamic_cast<SurgicalEqProcessor*>(getSlotProcessor(slot));
    if (eq == nullptr) return false;

    float freqs[LinkShm::kEqCurvePoints];
    float mags [LinkShm::kEqCurvePoints];
    LinkShm::eqCurveFreqs(freqs, n);
    eq->getEngine().getMagnitudeResponse(freqs, mags, n);

    for (int i = 0; i < n; ++i)
    {
        // A non-finite sample means the engine could not answer (an
        // unprepared slot, a degenerate filter). Publish NOTHING rather than
        // substituting a number: one fabricated point in a curve is a curve
        // that lies about one frequency, which is no better than lying about
        // all of them.
        if (!std::isfinite(mags[i])) return false;
        const float clampedDb = juce::jlimit(-(float)LinkShm::kEqCurveClampDeciDb * 0.1f,
                                              (float)LinkShm::kEqCurveClampDeciDb * 0.1f,
                                              mags[i]);
        out[i] = (int16_t) juce::roundToInt(clampedDb * 10.0f);
    }
    return true;
}

void ChainHost::attachHostedListener(int i)
{
    // NO stateCacheEnabled_ GATE, deliberately, and this is a fix rather than
    // a relaxation. The listener is the SIGNAL that a hosted plugin changed;
    // the state cache is only one CONSUMER of it, and gating the signal on
    // that one consumer meant the Link -- which never calls
    // setStateCacheEnabled -- could not observe a hosted knob move at all.
    // The rack sidecar's EQ curve is a second consumer and needs the same
    // signal. Cost of attaching regardless: audioProcessorParameterChanged
    // does two relaxed atomic stores. Everything that actually SERIALISES
    // state still checks stateCacheEnabled_ for itself (stateCacheTick and
    // refreshStateCacheIfIdle both return early), so the Link gains the
    // notifications without gaining the capture.
    if (i < 0 || i >= (int)slots_.size() || !slots_[(size_t)i].node) return;
    if (auto* p = slots_[(size_t)i].node->getProcessor())
    {
        p->removeListener(this);   // never double-register
        p->addListener(this);
    }
}

void ChainHost::detachHostedListener(int i)
{
    if (i < 0 || i >= (int)slots_.size() || !slots_[(size_t)i].node) return;
    if (auto* p = slots_[(size_t)i].node->getProcessor())
        p->removeListener(this);
}

void ChainHost::stateCacheTick()
{
    if (!stateCacheEnabled_ || slots_.empty()) return;

    const double now     = juce::Time::getMillisecondCounterHiRes();
    const bool   dirty   = stateDirty_.load(std::memory_order_relaxed);
    const bool   settled = (now - lastChangeMs_.load(std::memory_order_relaxed))
                             >= kStateDebounceMs;

    // Still moving: let the gesture finish rather than serialising mid-drag.
    if (dirty && !settled) return;

    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        auto& s = slots_[(size_t)i];
        const bool due = dirty
                      || s.capturedAtMs <= 0.0
                      || (now - s.capturedAtMs) >= kStateSweepMs;   // silent plugins
        if (!due || now < s.nextCaptureMs) continue;
        captureSlotState(i, now);
    }

    if (dirty) stateDirty_.store(false, std::memory_order_relaxed);
}

void ChainHost::refreshStateCacheIfIdle()
{
    if (!stateCacheEnabled_ || slots_.empty()) return;

    const double now   = juce::Time::getMillisecondCounterHiRes();
    const bool   dirty = stateDirty_.load(std::memory_order_relaxed);

    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        auto& s = slots_[(size_t)i];
        // Nothing observed since this slot's last capture: skip it entirely.
        // This is what makes the teardown refresh point free in Logic, where
        // the editor is recreated every time the user switches between the
        // Link window and EchoJay.
        if (!dirty && s.capturedAtMs > 0.0) continue;
        if (now < s.nextCaptureMs) continue;   // expensive slot, still backing off
        captureSlotState(i, now);
    }
    stateDirty_.store(false, std::memory_order_relaxed);
}

void ChainHost::captureAllSlotStatesNow()
{
    if (!stateCacheEnabled_ || slots_.empty()) return;

    const double now = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        // Deliberately ignores nextCaptureMs. The backoff protects the
        // BACKGROUND cadence from an expensive plugin; it must not make an
        // explicit Save quietly write a stale blob. If a sampler takes a
        // second here, that second belongs to the user's action.
        captureSlotState(i, now);
    }
    // Everything is current as of now, so no debounced pass is owed.
    stateDirty_.store(false, std::memory_order_relaxed);
}

void ChainHost::captureSlotState(int i, double nowMs)
{
    if (inStateCapture_) return;   // see inStateCapture_ in the header
    if (i < 0 || i >= (int)slots_.size()) return;
    auto* proc = slots_[(size_t)i].node ? slots_[(size_t)i].node->getProcessor() : nullptr;
    if (proc == nullptr) return;
    const juce::ScopedValueSetter<bool> capturing(inStateCapture_, true);

    // The hosted call happens with NO lock held. A plugin that blocks in
    // here blocks only this message-thread tick, never a save.
    juce::MemoryBlock mb;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    try { proc->getStateInformation(mb); }
    catch (...) { return; }   // keep whatever we last held for this slot
    const double cost = juce::Time::getMillisecondCounterHiRes() - t0;

    // Stored up to the LOOSEST consumer's cap, so one capture can serve both.
    // The tighter session cap is applied where the session is written, not
    // here: capping at storage time would silently make the API's larger cap
    // unreachable and there would be no second capture path to fall back on.
    const int  bytes    = (int)mb.getSize();
    const bool oversize = bytes > kStateStoreMaxSlotBytes;
    juce::String b64;
    if (bytes > 0 && !oversize)
        b64 = juce::Base64::toBase64(mb.getData(), mb.getSize());

    // VST3 only: the JUCE-side parameter values, read from the CACHE
    // (AudioProcessorParameter::getValue, the edited value), never from the
    // plugin's own state (stale until the next process call). See
    // getCachedSlotParamsVar in the header. Built off the lock, like the blob.
    juce::String params;
    if (slots_[(size_t)i].desc.pluginFormatName == "VST3")
    {
        juce::MemoryOutputStream ps;
        bool first = true;
        for (auto* p : proc->getParameters())
        {
            auto* hp = dynamic_cast<juce::HostedAudioProcessorParameter*>(p);
            if (hp == nullptr) continue;
            const auto id = hp->getParameterID();
            if (id.isEmpty() || id.containsAnyOf("=,")) continue;
            if (!first) ps.writeByte(',');
            first = false;
            ps << id << "=" << juce::String((double) p->getValue(), 7);
        }
        params = ps.toString();
        if (params.length() > kApiStateMaxSlotBytes) params.clear();   // never seen; a list, not a blob
    }

    juce::String note;
    {
        std::lock_guard<std::mutex> lock(stateCacheMutex_);
        auto& s = slots_[(size_t)i];
        s.lastKnownState  = b64;
        s.lastKnownBytes  = oversize ? 0 : bytes;
        s.lastKnownParams = params;
        s.lastCaptureMs   = cost;
        s.capturedAtMs    = nowMs;

        // Backoff. An expensive slot earns a long minimum interval so one
        // sampler cannot make the whole cache thrash; a slot over the cap is
        // retried rarely, since re-serialising it only to discard it again is
        // pure cost (it can still shrink, so this is a gate, not a ban).
        if (oversize)
            s.nextCaptureMs = nowMs + kStateOversizeMs;
        else if (cost >= kStateExpensiveMs)
            s.nextCaptureMs = nowMs + juce::jmax(5000.0, cost * kStateBackoffFactor);
        else
            s.nextCaptureMs = 0.0;

        if (oversize && !s.oversizeReported)
        {
            s.oversizeReported = true;
            note = s.desc.name + ": settings are "
                 + juce::File::descriptionOfSizeInBytes((juce::int64)bytes)
                 + ", over the " + juce::File::descriptionOfSizeInBytes(
                       (juce::int64)kStateStoreMaxSlotBytes)
                 + " limit, so they will not be saved";
        }
        else if (!oversize && s.oversizeReported)
        {
            s.oversizeReported = false;   // shrank back under the cap
        }
    }
    if (note.isNotEmpty()) addStateNote(note);
}

juce::var ChainHost::getCachedSlotParamsVar() const
{
    // Strings the cache already holds, nothing else (safe in a save callback)
    std::lock_guard<std::mutex> lock(stateCacheMutex_);
    auto* obj = new juce::DynamicObject();
    int n = 0;
    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        const auto& s = slots_[(size_t)i];
        if (s.desc.pluginFormatName != "VST3" || s.lastKnownParams.isEmpty()) continue;
        // Identity-carrying object form (server contract, 17 Aug 2026): the
        // values apply on restore ONLY if the plugin that resolves into the
        // slot is the same one, so its identity travels with them. uid is a
        // STRING, juce::String(desc.uniqueId), byte-identical to what
        // buildChainSlotsVar writes for the slot's own uid: the server's
        // normalizeSlots runs every slot field through str(), so the slot
        // uid is a string by contract and an integer here would compare
        // against null. So this side matches it: a string compare on restore.
        auto* e = new juce::DynamicObject();
        e->setProperty("uid",    juce::String(s.desc.uniqueId));
        e->setProperty("format", s.desc.pluginFormatName);
        e->setProperty("plugin", s.desc.name);
        e->setProperty("params", s.lastKnownParams);
        obj->setProperty(juce::String(i + 1), juce::var(e));
        ++n;
    }
    juce::var out(obj);
    return n > 0 ? out : juce::var();
}

juce::var ChainHost::getCachedSlotStatesVar(int maxSlotBytes,
                                            int maxTotalBytes,
                                            const juce::String& consumer) const
{
    // Serialises the cache and NOTHING else: no call into a hosted plugin,
    // no work that can block, so this is safe inside the host's save
    // callback. The lock is EchoJay's own and is held for a few string
    // copies.
    struct Held { int n; juce::String b64; int bytes; juce::String name; };
    std::vector<Held> held;
    {
        std::lock_guard<std::mutex> lock(stateCacheMutex_);
        held.reserve(slots_.size());
        for (int i = 0; i < (int)slots_.size(); ++i)
        {
            const auto& s = slots_[(size_t)i];
            held.push_back({ i + 1, s.lastKnownState, s.lastKnownBytes, s.desc.name });
        }
    }
    if (held.empty()) return {};

    // Per-slot cap for THIS consumer. The cache may hold a blob that is fine
    // for one consumer and too big for the other; that is the whole reason
    // the caps are parameters.
    for (auto& h : held)
    {
        if (h.bytes > maxSlotBytes && h.bytes > 0)
        {
            addStateNote(h.name + ": settings were not saved with " + consumer
                       + " (they are "
                       + juce::File::descriptionOfSizeInBytes((juce::int64)h.bytes)
                       + ", over the "
                       + juce::File::descriptionOfSizeInBytes((juce::int64)maxSlotBytes)
                       + " limit)");
            h.b64   = {};
            h.bytes = 0;
        }
    }

    // Total cap: keep the smallest states that fit, drop the largest first,
    // and name what was dropped. Degrade, never fail.
    juce::int64 total = 0;
    for (auto& h : held) total += h.bytes;
    if (total > maxTotalBytes)
    {
        std::vector<int> byLargest;
        for (int i = 0; i < (int)held.size(); ++i)
            if (held[(size_t)i].bytes > 0) byLargest.push_back(i);
        std::sort(byLargest.begin(), byLargest.end(),
                  [&held](int a, int b) { return held[(size_t)a].bytes > held[(size_t)b].bytes; });
        for (int idx : byLargest)
        {
            if (total <= maxTotalBytes) break;
            auto& h = held[(size_t)idx];
            total -= h.bytes;
            addStateNote(h.name + ": settings were not saved with " + consumer
                       + " (the chain's settings exceeded "
                       + juce::File::descriptionOfSizeInBytes(
                             (juce::int64)maxTotalBytes) + " in total)");
            h.b64   = {};
            h.bytes = 0;
        }
    }

    // Every slot gets a key, including the ones that hold nothing. A chain
    // where NO slot captured must still write the object: it is the only
    // signal the restore has that this session was saved by a build that
    // persists settings, and therefore the only way it can tell the user why
    // their plugins came back at their defaults. Explicit null beats absent.
    auto obj = std::make_unique<juce::DynamicObject>();
    for (auto& h : held)
        obj->setProperty(juce::String(h.n),
                         h.b64.isEmpty() ? juce::var() : juce::var(h.b64));
    return juce::var(obj.release());
}

juce::var ChainHost::buildChainSlotsVar() const
{
    juce::Array<juce::var> arr;
    for (int i = 0; i < (int)slots_.size(); ++i)
    {
        const auto& s = slots_[(size_t)i];
        auto o = std::make_unique<juce::DynamicObject>();
        // 1-BASED and contiguous. The single 0-based-to-1-based conversion
        // for AI edit ops lives in parseChainEditOps; this is the other
        // boundary where the outside world counts from one.
        o->setProperty("n",            i + 1);
        o->setProperty("plugin",       s.desc.name);
        o->setProperty("manufacturer", s.desc.manufacturerName);
        o->setProperty("format",       s.desc.pluginFormatName);
        o->setProperty("version",      s.desc.version);
        o->setProperty("uid",          juce::String(s.desc.uniqueId));
        o->setProperty("bypassed",     s.bypassed);
        // Per-slot wet/dry (16 Aug 2026): a shared chain used to lose the
        // knob and reopen fully wet, the opposite of this product's
        // subtle-by-default. Written always; a reader treats absent as 1.0,
        // so chains saved before this line behave exactly as they did.
        // NOTE the server's slot normaliser (lib/dash/chains.js) whitelists
        // keys and drops this one until it learns it.
        o->setProperty("wet",          (double) s.wet);
        // The AI's prose dial-in guidance is the closest thing this rack has
        // to a role, and it is display text rather than a short label, so it
        // is NOT sent as one. An absent role is honest; an invented one would
        // put words in the model's mouth. Slot settings ride in `state`.
        o->setProperty("role",         juce::var());
        // Reserved for Phase 2. The server REJECTS a non-null params, so
        // this key must stay null until the param map work unblocks it.
        o->setProperty("params",       juce::var());
        arr.add(juce::var(o.release()));
    }
    return juce::var(arr);
}

void ChainHost::archiveCurrentRack(const juce::String& label)
{    // Borrowed mode is READ-ONLY on every shared file (spec §2.2): one
    // writer per file, and the primary host is it.
    if (mode_ == Mode::Borrowed) return;

    if (slots_.empty()) return;   // nothing populated: nothing to protect

    // Same capture path a deliberate library save uses (sendChainSave): force
    // a fresh capture, then serialise slots + per-slot state + the VST3 param
    // sidecar. Restore is the existing restoreSavedChain contract.
    captureAllSlotStatesNow();

    const juce::int64 now = juce::Time::getCurrentTime().toMilliseconds();

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("t",     (double) now);
    root->setProperty("label", label);
    root->setProperty("slots", buildChainSlotsVar());
    if (auto state = getCachedSlotStatesVar(kApiStateMaxSlotBytes, kApiStateMaxTotalBytes,
                                            "chain archive"); ! state.isVoid())
        root->setProperty("state", state);
    if (auto sp = getCachedSlotParamsVar(); ! sp.isVoid())
        root->setProperty("stateParams", sp);

    auto dir = appSupportDir().getChildFile("chain_archive");
    dir.createDirectory();

    auto safe = juce::File::createLegalFileName(label).substring(0, 40).trim();
    if (safe.isEmpty()) safe = "rack";
    auto f = dir.getChildFile(juce::String(now) + "_" + safe + ".json");
    f.replaceWithText(juce::JSON::toString(juce::var(root.release()), true));

    EchoJay_NSLog(("EJArchive: saved rack (" + juce::String((int) slots_.size())
                   + " slots) before overwrite -> " + f.getFileName()).toRawUTF8());

    // COUNT ring (keep newest 20) then a SIZE cap (prune oldest until under
    // budget). The ms-epoch filename prefix sorts lexically == chronologically,
    // so File's path order is oldest-first.
    auto files = dir.findChildFiles(juce::File::findFiles, false, "*.json");
    files.sort();
    constexpr int kKeep = 20;
    while (files.size() > kKeep) { files.getReference(0).deleteFile(); files.remove(0); }
    constexpr juce::int64 kMaxBytes = 100LL * 1024 * 1024;
    juce::int64 total = 0;
    for (auto& e : files) total += e.getSize();
    for (int i = 0; i < files.size() && total > kMaxBytes; ++i)
    {
        total -= files.getReference(i).getSize();
        files.getReference(i).deleteFile();
    }
}

void ChainHost::restoreSavedChain(const juce::var& slotsArr, const juce::var& stateObj,
                                  std::function<void()> onSlotSettled,
                                  std::function<bool(const juce::String&)> isDisabledByName,
                                  const juce::var& paramsObj)
{
    auto* arr = slotsArr.getArray();
    if (arr == nullptr || arr->isEmpty()) return;

    // One load, one clean slate: notes are about the chain now on screen.
    clearStateNotes();
    auto* statesObj = stateObj.getDynamicObject();
    auto* paramsMap = paramsObj.getDynamicObject();   // stateParams; absent until the server carries it

    std::vector<RestoreItem> items;
    for (int i = 0; i < arr->size(); ++i)
    {
        auto* o = (*arr)[i].getDynamicObject();
        if (o == nullptr) continue;
        const juce::String name = o->getProperty("plugin").toString().trim();
        if (name.isEmpty()) continue;

        // The SAVED slot number, not our position. State is keyed by it, and
        // a slot we cannot resolve must not shift anyone else's key.
        const int n = o->hasProperty("n") ? (int)o->getProperty("n") : (i + 1);

        WithholdReason why = WithholdReason::None;
        auto desc = resolveByName(name, {}, nullptr, &why);
        if (desc.name.isEmpty())
        {
            // NEVER "not owned": a plugin the user owns looks exactly like
            // this on a machine where it is not installed, or where it
            // cannot authorise right now. And never "not found" for a
            // plugin that IS here but this host withholds (an Intel-only
            // VST3 under arm64, a crash-blacklisted row): that is the one
            // miss the user can act on, so the note says which it was.
            addStateNote(name + (why == WithholdReason::None
                                     ? juce::String(": could not be found on this machine,")
                                     : " " + withholdReasonText(why) + ",")
                              + " so this slot was skipped");
            continue;
        }

        // Disabled-set check, against the RESOLVED name (mirrors the AI
        // build path's gate). Reported through the same state-note channel
        // as the not-found case: one mechanism, one place the user looks.
        if (isDisabledByName && isDisabledByName(desc.name))
        {
            addStateNote(name + ": is disabled in Settings,"
                                " so this slot was skipped");
            continue;
        }

        RestoreItem item;
        item.desc     = desc;
        item.bypassed = (bool)o->getProperty("bypassed");
        // Per-slot wet/dry: carried since 16 Aug 2026 (a chain saved
        // before then, or one the server normalised without it, has no
        // "wet" and restores fully wet, exactly as before).
        item.wet         = o->hasProperty("wet")
                             ? juce::jlimit(0.0f, 1.0f, (float)(double) o->getProperty("wet"))
                             : 1.0f;
        item.expectState = (statesObj != nullptr);
        if (statesObj != nullptr)
        {
            const juce::String key (n);
            if (statesObj->hasProperty(key))
                item.stateBase64 = statesObj->getProperty(key).toString();
        }

        // THE STATE-MATCH POLICY lives in stateFitsPlugin (one place, so it
        // cannot drift). Here it runs against the RESOLVED description, which
        // usefully declines a pointless load when the format is already wrong,
        // but has no opinion on a thin VST3 row (uid 0, no version until its
        // first load). applyRestoredState runs the SAME policy again against
        // the description the plugin actually loaded with, where a thin row's
        // real uid is finally known — that apply-time re-check is what closes
        // the VST3-chunk-into-a-different-build hole this resolve-time call
        // cannot see. The saved triplet rides on the item to that re-check.
        //
        // Decided only when there is a chunk to push: with nothing saved for
        // the slot there is nothing to withhold and nothing to attempt.
        // deferredNote is deliberately null here: the version note is written
        // by applyRestoredState after the chunk applies, never at resolve time
        // (a note must not claim a dial that has not happened yet, or that a
        // load failure downstream then contradicts).
        item.savedFormat  = o->getProperty("format").toString().trim();
        item.savedVersion = o->getProperty("version").toString().trim();
        item.savedUid     = o->getProperty("uid").toString().trim();
        if (item.stateBase64.isNotEmpty())
            item.withholdState = ! stateFitsPlugin(desc, item.savedFormat, item.savedVersion,
                                                   item.savedUid, name);
        if (paramsMap != nullptr)
        {
            const juce::String key (n);
            if (paramsMap->hasProperty(key))
                item.params = paramsMap->getProperty(key);   // the {uid,format,plugin,params} object
        }
        items.push_back(std::move(item));
    }

    if (!items.empty())
        restoreNextSlot(std::move(items), 0, std::move(onSlotSettled));
    else if (onSlotSettled)
        onSlotSettled();   // nothing resolved: the caller still has to react
}

void ChainHost::addStateNote(const juce::String& note) const
{
    std::lock_guard<std::mutex> lock(stateCacheMutex_);
    stateNotes_.addIfNotAlreadyThere(note);
}

juce::StringArray ChainHost::getStateNotes() const
{
    std::lock_guard<std::mutex> lock(stateCacheMutex_);
    return stateNotes_;
}

void ChainHost::clearStateNotes()
{
    std::lock_guard<std::mutex> lock(stateCacheMutex_);
    stateNotes_.clear();
}
