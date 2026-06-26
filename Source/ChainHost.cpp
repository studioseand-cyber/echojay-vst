#include "ChainHost.h"

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
juce::File ChainHost::getDeadmanFile()    { return appSupportDir().getChildFile("chain_scan_deadman.txt"); }

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
ChainHost::ChainHost()
{
    // addDefaultFormats() is deleted in juce_audio_processors_headless.
    // Use addDefaultFormatsToManager() from juce_audio_processors instead.
    juce::addDefaultFormatsToManager(formatManager_);

    graph_ = std::make_unique<juce::AudioProcessorGraph>();

    // Add I/O nodes
    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode_  = graph_->addNode(std::make_unique<IOProc>(IOProc::audioInputNode));
    outputNode_ = graph_->addNode(std::make_unique<IOProc>(IOProc::audioOutputNode));

    rebuildPassthrough();

    loadFromDisk();

    // Check deadman file — if it exists with content, the last scan crashed on that file
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
    if (graph_)
        graph_->releaseResources();
    prepared_ = false;
}

void ChainHost::process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (prepared_ && graph_)
        graph_->processBlock(buffer, midi);
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------
void ChainHost::startScan()
{
    if (scanning_.load()) return;

    cancelFlag_.store(false);
    scanning_.store(true);
    scanProgress_.store(0.0f);

    if (scanThread_.joinable())
        scanThread_.join();

    scanThread_ = std::thread([this] { doScan(); });
}

void ChainHost::cancelScan()
{
    cancelFlag_.store(true);
    if (scanThread_.joinable())
        scanThread_.join();
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

void ChainHost::doScan()
{
    // RAII: always clear scanning flag and save on exit
    struct Finally {
        ChainHost* h;
        ~Finally() { h->scanning_.store(false); h->saveToDisk(); }
    } fin { this };

    // Clear existing list under lock
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        knownPlugins_.clear();
    }

    auto formats = formatManager_.getFormats();

    // First pass: count total files for progress tracking
    int totalFiles = 0;
    int filesScanned = 0;

    // Collect all (format, file) pairs
    struct ScanItem { juce::AudioPluginFormat* fmt; juce::File file; bool isAU; };
    std::vector<ScanItem> items;

    for (auto* fmt : formats)
    {
        if (cancelFlag_.load()) return;

        juce::String fmtName = fmt->getName();
        bool isAU  = fmtName.startsWithIgnoreCase("AudioUnit");
        bool isVST3 = fmtName.containsIgnoreCase("VST3") || fmtName.containsIgnoreCase("VST 3");

        if (!isAU && !isVST3) continue; // skip any unexpected formats

        auto searchPaths = fmt->getDefaultLocationsToSearch();

        for (int pi = 0; pi < searchPaths.getNumPaths(); ++pi)
        {
            if (cancelFlag_.load()) return;
            auto dir = searchPaths[pi];
            if (!dir.isDirectory()) continue;

            auto children = dir.findChildFiles(
                juce::File::findDirectories | juce::File::findFiles, false);

            for (auto& f : children)
            {
                juce::String ext = f.getFileExtension().toLowerCase();
                if (isAU  && ext == ".component") items.push_back({ fmt, f, true });
                if (isVST3 && ext == ".vst3")     items.push_back({ fmt, f, false });
            }
        }
    }

    totalFiles = (int)items.size();

    for (auto& item : items)
    {
        if (cancelFlag_.load()) return;

        juce::String filePath = item.file.getFullPathName();
        juce::String filename = item.file.getFileName();

        if (isBlacklisted(filePath))
        {
            filesScanned++;
            scanProgress_.store(totalFiles > 0 ? (float)filesScanned / (float)totalFiles : 1.0f);
            continue;
        }

        setScanStatus("Scanning: " + filename);

        if (item.isAU)
        {
            // AU: metadata-only scan — crash-safe
            juce::OwnedArray<juce::PluginDescription> found;
            item.fmt->findAllTypesForFile(found, filePath);
            {
                std::lock_guard<std::mutex> lock(pluginsMutex_);
                for (auto* d : found)
                    knownPlugins_.addType(*d);
            }
        }
        else
        {
            // VST3: run in a timeout thread with deadman file for crash detection
            auto deadman = getDeadmanFile();
            appSupportDir().createDirectory();
            deadman.replaceWithText(filePath);

            // Shared state for cross-thread result passing
            struct ScanState {
                std::atomic<bool> done { false };
                std::mutex mtx;
                std::vector<juce::PluginDescription> results;
            };
            auto state = std::make_shared<ScanState>();

            // Capture format pointer and path by value for the thread
            auto* fmt = item.fmt;
            juce::String path = filePath;
            auto weakState = std::weak_ptr<ScanState>(state);

            std::thread worker([fmt, path, weakState] {
                juce::OwnedArray<juce::PluginDescription> found;
                fmt->findAllTypesForFile(found, path);

                auto s = weakState.lock();
                if (!s) return;
                {
                    std::lock_guard<std::mutex> lk(s->mtx);
                    for (auto* d : found)
                        s->results.push_back(*d);
                }
                s->done.store(true);
            });
            worker.detach();

            // Poll up to 10 seconds with 100ms sleep
            bool timedOut = true;
            for (int t = 0; t < 100; ++t)
            {
                if (cancelFlag_.load()) break;
                if (state->done.load()) { timedOut = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (!timedOut && !cancelFlag_.load())
            {
                std::lock_guard<std::mutex> lock(pluginsMutex_);
                std::lock_guard<std::mutex> slk(state->mtx);
                for (auto& d : state->results)
                    knownPlugins_.addType(d);
                deadman.deleteFile();
            }
            else if (timedOut)
            {
                addToBlacklist(filePath);
                setScanStatus("Skipped (timed out): " + filename);
                // Leave deadman file so next launch can detect the crash
            }
        }

        filesScanned++;
        scanProgress_.store(totalFiles > 0 ? (float)filesScanned / (float)totalFiles : 1.0f);
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
    return knownPlugins_.getNumTypes();
}

juce::PluginDescription ChainHost::getPlugin(int index) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    if (index < 0 || index >= knownPlugins_.getNumTypes())
        return {};
    return *knownPlugins_.getType(index);
}

juce::Array<juce::PluginDescription> ChainHost::getFilteredPlugins(const juce::String& filter) const
{
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    juce::Array<juce::PluginDescription> result;

    if (filter.isEmpty())
    {
        for (int i = 0; i < knownPlugins_.getNumTypes(); ++i)
            result.add(*knownPlugins_.getType(i));
        return result;
    }

    juce::String lf = filter.toLowerCase();
    for (int i = 0; i < knownPlugins_.getNumTypes(); ++i)
    {
        auto* d = knownPlugins_.getType(i);
        if (d->name.toLowerCase().contains(lf) ||
            d->manufacturerName.toLowerCase().contains(lf))
            result.add(*d);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Hosting
// ---------------------------------------------------------------------------
juce::String ChainHost::loadPlugin(const juce::PluginDescription& desc)
{
    if (isScanning())
        return "Cannot load while scanning";

    if (pluginLoaded_)
        unloadPlugin();

    juce::String errorMessage;
    auto plugin = formatManager_.createPluginInstance(desc, sampleRate_, blockSize_, errorMessage);
    if (!plugin)
        return errorMessage.isNotEmpty() ? errorMessage : "Failed to create plugin instance";

    hostedNode_ = graph_->addNode(std::move(plugin));
    if (!hostedNode_)
        return "Failed to add plugin to graph";

    rebuildWithPlugin();

    if (prepared_)
        graph_->prepareToPlay(sampleRate_, blockSize_);

    pluginLoaded_ = true;
    loadedDesc_   = desc;
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

    if (prepared_)
        graph_->prepareToPlay(sampleRate_, blockSize_);

    pluginLoaded_ = false;
}

juce::String ChainHost::getLoadedPluginName() const
{
    if (!pluginLoaded_) return {};
    return loadedDesc_.name;
}

juce::AudioProcessorEditor* ChainHost::createHostedEditor()
{
    if (!pluginLoaded_ || !hostedNode_) return nullptr;
    try
    {
        auto* proc = hostedNode_->getProcessor();
        if (!proc) return nullptr;
        return proc->createEditor();
    }
    catch (...)
    {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Graph wiring
// ---------------------------------------------------------------------------
void ChainHost::rebuildPassthrough()
{
    if (!graph_) return;
    auto connections = graph_->getConnections();
    for (auto& c : connections)
        graph_->removeConnection(c);

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
    for (auto& c : connections)
        graph_->removeConnection(c);

    if (!inputNode_ || !outputNode_ || !hostedNode_)
    {
        rebuildPassthrough();
        return;
    }

    auto* proc = hostedNode_->getProcessor();
    int nIn  = proc ? proc->getTotalNumInputChannels()  : 0;
    int nOut = proc ? proc->getTotalNumOutputChannels() : 0;

    for (int ch = 0; ch < juce::jmin(2, nIn); ++ch)
        graph_->addConnection({{inputNode_->nodeID, ch}, {hostedNode_->nodeID, ch}});

    for (int ch = 0; ch < juce::jmin(2, nOut); ++ch)
        graph_->addConnection({{hostedNode_->nodeID, ch}, {outputNode_->nodeID, ch}});

    // Passthrough for any output channels not covered by the plugin
    for (int ch = nOut; ch < 2; ++ch)
        graph_->addConnection({{inputNode_->nodeID, ch}, {outputNode_->nodeID, ch}});
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
    if (!blacklist_.contains(path))
        blacklist_.add(path);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void ChainHost::saveToDisk() const
{
    appSupportDir().createDirectory();

    // Save plugin list as XML
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        auto xml = knownPlugins_.createXml();
        if (xml)
        {
            auto f = getPluginListFile();
            xml->writeTo(f);
        }
    }

    // Save blacklist (one path per line)
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        juce::String bl;
        for (auto& p : blacklist_)
            bl += p + "\n";
        getBlacklistFile().replaceWithText(bl);
    }
}

void ChainHost::loadFromDisk()
{
    // Load plugin list
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
    {
        loadPlugin(desc);
        // Editor will be created when user opens CHAIN tab
    }
}
