#include "ChainHost.h"
#include "AUEnumerator.h"
#include <algorithm>
#include <unordered_map>

// ---------------------------------------------------------------------------
// File path helpers
// ---------------------------------------------------------------------------
static juce::File appSupportDir()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
           .getChildFile("EchoJay");
}

juce::File ChainHost::getPluginListFile() { return appSupportDir().getChildFile("chain_plugins.xml"); }
juce::File ChainHost::getBlacklistFile()  { return appSupportDir().getChildFile("chain_blacklist.txt"); }
juce::File ChainHost::getDeadmanFile()    { return appSupportDir().getChildFile("chain_load_deadman.txt"); }

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

ChainHost::~ChainHost()
{
    cancelFlag_.store(true);
    if (scanThread_.joinable()) scanThread_.join();

    // Remove all connections and nodes cleanly
    if (graph_)
    {
        for (auto& c : graph_->getConnections()) graph_->removeConnection(c);
        for (auto& s : slots_)
            if (s.node) graph_->removeNode(s.node->nodeID);
    }
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
    if (prepared_ && graph_)
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
        result.push_back({ s.desc.name, s.bypassed });
    return result;
}

ChainHost::SlotInfo ChainHost::getSlotInfo(int i) const
{
    if (i < 0 || i >= (int)slots_.size()) return { {}, false };
    return { slots_[i].desc.name, slots_[i].bypassed };
}

void ChainHost::removeSlot(int i)
{
    if (i < 0 || i >= (int)slots_.size()) return;
    for (auto& c : graph_->getConnections()) graph_->removeConnection(c);
    if (slots_[i].node) graph_->removeNode(slots_[i].node->nodeID);
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

    if (active.empty())
    {
        // Pure passthrough
        if (inputNode_ && outputNode_)
        {
            graph_->addConnection({{inputNode_->nodeID, 0}, {outputNode_->nodeID, 0}});
            graph_->addConnection({{inputNode_->nodeID, 1}, {outputNode_->nodeID, 1}});
        }
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
            loadPluginAsync(e.desc, std::move(callback));
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
