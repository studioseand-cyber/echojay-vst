#include "ChainHost.h"
#include "EchoJayParamApply.h"
#include "EchoJayParamMaps.h"
#include "AUEnumerator.h"
#include "NativeClip.h"   // EchoJay_NSLog
#include <algorithm>
#include <unordered_map>

#if JUCE_MAC
 #include <libproc.h>
 #include <dlfcn.h>
 #include <unistd.h>
 #include <sys/param.h>   // MAXCOMLEN
#endif

// Defined later in this file (used by the shared name resolution above it)
static juce::String normalizeName(const juce::String& raw);
// Trailing all-digits MODEL number (the token normalizeName strips as a
// "version"), or empty. Lets resolveByName keep "AMEK EQ 250" and "AMEK EQ
// 200" distinct while still tolerating genuine version suffixes.
static juce::String trailingModelNumber(const juce::String& raw);

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

juce::PluginDescription ChainHost::preferInlineHostableDesc(const juce::PluginDescription& d)
{
    if (d.pluginFormatName != "AudioUnit") return d;
    if (!isPopoutOnly(d.name, "AudioUnit")) return d;
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

    // 1. Direct VST3 entry / previously deep-scanned cache
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        for (auto& d : entries_)
            if (d.pluginFormatName == "VST3" && matches(d.name))
                return d;
        for (const auto& d : knownPlugins_.getTypes())
            if (d.pluginFormatName == "VST3" && matches(d.name))
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
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const float target = juce::jlimit(0.0f, 1.0f,
            wet_ ? wet_->load(std::memory_order_relaxed) : 1.0f);
        smooth_.setTargetValue(target);

        const int n = buffer.getNumSamples();
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

ChainHost::ChainHost()
{
    juce::addDefaultFormatsToManager(formatManager_);

    graph_ = std::make_unique<juce::AudioProcessorGraph>();

    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode_  = graph_->addNode(std::make_unique<IOProc>(IOProc::audioInputNode));
    outputNode_ = graph_->addNode(std::make_unique<IOProc>(IOProc::audioOutputNode));

    rebuildGraph(); // passthrough (no slots yet)
    loadFromDisk();
    loadParamMapsFromDisk();
    mergeBootstrapMaps();

    auto deadman = getDeadmanFile();
    if (deadman.existsAsFile())
    {
        juce::String crashed = deadman.loadFileAsString().trim();
        if (crashed.isNotEmpty()) addToBlacklist(crashed);
        deadman.deleteFile();
    }
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
    stateCacheEnabled_ = false;
    for (int i = 0; i < (int)slots_.size(); ++i)
        detachStateListener(i);
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
    if (!prepared_ || !graph_ || !hasActiveSlots_.load())
        return;   // buffer passes through untouched

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
}

// ---------------------------------------------------------------------------
// List refresh — no plugin instantiation
// ---------------------------------------------------------------------------
void ChainHost::startScan()
{
    if (scanning_.load()) return;
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
{
    struct Finally { ChainHost* h; ~Finally() { h->scanning_.store(false); } } fin{this};

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
    scanProgress_.store(0.5f);
#endif

    if (cancelFlag_.load()) { std::lock_guard<std::mutex> lk(pluginsMutex_); entries_ = auEntries; return; }

    setScanStatus("Reading VST3 folders...");
    auto* vst3Fmt = getFormatByName("VST3");
    if (vst3Fmt)
    {
        auto paths = vst3Fmt->getDefaultLocationsToSearch();
        for (int pi = 0; pi < paths.getNumPaths(); ++pi)
        {
            if (cancelFlag_.load()) break;
            auto dir = paths[pi];
            if (!dir.isDirectory()) continue;

            auto found = dir.findChildFiles(
                juce::File::findDirectories | juce::File::findFiles, false, "*.vst3");

            for (auto& f : found)
            {
                if (cancelFlag_.load()) break;
                juce::String path = f.getFullPathName();
                if (isBlacklisted(path)) continue;

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

    scanProgress_.store(0.9f);

    std::unordered_set<std::string> auNames;
    for (auto& d : auEntries)
        auNames.insert(d.name.toLowerCase().toStdString());

    juce::Array<juce::PluginDescription> collected;
    for (auto& d : auEntries) collected.add(d);
    for (auto& d : vst3Entries)
        if (auNames.find(d.name.toLowerCase().toStdString()) == auNames.end())
            collected.add(d);

    std::sort(collected.begin(), collected.end(),
              [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
                  return a.name.compareIgnoreCase(b.name) < 0;
              });

    {
        std::lock_guard<std::mutex> lk(pluginsMutex_);
        entries_ = collected;
    }

    // Persist the FULL entries list so the other host (main plugin / Link)
    // resolves against the same list without running its own scan
    {
        auto root = std::make_unique<juce::XmlElement>("CHAIN_ENTRIES");
        for (auto& d : collected)
            if (auto x = d.createXml())
                root->addChildElement(x.release());
        appSupportDir().createDirectory();
        auto ecFile = getEntriesCacheFile();
        root->writeTo(ecFile);
        entriesCacheTime_ = ecFile.getLastModificationTime();
    }

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
int ChainHost::getNumPlugins() const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    return entries_.size();
}

juce::Array<juce::PluginDescription> ChainHost::getFilteredPlugins(
    const juce::String& filter,
    const juce::String& formatFilter) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    juce::Array<juce::PluginDescription> result;
    juce::String lf = filter.toLowerCase();
    for (auto& d : entries_)
    {
        if (formatFilter.isNotEmpty() && d.pluginFormatName != formatFilter) continue;
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
    std::vector<SlotInfo> result;
    result.reserve(slots_.size());
    for (auto& s : slots_)
        result.push_back({ s.desc.name, s.bypassed, s.settings,
                           s.desc.pluginFormatName, s.wet });
    return result;
}

ChainHost::SlotInfo ChainHost::getSlotInfo(int i) const
{
    if (i < 0 || i >= (int)slots_.size()) return { {}, false, {}, {}, 1.0f };
    return { slots_[i].desc.name, slots_[i].bypassed, slots_[i].settings,
             slots_[i].desc.pluginFormatName, slots_[i].wet };
}

void ChainHost::setSlotSettings(int i, const juce::String& settings)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    slots_[i].settings = settings;
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
        // Machine-readable dial values on add/replace (28 Jul 2026, surgical
        // EQ): the server sends the same settings_structured object a chain
        // entry carries. Read as a raw var; setSlotStructuredSettings ignores
        // a void/non-object, so ops without it behave exactly as before.
        op.settingsStructured = eo->getProperty("settings_structured");
        if (op.op.isNotEmpty()) out.push_back(std::move(op));
    }
    return out;
}

juce::String ChainHost::describeEditOp(const ChainEditOp& op,
                                       const juce::StringArray& baseSlots)
{
    // Display is 1-BASED (matches the [CURRENT CHAIN] injection and the
    // model's numbering); op fields are internal 0-based post-parse.
    auto slotName = [&baseSlots](int i) -> juce::String {
        return (i >= 0 && i < baseSlots.size())
            ? baseSlots[i] : ("slot " + juce::String(i + 1));
    };
    if (op.op == "add")
        return juce::String::fromUTF8("+ add ") + op.name
             + (op.after <= -1 ? juce::String(" first")
                : op.after >= baseSlots.size()
                    ? juce::String(" at the end of the chain")
                    : " after slot " + juce::String(op.after + 1)
                      + " (" + slotName(op.after) + ")");
    if (op.op == "remove")
        return juce::String::fromUTF8("\xe2\x88\x92 remove ") + slotName(op.slot)
             + " (slot " + juce::String(op.slot + 1) + ")";
    if (op.op == "replace")
        return juce::String::fromUTF8("\xe2\x87\x84 replace ") + slotName(op.slot)
             + " (slot " + juce::String(op.slot + 1) + ") with " + op.name;
    if (op.op == "move")
        return juce::String::fromUTF8("\xe2\x86\x95 move ") + slotName(op.slot)
             + " (slot " + juce::String(op.slot + 1) + ") to position " + juce::String(op.to + 1);
    if (op.op == "bypass")
        return juce::String(op.on ? "\xe2\x8f\xbb bypass " : "\xe2\x8f\xbb un-bypass ")
             + slotName(op.slot) + " (slot " + juce::String(op.slot + 1) + ")";
    if (op.op == "set")
        return juce::String::fromUTF8("\xe2\x9a\x99 dial ") + slotName(op.slot)
             + " (slot " + juce::String(op.slot + 1) + ")"
             + (op.settings.isNotEmpty() ? ": " + op.settings : juce::String());
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
    if (expectedRevision >= 0 && expectedRevision != getChainRevision())
        return abort("chain changed since this edit was proposed — ask again");

    // ---- Pre-flight guard 2: baseSlots vs live rack ----
    const int n = getNumSlots();
    if (baseSlots.size() != n)
        return abort("chain changed since this edit was proposed — ask again");
    for (int i = 0; i < n; ++i)
        if (!namesMatchLoose(baseSlots[i], slots_[(size_t)i].desc.name))
            return abort("chain changed since this edit was proposed — ask again");

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
            auto validSlot = [&](int s) {
                return s >= 0 && s < n && alive[(size_t)s];
            };
            if (op.op == "add")
            {
                if (op.name.isEmpty()) return bad("add without a plugin name");
                if (resolveByName(op.name, {}).name.isEmpty())
                    return bad("\"" + op.name + "\" not in the loadable plugin list");
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
                if (resolveByName(op.name, {}).name.isEmpty())
                    return bad("\"" + op.name + "\" not in the loadable plugin list");
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
                if (op.settings.isEmpty() && op.settingsStructured.getDynamicObject() == nullptr)
                    return bad("set without any settings");
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

    if (op.op == "remove")
    {
        const int cur = curOf(op.slot);
        if (cur < 0) return failAndStop("remove failed: slot no longer present");
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
        setSlotBypassed(cur, op.on);
        finishOpAndContinue(juce::String(op.on ? "bypassed " : "un-bypassed ")
                            + slots_[(size_t)cur].desc.name);
        return;
    }
    if (op.op == "move")
    {
        const int cur = curOf(op.slot);
        if (cur < 0) return failAndStop("move failed: slot no longer present");
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
        const juce::String nm = slots_[(size_t)cur].desc.name;
        if (op.settings.isNotEmpty())
            setSlotSettings(cur, op.settings);
        if (op.settingsStructured.getDynamicObject() != nullptr)
            setSlotStructuredSettings(cur, op.settingsStructured);
        finishOpAndContinue("dialled " + nm);
        return;
    }
    if (op.op == "add" || op.op == "replace")
    {
        // Fallible work FIRST: load (appends at the end). Only on success do
        // we touch existing slots, so a failed load is a clean no-op and the
        // sequence CONTINUES with the remaining independent ops.
        auto desc = resolveByName(op.name, {});
        if (desc.name.isEmpty())
            return failButContinue(op.op + " failed: \"" + op.name + "\" not resolvable");
        desc = preferInlineHostableDesc(desc);
        auto self = st;
        const auto theOp = op;
        loadPluginAsync(desc, [this, self, theOp, desc](const juce::String& err)
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
            auto applyOpSettings = [this, &theOp](int slotIdx)
            {
                if (theOp.settings.isNotEmpty())
                    setSlotSettings(slotIdx, theOp.settings);
                if (theOp.settingsStructured.getDynamicObject() != nullptr)
                    setSlotStructuredSettings(slotIdx, theOp.settingsStructured);
            };

            if (theOp.op == "add")
            {
                applyOpSettings(newCur);
                int target = theOp.after <= -1 ? 0
                    : (theOp.after < (int)self->map.size() && self->map[(size_t)theOp.after] >= 0
                        ? self->map[(size_t)theOp.after] + 1
                        : newCur);   // fallback: leave at end
                walkSlotTo(self->map, newCur, target);
                // Entries at/after the insert point were fixed by walkSlotTo's
                // swap bookkeeping; nothing else to update (new slot is not
                // addressable by original numbering).
                finish("added " + theOp.name
                       + (theOp.after <= -1 ? juce::String(" first")
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
                for (auto& m : self->map) { if (m == oldCur) m = -1; else if (m > oldCur) --m; }
                int fromCur = getNumSlots() - 1;   // new slot after the removal shift
                walkSlotTo(self->map, fromCur, oldCur);
                // The replacement now answers to the original slot number
                self->map[(size_t)theOp.slot] = oldCur;

                if (samePlugin && oldState.getSize() > 0)
                    if (auto* newNode = slots_[(size_t)oldCur].node.get())
                        if (auto* newProc = newNode->getProcessor())
                            try { newProc->setStateInformation(oldState.getData(),
                                                               (int)oldState.getSize()); }
                            catch (...) {}

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

void ChainHost::setSlotWet(int i, float wet01)
{
    if (i < 0 || i >= (int)slots_.size()) return;
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

    auto results = echojay::applySettings (*instance, map, structuredSettings);
    for (auto& r : results)
        out.push_back ({ r.semantic, r.applied, r.normalized, r.note,
                         r.landedText, r.displayVerified, r.readbackMismatch,
                         r.requestedValue });

    return out;
}

void ChainHost::removeSlot(int i)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    // Before the instance goes to the graveyard, where it stays ALIVE for
    // the session: a parked plugin with a leaked UI timer must not be able
    // to call a listener on a ChainHost that has since gone away.
    detachStateListener(i);
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
        return proc ? proc->createEditor() : nullptr;
    }
    catch (...) { return nullptr; }
}

// ---------------------------------------------------------------------------
// Async load (appends to chain)
// ---------------------------------------------------------------------------
void ChainHost::completeLoad(std::unique_ptr<juce::AudioPluginInstance> inst,
                              const juce::PluginDescription& desc)
{
    // Any successful load clears a stale session load-failure mark
    sessionLoadFailed_.removeString(sessionLoadKey(desc.name, desc.pluginFormatName));

    ChainSlot slot;
    slot.node     = graph_->addNode(std::move(inst));
    slot.desc     = desc;
    slot.bypassed = false;
    slots_.push_back(std::move(slot));
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
        const auto ik = echojay::identityKeyForDescription(liveDesc);
        auto it = identityToFp_.find(ik);
        if (it == identityToFp_.end() || it->second != slots_[(size_t)newSlotIdx].fp)
        {
            identityToFp_[ik] = slots_[(size_t)newSlotIdx].fp;
            saveParamMapsToDisk();
        }
        applyStructuredIfReady(newSlotIdx);
    }

    // Hosted settings cache. No-op unless enabled (it is not in EchoJay
    // Link, which captures live in chainModelToVar). The first capture is
    // taken NOW rather than waiting for the debounce: a plugin added a
    // second before the user hits Cmd-S would otherwise save as null, which
    // reads as "your settings were dropped" for a slot nothing was wrong
    // with. Every later capture for this slot goes through the timer.
    if (stateCacheEnabled_)
    {
        attachStateListener(newSlotIdx);
        captureSlotState(newSlotIdx, juce::Time::getMillisecondCounterHiRes());
    }
}

// ---------------------------------------------------------------------------
// Auto-parameter-mapping pipeline (the ONE apply path)
// ---------------------------------------------------------------------------
void ChainHost::setSlotStructuredSettings(int i, const juce::var& structured)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    if (structured.isVoid() || structured.getDynamicObject() == nullptr) return;

    slots_[(size_t)i].structuredSettings = structured;
    slots_[(size_t)i].structuredApplied  = false;
    applyStructuredIfReady(i);

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
    // The applyStructuredIfReady above ran BEFORE the fetch kicked, so a
    // first-encounter slot got noMap; correct it to pending while the
    // answer is in flight.
    if (!slots_[(size_t)i].structuredApplied && pendingMapFps_.contains(fp))
        slots_[(size_t)i].dialStatus = DialStatus::pending;
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
        applyStructuredIfReady(i);
        if (!wasApplied && slots_[(size_t)i].structuredApplied) changed = true;
    }
    if (changed && onSlotSettingsChanged) onSlotSettingsChanged();
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
            applyStructuredIfReady(i);
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

void ChainHost::applyStructuredIfReady(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= (int)slots_.size()) return;
    auto& s = slots_[(size_t)slotIndex];
    if (s.structuredApplied) return;
    if (s.structuredSettings.isVoid() || s.structuredSettings.getDynamicObject() == nullptr) return;
    if (s.fp.isEmpty()) { s.dialStatus = DialStatus::pending; return; } // fp arrives at load
    auto it = paramMaps_.find(s.fp);
    if (it == paramMaps_.end())
    {
        // Apply-time honesty: never a silent skip any more. Outcome is
        // "pending" while a fetch is in flight, "noMap" once the fetch has
        // answered (or was never possible). The result bubble reads this.
        s.dialStatus = pendingMapFps_.contains(s.fp) ? DialStatus::pending
                                                     : DialStatus::noMap;
        return;
    }

    // INTEGRITY: the map's own fp field must equal the slot's live
    // fingerprint, not just the cache key it was stored under. Catches any
    // keying bug (server response, cache merge, disk corruption) before a
    // wrong-layout map can touch a single parameter.
    const auto mapFp = it->second.getProperty("fp", juce::var()).toString();
    if (mapFp != s.fp)
    {
        EchoJay_NSLog(("EJParamApply: map fp mismatch for slot " + juce::String(slotIndex)
                       + " (\"" + s.desc.name + "\"): key " + s.fp.substring(0, 12)
                       + " vs map.fp " + (mapFp.isEmpty() ? juce::String("(missing)")
                                                          : mapFp.substring(0, 12))
                       + ", apply refused").toRawUTF8());
        s.dialStatus = DialStatus::unusableMap;
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
            // Read-back caveat travels to the card: a setread/unparseable
            // write proves addressability, not correctness, and must never
            // present the same as a display-verified write.
            if (!r.displayVerified) line += " (unverified)";
            appliedSummary.add(line);
        }
        else
        {
            s.dialManual.addIfNotAlreadyThere(echojay::semanticLabel(r.semantic));
            if (r.readbackMismatch)
                s.dialReadbackMiss.addIfNotAlreadyThere(echojay::semanticLabel(r.semantic));
        }
    }

    // Honest per-slot verdict: applied only when EVERY requested semantic
    // was written; anything less is partial (some written) or unusableMap
    // (map exists, nothing written). ">=1 written" reported as success
    // would still overclaim (the spiff class of bug).
    s.dialAppliedCount = (int) appliedSummary.size();
    if (report.empty())
        s.dialStatus = DialStatus::unusableMap;   // structured present, nothing requested survived
    else if (s.dialManual.isEmpty())
        s.dialStatus = DialStatus::applied;
    else if (s.dialAppliedCount > 0)
        s.dialStatus = DialStatus::partial;
    else
        s.dialStatus = DialStatus::unusableMap;

    // SUGGESTED SETTINGS display contract: auto-applied slots show
    // "Applied automatically" + a compact summary of what was set;
    // slots with nothing applied keep the prose guidance unchanged.
    if (!appliedSummary.isEmpty())
        s.settings = "Applied automatically\n" + appliedSummary.joinIntoString(", ");
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
{
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
    juce::File deadman,
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
    juce::File deadman,
    std::function<void(const juce::String&)> cb,
    int ticksLeft)
{
    if (vs->done.load())
    {
        deadman.deleteFile();
        juce::PluginDescription fullDesc;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(vs->mtx);
            if (!vs->results.isEmpty()) { fullDesc = vs->results[0]; found = true; }
        }
        if (!found) { cb("No types found in " + desc.name); return; }

        host->saveToDisk();
        host->asyncCreatePlugin(fullDesc,
            [host, cb, fullDesc](std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
            {
                if (!inst) { cb(err.isNotEmpty() ? err : "createPluginInstance failed"); return; }
                host->completeLoad(std::move(inst), fullDesc);
                cb({});
            });
        return;
    }

    if (ticksLeft <= 0)
    {
        host->addToBlacklist(desc.fileOrIdentifier);
        cb("Timed out loading \"" + desc.name + "\": added to skip list");
        return;
    }

    juce::Timer::callAfterDelay(100, [host, vs, desc, deadman, cb, ticksLeft]() mutable {
        pollVST3Validation(host, vs, desc, deadman, cb, ticksLeft - 1);
    });
}

void ChainHost::loadPluginAsync(const juce::PluginDescription& desc,
                                std::function<void(const juce::String& error)> callback)
{
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
        formatManager_.createPluginInstanceAsync(
            fullDesc, sampleRate_, blockSize_,
            [this, callback, fullDesc](std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
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
                completeLoad(std::move(inst), fullDesc);
                callback({});
            });
        return;
    }

    // Thin VST3: validate in detached thread, poll on message thread
    appSupportDir().createDirectory();
    auto deadman = getDeadmanFile();
    deadman.replaceWithText(desc.fileOrIdentifier);

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

    pollVST3Validation(this, vs, desc, deadman, callback, 100);
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
                                                 juce::String* matchLogOut) const
{
    auto raw  = rawName.trim();
    auto base = stripParenthetical(raw);
    // Manufacturer from the parenthetical (if any) — disambiguation only
    juce::String manu;
    if (raw.endsWithChar(')') && raw.contains(" ("))
        manu = raw.fromLastOccurrenceOf(" (", false, false)
                  .dropLastCharacters(1).trim();

    juce::Array<juce::PluginDescription> cands;
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        for (auto& d : entries_)
        {
            if (formatFilter.isNotEmpty() && d.pluginFormatName != formatFilter)
                continue;
            cands.add(d);
        }
    }

    auto logMatch = [&](const char* how, const juce::PluginDescription& d)
    {
        if (matchLogOut)
            *matchLogOut = juce::String(how) + " -> \"" + d.name + "\" ["
                         + d.pluginFormatName + "]";
    };

    for (auto& d : cands)
        if (d.name.equalsIgnoreCase(raw)) { logMatch("exact", d); return d; }

    // Parenthetical-stripped match, manufacturer as tie-breaker
    juce::Array<juce::PluginDescription> baseHits;
    for (auto& d : cands)
        if (d.name.equalsIgnoreCase(base)) baseHits.add(d);
    if (baseHits.size() == 1) { logMatch("stripped", baseHits[0]); return baseHits[0]; }
    if (baseHits.size() > 1)
    {
        if (manu.isNotEmpty())
            for (auto& d : baseHits)
                if (d.manufacturerName.containsIgnoreCase(manu))
                { logMatch("stripped+manufacturer", d); return d; }
        logMatch("stripped (first of several)", baseHits[0]);
        return baseHits[0];
    }

    // Normalised (case/punctuation/version-token tolerant). Model-number guard:
    // normalizeName strips a trailing number as a version, which collapses
    // "AMEK EQ 250" and "AMEK EQ 200" to the same stem. When the request AND
    // the candidate each carry a trailing number and they DIFFER, the number is
    // a model, not a version, so it is not a match. A number on only one side
    // (e.g. "Saturn 2" vs a plugin named "Saturn") still tolerates the strip.
    auto keyIn = normalizeName(base);
    auto numIn = trailingModelNumber(base);
    for (auto& d : cands)
        if (normalizeName(stripParenthetical(d.name)) == keyIn)
        {
            auto numCand = trailingModelNumber(stripParenthetical(d.name));
            if (numIn.isNotEmpty() && numCand.isNotEmpty() && numIn != numCand)
                continue;
            logMatch("normalised", d); return d;
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

void ChainHost::addToBlacklist(const juce::String& path)
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    if (!blacklist_.contains(path)) blacklist_.add(path);
}

// ---------------------------------------------------------------------------
// Settings ↔ ChainHost resolver
// ---------------------------------------------------------------------------

// Normalize a plugin name for fuzzy matching:
//   lowercase, trim whitespace, collapse internal runs of spaces/punctuation
//   to a single space, strip trailing version suffixes like " 3" / " v2" / " 2.0".
static juce::String normalizeName(const juce::String& raw)
{
    juce::String s = raw.toLowerCase().trim();
    // Replace common punctuation chars that differ between sources with space
    s = s.replace("-", " ").replace("_", " ").replace(".", " ");
    // Collapse multiple spaces
    while (s.contains("  ")) s = s.replace("  ", " ");
    // Strip trailing version tokens: " 3", " v2", " 2", " ii", " iii"
    s = s.trimEnd();
    juce::StringArray parts = juce::StringArray::fromTokens(s, " ", "");
    if (parts.size() >= 2)
    {
        const auto& last = parts[parts.size() - 1];
        bool isVersion = last.containsOnly("0123456789") ||
                         (last.startsWithChar('v') && last.substring(1).containsOnly("0123456789")) ||
                         last == "ii" || last == "iii" || last == "iv";
        if (isVersion) parts.remove(parts.size() - 1);
    }
    return parts.joinIntoString(" ").trim();
}

// The trailing all-digits token normalizeName would strip, or empty. Mirrors
// that tokenization so the number it returns is exactly the one stripped. Only
// bare digits count as a model (v2 / II / III stay version suffixes); those are
// still stripped and never guarded.
static juce::String trailingModelNumber(const juce::String& raw)
{
    juce::String s = raw.toLowerCase().trim();
    s = s.replace("-", " ").replace("_", " ").replace(".", " ");
    while (s.contains("  ")) s = s.replace("  ", " ");
    juce::StringArray parts = juce::StringArray::fromTokens(s.trim(), " ", "");
    if (parts.size() >= 2)
    {
        const auto& last = parts[parts.size() - 1];
        if (last.containsOnly("0123456789")) return last;
    }
    return {};
}

void ChainHost::buildRecommendable(const std::vector<ScannedPlugin>& allPlugins,
                                    const juce::String& formatFilter)
{
    // Snapshot the loadable entries under lock
    juce::Array<juce::PluginDescription> loadable;
    bool entriesEmpty = false;
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
            loadable.add(d);
        }
    }

    // Build a normalized-name → PluginDescription map from the loadable entries.
    // If multiple entries share the same normalized name, keep the first (alphabetically
    // stable since entries_ is already sorted).
    std::unordered_map<std::string, juce::PluginDescription> nameMap;
    nameMap.reserve((size_t)loadable.size() * 2);
    for (const auto& d : loadable)
    {
        std::string key = normalizeName(d.name).toStdString();
        if (nameMap.find(key) == nameMap.end())
            nameMap[key] = d;
        // Variant-suffix secondary key: WaveShell AUs register per-variant
        // component names ("Abbey Road Plates (s)"/"(m)") while the Settings
        // scanner lists the curated suffix-less name ("Abbey Road Plates").
        // Without this key the exact map missed them, the plugin dropped out
        // of the AI feed entirely, and the model told the user a plugin
        // RUNNING IN THEIR RACK was "not in your available plugins".
        std::string baseKey = normalizeName(stripParenthetical(d.name)).toStdString();
        if (baseKey != key && nameMap.find(baseKey) == nameMap.end())
            nameMap[baseKey] = d;
    }

    // Filter enabled scanner plugins and resolve against the map
    int enabledCount = 0;
    std::vector<RecommendableEntry> resolved;

    for (const auto& sp : allPlugins)
    {
        if (!sp.enabled) continue;
        ++enabledCount;

        // Try exact normalized name
        std::string key = normalizeName(sp.name).toStdString();
        auto it = nameMap.find(key);

        // If not found, try without manufacturer prefix ("Fab Filter: Pro-Q 3" → "pro q 3")
        if (it == nameMap.end() && sp.name.containsChar(':'))
        {
            juce::String afterColon = sp.name.fromFirstOccurrenceOf(":", false, false).trim();
            key = normalizeName(afterColon).toStdString();
            it = nameMap.find(key);
        }

        if (it != nameMap.end())
            resolved.push_back({ sp.name, it->second });
    }

    // Log unmatched count to stderr so it's visible in DAW console
    int unmatched = enabledCount - (int)resolved.size();
    if (unmatched > 0)
        DBG("ChainHost resolver: " + juce::String(resolved.size()) + "/" + juce::String(enabledCount)
            + " enabled plugins resolved (" + juce::String(unmatched) + " unmatched)");

    // Cache result (message thread only — no mutex)
    recommendable_          = std::move(resolved);
    recommendableEnabledIn_ = enabledCount;
    recommendableFormat_    = formatFilter;

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

std::vector<ChainHost::SlotDialInfo> ChainHost::getDialInfos() const
{
    std::vector<SlotDialInfo> out;
    out.reserve(slots_.size());
    for (const auto& s : slots_)
    {
        SlotDialInfo di;
        di.name         = s.desc.name;
        di.fp           = s.fp;
        di.status       = s.dialStatus;
        di.manual       = s.dialManual;
        di.readbackMiss = s.dialReadbackMiss;
        di.appliedCount = s.dialAppliedCount;
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

juce::StringArray ChainHost::getDialableRecommendableNames() const
{
    juce::StringArray out;
    for (const auto& e : recommendable_)
    {
        auto it = identityToFp_.find(echojay::identityKeyForDescription(e.desc));
        if (it == identityToFp_.end()) continue;
        auto m = paramMaps_.find(it->second);
        if (m != paramMaps_.end() && echojay::mapIsDialableForSignals(m->second))
            out.addIfNotAlreadyThere(e.displayName);
    }
    return out;
}

void ChainHost::loadByRecommendedName(const juce::String& name,
                                       std::function<void(const juce::String&)> callback)
{
    juce::String nameLower = name.toLowerCase().trim();
    for (const auto& e : recommendable_)
    {
        if (e.displayName.toLowerCase().trim() == nameLower)
        {
            // NEW instantiation — popout-only AUs may swap to their VST3 build
            loadPluginAsync(preferInlineHostableDesc(e.desc), std::move(callback));
            return;
        }
    }

    // Loose fallback via the shared resolver — handles "Name (Manufacturer)"
    // strings and punctuation/version drift the exact match above misses.
    // Same resolution both hosts use, honouring the active format filter.
    {
        juce::String matchLog;
        auto d = resolveByName(name, recommendableFormat_, &matchLog);
        // Log which stage resolved (or failed) so a future mis-resolution -
        // like "AMEK EQ 250" landing on "AMEK EQ 200" - is diagnosable from
        // the unified log instead of invisible.
        EchoJay_NSLog(("EJChain: resolve \"" + name + "\" -> " + matchLog).toRawUTF8());
        if (d.name.isNotEmpty())
        {
            loadPluginAsync(preferInlineHostableDesc(d), std::move(callback));
            return;
        }
    }

    callback("\"" + name + "\" not found in recommendable list");
}

// ---------------------------------------------------------------------------
// Persistence — plugin list cache
// ---------------------------------------------------------------------------
void ChainHost::saveToDisk() const
{
    appSupportDir().createDirectory();
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    auto xml = knownPlugins_.createXml();
    if (xml) xml->writeTo(getPluginListFile());
    juce::String bl;
    for (auto& p : blacklist_) bl += p + "\n";
    getBlacklistFile().replaceWithText(bl);
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
            }
            entriesCacheTime_ = ecFile.getLastModificationTime();
        }
    }
    auto blFile = getBlacklistFile();
    if (blFile.existsAsFile())
    {
        auto lines = juce::StringArray::fromLines(blFile.loadFileAsString());
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        for (auto& line : lines)
            if (line.trim().isNotEmpty())
                blacklist_.addIfNotAlreadyThere(line.trim());
    }
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
    loadPluginAsync(items[idx].desc,
        [this, items = std::move(items), idx, wasBypassed, savedWet,
         stateB64, expectState, slotName, onSlotSettled](const juce::String& err) mutable
        {
            if (err.isEmpty())
            {
                int lastSlot = (int)slots_.size() - 1;
                if (lastSlot >= 0)
                {
                    setSlotWet(lastSlot, savedWet);
                    if (wasBypassed) setSlotBypassed(lastSlot, true);
                    applyRestoredState(lastSlot, stateB64, expectState, slotName);
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

void ChainHost::applyRestoredState(int slotIdx, const juce::String& b64,
                                   bool expectState, const juce::String& slotName)
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

    try
    {
        proc->setStateInformation(mo.getData(), (int)mo.getDataSize());
    }
    catch (...)
    {
        addStateNote(slotName + ": rejected its saved settings,"
                                " so it loaded at its defaults");
        return;
    }

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

void ChainHost::tryRestoreSlotsFromXml(const juce::String& xml,
                                       const juce::var& slotStates)
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
        attachStateListener(i);
    if (stateCacheTimer_ == nullptr)
        stateCacheTimer_ = std::make_unique<StateCacheTimer>(*this);
    stateCacheTimer_->startTimer(kStateTickMs);
    noteHostedChange();   // first capture on the next settled tick
}

void ChainHost::noteHostedChange() noexcept
{
    // Reachable from the audio thread during automation: two relaxed atomic
    // stores, nothing else. No container access, no allocation, no lock.
    lastChangeMs_.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    stateDirty_.store(true, std::memory_order_relaxed);
}

void ChainHost::attachStateListener(int i)
{
    if (!stateCacheEnabled_) return;
    if (i < 0 || i >= (int)slots_.size() || !slots_[(size_t)i].node) return;
    if (auto* p = slots_[(size_t)i].node->getProcessor())
    {
        p->removeListener(this);   // never double-register
        p->addListener(this);
    }
}

void ChainHost::detachStateListener(int i)
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
    const bool oversize = bytes > kApiStateMaxSlotBytes;
    juce::String b64;
    if (bytes > 0 && !oversize)
        b64 = juce::Base64::toBase64(mb.getData(), mb.getSize());

    juce::String note;
    {
        std::lock_guard<std::mutex> lock(stateCacheMutex_);
        auto& s = slots_[(size_t)i];
        s.lastKnownState = b64;
        s.lastKnownBytes = oversize ? 0 : bytes;
        s.lastCaptureMs  = cost;
        s.capturedAtMs   = nowMs;

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
                       (juce::int64)kApiStateMaxSlotBytes)
                 + " limit, so they will not be saved";
        }
        else if (!oversize && s.oversizeReported)
        {
            s.oversizeReported = false;   // shrank back under the cap
        }
    }
    if (note.isNotEmpty()) addStateNote(note);
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

void ChainHost::restoreSavedChain(const juce::var& slotsArr, const juce::var& stateObj,
                                  std::function<void()> onSlotSettled)
{
    auto* arr = slotsArr.getArray();
    if (arr == nullptr || arr->isEmpty()) return;

    // One load, one clean slate: notes are about the chain now on screen.
    clearStateNotes();
    auto* statesObj = stateObj.getDynamicObject();

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

        auto desc = resolveByName(name, {}, nullptr);
        if (desc.name.isEmpty())
        {
            // NEVER "not owned": a plugin the user owns looks exactly like
            // this on a machine where it is not installed, or where it
            // cannot authorise right now.
            addStateNote(name + ": could not be found on this machine,"
                                " so this slot was skipped");
            continue;
        }

        RestoreItem item;
        item.desc     = desc;
        item.bypassed = (bool)o->getProperty("bypassed");
        // The shared chain format carries no wet/dry, so a saved chain
        // restores fully wet. Session restore keeps wet because its own XML
        // has it. Stated here so the difference is a known gap and not a
        // mystery the next reader has to rediscover.
        item.wet         = 1.0f;
        item.expectState = (statesObj != nullptr);
        if (statesObj != nullptr)
        {
            const juce::String key (n);
            if (statesObj->hasProperty(key))
                item.stateBase64 = statesObj->getProperty(key).toString();
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
