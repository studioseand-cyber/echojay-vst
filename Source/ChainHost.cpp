#include "ChainHost.h"
#include "AUEnumerator.h"
#include <algorithm>

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

    rebuildPassthrough();
    loadFromDisk();

    // Deadman check: if present, the last single-plugin load crashed
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

    if (pluginLoaded_ && hostedNode_ != nullptr && graph_ != nullptr)
    {
        auto connections = graph_->getConnections();
        for (auto& c : connections) graph_->removeConnection(c);
        graph_->removeNode(hostedNode_->nodeID);
        hostedNode_ = nullptr;
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

    // ---- Step 1: AU via CoreAudio registry (no instantiation) ----------------
    // enumerateAUs() is compiled in AUEnumerator.mm (no JUCE headers) to avoid
    // the CoreAudio/juce::AudioBuffer typedef conflict.
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

    // ---- Step 2: VST3 via filesystem walk (no instantiation) -----------------
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

                // Check validated cache — full description already known?
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
                    // Thin entry: name from bundle filename, no loading
                    juce::PluginDescription thin;
                    thin.name             = f.getFileNameWithoutExtension();
                    thin.pluginFormatName = "VST3";
                    thin.fileOrIdentifier = path;
                    thin.category         = "Effect";
                    // version left empty → signals "thin, needs validation"
                    vst3Entries.add(thin);
                }
            }
        }
    }

    scanProgress_.store(0.9f);

    // ---- Step 3: Dedup — prefer AU over VST3 of same name ------------------
    std::unordered_set<std::string> auNames;
    for (auto& d : auEntries)
        auNames.insert(d.name.toLowerCase().toStdString());

    juce::Array<juce::PluginDescription> collected;
    for (auto& d : auEntries) collected.add(d);
    for (auto& d : vst3Entries)
        if (auNames.find(d.name.toLowerCase().toStdString()) == auNames.end())
            collected.add(d);

    // Sort alphabetically by name
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
// Hosting — async load
// ---------------------------------------------------------------------------

// Internal: wire the newly created instance into the graph.
// Called on the message thread from createPluginInstanceAsync callback.
void ChainHost::completeLoad(std::unique_ptr<juce::AudioPluginInstance> inst,
                              const juce::PluginDescription& desc)
{
    loadedDesc_   = desc;
    hostedNode_   = graph_->addNode(std::move(inst));
    rebuildWithPlugin();
    if (prepared_)
    {
        graph_->setPlayConfigDetails(2, 2, sampleRate_, blockSize_);
        graph_->prepareToPlay(sampleRate_, blockSize_);
    }
    pluginLoaded_ = true;
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

        // asyncCreatePlugin is a public helper wrapping formatManager_ + sampleRate_ + blockSize_
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

    // Poll again after 100 ms (non-blocking, no message-thread sleep)
    juce::Timer::callAfterDelay(100, [host, vs, desc, deadman, cb, ticksLeft]() mutable {
        pollVST3Validation(host, vs, desc, deadman, cb, ticksLeft - 1);
    });
}

void ChainHost::loadPluginAsync(const juce::PluginDescription& desc,
                                std::function<void(const juce::String& error)> callback)
{
    if (pluginLoaded_) unloadPlugin();

    // Determine if we need VST3 validation (thin entry: version empty = never validated)
    bool needsValidation = (desc.pluginFormatName == "VST3" && desc.version.isEmpty());

    juce::PluginDescription fullDesc = desc;
    if (needsValidation)
    {
        // Check validated cache first
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
        // AU or already-cached VST3: go straight to async instantiation
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

    // Thin VST3: run findAllTypesForFile in a detached thread, poll result
    appSupportDir().createDirectory();
    auto deadman = getDeadmanFile();
    deadman.replaceWithText(desc.fileOrIdentifier);

    auto* vst3Fmt = getFormatByName("VST3");
    if (!vst3Fmt) { callback("VST3 format not available"); return; }

    auto vs = std::make_shared<VST3ValState>();
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

    pollVST3Validation(this, vs, desc, deadman, callback, 100 /*10 s*/);
}

void ChainHost::unloadPlugin()
{
    if (!pluginLoaded_ || !hostedNode_) return;
    auto connections = graph_->getConnections();
    for (auto& c : connections) graph_->removeConnection(c);
    graph_->removeNode(hostedNode_->nodeID);
    hostedNode_ = nullptr;
    rebuildPassthrough();
    if (prepared_) graph_->prepareToPlay(sampleRate_, blockSize_);
    pluginLoaded_ = false;
}

juce::String ChainHost::getLoadedPluginName() const
{
    return pluginLoaded_ ? loadedDesc_.name : juce::String{};
}

juce::AudioProcessorEditor* ChainHost::createHostedEditor()
{
    if (!pluginLoaded_ || !hostedNode_) return nullptr;
    try
    {
        auto* proc = hostedNode_->getProcessor();
        return proc ? proc->createEditor() : nullptr;
    }
    catch (...) { return nullptr; }
}

// ---------------------------------------------------------------------------
// Graph wiring
// ---------------------------------------------------------------------------
void ChainHost::rebuildPassthrough()
{
    if (!graph_) return;
    auto connections = graph_->getConnections();
    for (auto& c : connections) graph_->removeConnection(c);
    if (inputNode_ && outputNode_)
    {
        graph_->addConnection({{inputNode_->nodeID, 0}, {outputNode_->nodeID, 0}});
        graph_->addConnection({{inputNode_->nodeID, 1}, {outputNode_->nodeID, 1}});
    }
}

void ChainHost::rebuildWithPlugin()
{
    if (!graph_) return;
    auto connections = graph_->getConnections();
    for (auto& c : connections) graph_->removeConnection(c);

    if (!inputNode_ || !outputNode_ || !hostedNode_) { rebuildPassthrough(); return; }

    auto* proc = hostedNode_->getProcessor();
    int nIn  = proc ? proc->getTotalNumInputChannels()  : 0;
    int nOut = proc ? proc->getTotalNumOutputChannels() : 0;

    for (int ch = 0; ch < juce::jmin(2, nIn);  ++ch)
        graph_->addConnection({{inputNode_->nodeID,  ch}, {hostedNode_->nodeID, ch}});
    for (int ch = 0; ch < juce::jmin(2, nOut); ++ch)
        graph_->addConnection({{hostedNode_->nodeID, ch}, {outputNode_->nodeID, ch}});
    // Passthrough for uncovered output channels
    for (int ch = nOut; ch < 2; ++ch)
        graph_->addConnection({{inputNode_->nodeID, ch}, {outputNode_->nodeID, ch}});
}

// ---------------------------------------------------------------------------
// Format lookup helper
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
// Blacklist helpers
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
// Persistence
// ---------------------------------------------------------------------------
void ChainHost::saveToDisk() const
{
    appSupportDir().createDirectory();
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        auto xml = knownPlugins_.createXml();
        if (xml) xml->writeTo(getPluginListFile());
    }
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        juce::String bl;
        for (auto& p : blacklist_) bl += p + "\n";
        getBlacklistFile().replaceWithText(bl);
    }
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

juce::String ChainHost::getLoadedDescXml() const
{
    if (!pluginLoaded_) return {};
    auto xml = loadedDesc_.createXml();
    return xml ? xml->toString() : juce::String{};
}

void ChainHost::tryRestoreFromXml(const juce::String& xml)
{
    if (xml.isEmpty()) return;
    auto elem = juce::XmlDocument::parse(xml);
    if (!elem) return;
    juce::PluginDescription desc;
    if (!desc.loadFromXml(*elem)) return;
    loadPluginAsync(desc, [](const juce::String&) {});
}
