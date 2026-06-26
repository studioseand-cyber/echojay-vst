#include "ChainHost.h"
#include <algorithm>
#include <chrono>

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

// (sort done via std::sort below — no comparator struct needed)

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

    // Deadman check: if present, last single-load crashed — blacklist that path
    auto deadman = getDeadmanFile();
    if (deadman.existsAsFile())
    {
        juce::String crashed = deadman.loadFileAsString().trim();
        if (crashed.isNotEmpty())
            addToBlacklist(crashed);
        deadman.deleteFile();
    }
}

ChainHost::~ChainHost()
{
    cancelFlag_.store(true);
    if (scanThread_.joinable())
        scanThread_.join();

    if (pluginLoaded_ && hostedNode_ != nullptr && graph_ != nullptr)
    {
        auto connections = graph_->getConnections();
        for (auto& c : connections)
            graph_->removeConnection(c);
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
// List refresh — no plugin instantiation, just enumeration
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
    // Always clear scanning flag on exit
    struct Finally { ChainHost* h; ~Finally() { h->scanning_.store(false); } } fin{this};

    juce::Array<juce::PluginDescription> collected;

    auto* auFmt   = getFormatByName("AudioUnit");
    auto* vst3Fmt = getFormatByName("VST3");

    // ---- AU: JUCE AU format queries the CoreAudio registry (no dylib load) ----
    if (auFmt && !cancelFlag_.load())
    {
        setScanStatus("Reading AU registry...");

        juce::StringArray componentPaths;
        auto paths = auFmt->getDefaultLocationsToSearch();
        for (int pi = 0; pi < paths.getNumPaths(); ++pi)
        {
            auto dir = paths[pi];
            if (!dir.isDirectory()) continue;
            auto found = dir.findChildFiles(
                juce::File::findDirectories, false, "*.component");
            for (auto& f : found)
                componentPaths.addIfNotAlreadyThere(f.getFullPathName());
        }

        int total = componentPaths.size(), done = 0;
        for (auto& path : componentPaths)
        {
            if (cancelFlag_.load()) break;
            if (isBlacklisted(path)) { done++; continue; }

            juce::OwnedArray<juce::PluginDescription> types;
            // AU findAllTypesForFile = registry query only, never loads dylib
            auFmt->findAllTypesForFile(types, path);
            for (auto* d : types)
                collected.add(*d);

            scanProgress_.store((float)++done / (float)juce::jmax(1, total) * 0.6f);
        }
    }

    // ---- VST3: filesystem walk only, NO findAllTypesForFile, NO loading ----
    if (vst3Fmt && !cancelFlag_.load())
    {
        setScanStatus("Reading VST3 folders...");

        juce::StringArray bundlePaths;
        auto paths = vst3Fmt->getDefaultLocationsToSearch();
        for (int pi = 0; pi < paths.getNumPaths(); ++pi)
        {
            auto dir = paths[pi];
            if (!dir.isDirectory()) continue;
            auto found = dir.findChildFiles(
                juce::File::findDirectories | juce::File::findFiles, false, "*.vst3");
            for (auto& f : found)
                bundlePaths.addIfNotAlreadyThere(f.getFullPathName());
        }

        int total = bundlePaths.size(), done = 0;
        for (auto& path : bundlePaths)
        {
            if (cancelFlag_.load()) break;
            if (isBlacklisted(path)) { done++; continue; }

            // Check validated cache first
            bool usedCache = false;
            {
                std::lock_guard<std::mutex> lk(pluginsMutex_);
                for (int i = 0; i < knownPlugins_.getNumTypes(); ++i)
                {
                    auto* d = knownPlugins_.getType(i);
                    if (d->fileOrIdentifier == path)
                    {
                        collected.add(*d);
                        usedCache = true;
                        // A single .vst3 can host multiple types; keep checking
                    }
                }
            }

            if (!usedCache)
            {
                // Thin entry: name from bundle filename (no loading)
                juce::PluginDescription thin;
                thin.name              = juce::File(path).getFileNameWithoutExtension();
                thin.pluginFormatName  = "VST3";
                thin.fileOrIdentifier  = path;
                thin.category          = "Effect";
                collected.add(thin);
            }

            scanProgress_.store(0.6f + (float)++done / (float)juce::jmax(1, total) * 0.4f);
        }
    }

    // Sort and store
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

juce::Array<juce::PluginDescription> ChainHost::getFilteredPlugins(const juce::String& filter) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    juce::Array<juce::PluginDescription> result;
    juce::String lf = filter.toLowerCase();
    for (auto& d : entries_)
    {
        if (lf.isEmpty()
            || d.name.toLowerCase().contains(lf)
            || d.manufacturerName.toLowerCase().contains(lf))
            result.add(d);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Hosting
// ---------------------------------------------------------------------------
juce::String ChainHost::loadPlugin(const juce::PluginDescription& desc)
{
    if (pluginLoaded_) unloadPlugin();

    // For AU, we always have a full PluginDescription from the CoreAudio
    // registry query in doRefresh(). Go straight to createPluginInstance().
    //
    // For VST3, the entry may be "thin" (version empty, uid 0). In that case
    // we run findAllTypesForFile() for this one bundle in a guarded thread,
    // check the result into knownPlugins_ cache, then instantiate.

    juce::PluginDescription fullDesc = desc;

    bool needsValidation = (desc.pluginFormatName == "VST3" && desc.version.isEmpty());

    if (needsValidation)
    {
        // 1. Check validated cache
        bool foundInCache = false;
        {
            std::lock_guard<std::mutex> lk(pluginsMutex_);
            for (int i = 0; i < knownPlugins_.getNumTypes(); ++i)
            {
                auto* d = knownPlugins_.getType(i);
                if (d->fileOrIdentifier == desc.fileOrIdentifier)
                {
                    fullDesc = *d;
                    foundInCache = true;
                    break;
                }
            }
        }

        if (!foundInCache)
        {
            // 2. Single-file validation (timeout-guarded, deadman crash guard)
            appSupportDir().createDirectory();
            auto deadman = getDeadmanFile();
            deadman.replaceWithText(desc.fileOrIdentifier);

            struct ScanState {
                std::atomic<bool> done { false };
                std::mutex mtx;
                juce::PluginDescription result;
                bool found = false;
            };
            auto state = std::make_shared<ScanState>();

            auto* vst3Fmt = getFormatByName("VST3");
            if (!vst3Fmt)
                return "VST3 format not available";

            auto path = desc.fileOrIdentifier;
            auto weakState = std::weak_ptr<ScanState>(state);

            std::thread worker([vst3Fmt, path, weakState] {
                juce::OwnedArray<juce::PluginDescription> found;
                vst3Fmt->findAllTypesForFile(found, path);
                if (auto s = weakState.lock())
                {
                    if (!found.isEmpty())
                    {
                        std::lock_guard<std::mutex> lk(s->mtx);
                        s->result = *found[0];
                        s->found = true;
                    }
                    s->done.store(true);
                }
            });
            worker.detach();

            bool timedOut = true;
            for (int t = 0; t < 100; ++t)
            {
                if (state->done.load()) { timedOut = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (timedOut || !state->found)
            {
                addToBlacklist(desc.fileOrIdentifier);
                deadman.deleteFile();
                return "Could not load " + desc.name
                       + (timedOut ? " (timed out — added to skip list)" : " (no types found)");
            }

            {
                std::lock_guard<std::mutex> lk(state->mtx);
                fullDesc = state->result;
            }
            deadman.deleteFile();

            // Cache validated description
            {
                std::lock_guard<std::mutex> lk(pluginsMutex_);
                knownPlugins_.addType(fullDesc);
            }
            saveToDisk();
        }
    }

    // 3. Instantiate
    juce::String errorMessage;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    try
    {
        plugin = formatManager_.createPluginInstance(
            fullDesc, sampleRate_, blockSize_, errorMessage);
    }
    catch (...)
    {
        return "Plugin threw an exception during instantiation: " + desc.name;
    }

    if (!plugin)
        return errorMessage.isNotEmpty() ? errorMessage : "Failed to create plugin instance";

    hostedNode_ = graph_->addNode(std::move(plugin));
    if (!hostedNode_)
        return "Failed to add plugin to graph";

    rebuildWithPlugin();
    if (prepared_) graph_->prepareToPlay(sampleRate_, blockSize_);

    pluginLoaded_ = true;
    loadedDesc_   = fullDesc;
    return {};
}

void ChainHost::unloadPlugin()
{
    if (!pluginLoaded_ || !hostedNode_) return;

    auto connections = graph_->getConnections();
    for (auto& c : connections)
        graph_->removeConnection(c);

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

    if (!inputNode_ || !outputNode_ || !hostedNode_)
    { rebuildPassthrough(); return; }

    auto* proc = hostedNode_->getProcessor();
    int nIn  = proc ? proc->getTotalNumInputChannels()  : 0;
    int nOut = proc ? proc->getTotalNumOutputChannels() : 0;

    for (int ch = 0; ch < juce::jmin(2, nIn); ++ch)
        graph_->addConnection({{inputNode_->nodeID, ch}, {hostedNode_->nodeID, ch}});
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

    // Save validated VST3 cache
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        auto xml = knownPlugins_.createXml();
        if (xml) xml->writeTo(getPluginListFile());
    }

    // Save blacklist
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        juce::String bl;
        for (auto& p : blacklist_) bl += p + "\n";
        getBlacklistFile().replaceWithText(bl);
    }
}

void ChainHost::loadFromDisk()
{
    // Load validated VST3 cache
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

    // Load blacklist
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
    if (desc.loadFromXml(*elem))
        loadPlugin(desc);
}
