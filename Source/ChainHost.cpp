#include "ChainHost.h"
#include "AUEnumerator.h"
#include "NativeClip.h"   // EchoJay_NSLog
#include <algorithm>
#include <unordered_map>

// Defined later in this file (used by the shared name resolution above it)
static juce::String normalizeName(const juce::String& raw);

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
juce::File ChainHost::getBlacklistFile()  { return appSupportDir().getChildFile("chain_blacklist.txt"); }
juce::File ChainHost::getDeadmanFile()    { return appSupportDir().getChildFile("chain_load_deadman.txt"); }

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
// Session project name — shared file, mtime-cached (popout_only pattern)
// ---------------------------------------------------------------------------
static juce::File sessionProjectFile() { return appSupportDir().getChildFile("session_project.json"); }
static juce::String& sessionProjectCache() { static juce::String s; return s; }
static juce::Time& sessionProjectLoadTime() { static juce::Time t; return t; }

juce::String ChainHost::getSessionProjectName()
{
    auto f  = sessionProjectFile();
    auto mt = f.getLastModificationTime();
    if (mt != sessionProjectLoadTime())
    {
        sessionProjectLoadTime() = mt;
        sessionProjectCache().clear();
        if (f.existsAsFile())
        {
            auto v = juce::JSON::parse(f.loadFileAsString());
            if (auto* o = v.getDynamicObject())
                sessionProjectCache() = o->getProperty("name").toString().trim();
        }
    }
    return sessionProjectCache();
}

void ChainHost::publishSessionProjectName(const juce::String& name)
{
    auto trimmed = name.trim();
    if (getSessionProjectName() == trimmed) return;   // no-op republish
    auto* o = new juce::DynamicObject();
    o->setProperty("name",      trimmed);
    o->setProperty("updatedAt", juce::Time::getCurrentTime().toISO8601(true));
    auto f = sessionProjectFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::JSON::toString(juce::var(o), true));
    sessionProjectCache()    = trimmed;
    sessionProjectLoadTime() = f.getLastModificationTime();
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
        sessionGenreCache().clear();
        if (f.existsAsFile())
        {
            auto v = juce::JSON::parse(f.loadFileAsString());
            if (auto* o = v.getDynamicObject())
                sessionGenreCache() = o->getProperty("genre").toString().trim();
        }
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
    auto f = sessionGenreFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::JSON::toString(juce::var(o), true));
    sessionGenreCache()    = trimmed;
    sessionGenreLoadTime() = f.getLastModificationTime();
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
ChainHost::ChainHost()
{
    juce::addDefaultFormatsToManager(formatManager_);

    graph_ = std::make_unique<juce::AudioProcessorGraph>();

    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode_  = graph_->addNode(std::make_unique<IOProc>(IOProc::audioInputNode));
    outputNode_ = graph_->addNode(std::make_unique<IOProc>(IOProc::audioOutputNode));

    rebuildGraph(); // passthrough (no slots yet)
    loadFromDisk();

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
    if (!prepared_ || !graph_ || !hasActiveSlots_.load())
        return;   // buffer passes through untouched

    graph_->processBlock(buffer, midi);
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
        result.push_back({ s.desc.name, s.bypassed, s.settings, s.desc.pluginFormatName });
    return result;
}

ChainHost::SlotInfo ChainHost::getSlotInfo(int i) const
{
    if (i < 0 || i >= (int)slots_.size()) return { {}, false, {}, {} };
    return { slots_[i].desc.name, slots_[i].bypassed, slots_[i].settings,
             slots_[i].desc.pluginFormatName };
}

void ChainHost::setSlotSettings(int i, const juce::String& settings)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    slots_[i].settings = settings;
}

void ChainHost::removeSlot(int i)
{
    if (i < 0 || i >= (int)slots_.size()) return;
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
    slots_.erase(slots_.begin() + i);
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
    ChainSlot slot;
    slot.node     = graph_->addNode(std::move(inst));
    slot.desc     = desc;
    slot.bypassed = false;
    slots_.push_back(std::move(slot));
    rebuildGraph();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
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
        cb("Timed out loading \"" + desc.name + "\" — added to skip list");
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

    // Collect active (non-bypassed) node IDs in chain order
    std::vector<juce::AudioProcessorGraph::NodeID> active;
    for (auto& s : slots_)
        if (!s.bypassed && s.node)
            active.push_back(s.node->nodeID);

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

    // Input → first active slot
    {
        int nIn = channelsOf(active[0], true);
        for (int ch = 0; ch < juce::jmin(2, nIn); ++ch)
            graph_->addConnection({{inputNode_->nodeID, ch}, {active[0], ch}});
    }

    // Each slot → next slot
    for (int i = 0; i + 1 < (int)active.size(); ++i)
    {
        int nOut = channelsOf(active[i],     false);
        int nIn2 = channelsOf(active[i + 1], true);
        for (int ch = 0; ch < std::min({2, nOut, nIn2}); ++ch)
            graph_->addConnection({{active[i], ch}, {active[i + 1], ch}});
    }

    // Last active slot → output
    {
        int nOut = channelsOf(active.back(), false);
        for (int ch = 0; ch < juce::jmin(2, nOut); ++ch)
            graph_->addConnection({{active.back(), ch}, {outputNode_->nodeID, ch}});
        // Passthrough for any uncovered output channels
        for (int ch = nOut; ch < 2; ++ch)
            graph_->addConnection({{inputNode_->nodeID, ch}, {outputNode_->nodeID, ch}});
    }

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
    return normalizeName(inBase) == normalizeName(stripParenthetical(en));
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

    // Normalised (case/punctuation/version-token tolerant)
    auto keyIn = normalizeName(base);
    for (auto& d : cands)
        if (normalizeName(stripParenthetical(d.name)) == keyIn)
        { logMatch("normalised", d); return d; }

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

void ChainHost::buildRecommendable(const std::vector<ScannedPlugin>& allPlugins,
                                    const juce::String& formatFilter)
{
    // Snapshot the loadable entries under lock
    juce::Array<juce::PluginDescription> loadable;
    {
        std::lock_guard<std::mutex> lk(pluginsMutex_);
        for (const auto& d : entries_)
        {
            if (formatFilter.isNotEmpty() && d.pluginFormatName != formatFilter) continue;
            loadable.add(d);
        }
    }

    // Build a normalized-name → PluginDescription map from the loadable entries.
    // If multiple entries share the same normalized name, keep the first (alphabetically
    // stable since entries_ is already sorted).
    std::unordered_map<std::string, juce::PluginDescription> nameMap;
    nameMap.reserve((size_t)loadable.size());
    for (const auto& d : loadable)
    {
        std::string key = normalizeName(d.name).toStdString();
        if (nameMap.find(key) == nameMap.end())
            nameMap[key] = d;
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
}

juce::StringArray ChainHost::getRecommendableNames() const
{
    juce::StringArray names;
    for (const auto& e : recommendable_)
        names.add(e.displayName);
    return names;
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
    for (auto& s : slots_)
    {
        auto* item = root->createNewChildElement("SLOT");
        item->setAttribute("bypassed", s.bypassed ? 1 : 0);
        if (auto descXml = s.desc.createXml())
            item->addChildElement(descXml.release());
    }
    return root->toString();
}

void ChainHost::restoreNextSlot(std::vector<RestoreItem> items, int idx)
{
    if (idx >= (int)items.size()) return;
    bool wasBypassed = items[idx].bypassed;
    loadPluginAsync(items[idx].desc,
        [this, items = std::move(items), idx, wasBypassed](const juce::String& err) mutable
        {
            if (err.isEmpty() && wasBypassed)
            {
                int lastSlot = (int)slots_.size() - 1;
                if (lastSlot >= 0) setSlotBypassed(lastSlot, true);
            }
            restoreNextSlot(std::move(items), idx + 1);
        });
}

void ChainHost::tryRestoreSlotsFromXml(const juce::String& xml)
{
    if (xml.isEmpty()) return;
    auto root = juce::XmlDocument::parse(xml);
    if (!root || root->getTagName() != "CHAIN_SLOTS") return;

    std::vector<RestoreItem> items;
    for (auto* child : root->getChildIterator())
    {
        if (child->getTagName() != "SLOT") continue;
        bool bypassed = child->getIntAttribute("bypassed", 0) != 0;
        auto* descElem = child->getFirstChildElement();
        if (!descElem) continue;
        juce::PluginDescription desc;
        if (!desc.loadFromXml(*descElem)) continue;
        items.push_back({ desc, bypassed });
    }

    if (!items.empty())
        restoreNextSlot(std::move(items), 0);
}
