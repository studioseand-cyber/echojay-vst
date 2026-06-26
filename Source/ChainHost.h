#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>

// Manages plugin discovery and hosting via AudioProcessorGraph.
// Owned by EchoJayProcessor.
//
// Discovery is list-then-load-on-demand:
//   startScan() – enumerates plugins by reading the OS registry/filesystem,
//                 WITHOUT instantiating any plugin code.
//     AU:   JUCE AudioUnitPluginFormat queries the CoreAudio registry
//           (metadata-only, no dylib loaded) — crash-safe by design.
//     VST3: filesystem walk only; name from bundle filename, no loading.
//
//   loadPlugin() – called once the user picks a plugin.
//     AU:   already has a full PluginDescription → createPluginInstance().
//     VST3: if not in the validated cache, runs findAllTypesForFile() for
//           that ONE bundle in a detached thread (10 s timeout + deadman
//           crash guard), then createPluginInstance().
class ChainHost
{
public:
    ChainHost();
    ~ChainHost();

    // ---- Audio thread hooks -----------------------------------------------
    void prepare(double sampleRate, int blockSize);
    void release();
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // ---- List refresh (message thread) -----------------------------------
    // Enumerates AU (registry) + VST3 (filesystem) without loading plugins.
    void startScan();
    void cancelScan();
    bool  isScanning()      const noexcept { return scanning_.load(); }
    float getScanProgress() const noexcept { return scanProgress_.load(); }
    juce::String getScanStatus() const;

    // ---- Plugin list (message thread) ------------------------------------
    int getNumPlugins() const;
    juce::Array<juce::PluginDescription> getFilteredPlugins(const juce::String& filter) const;

    // ---- Hosting (message thread only) -----------------------------------
    // desc comes from getFilteredPlugins().  AU entries are fully-described;
    // VST3 entries may be thin (name+path) and are validated lazily here.
    // Returns "" on success, error string on failure.
    juce::String loadPlugin(const juce::PluginDescription& desc);
    void unloadPlugin();
    bool isPluginLoaded() const noexcept { return pluginLoaded_; }
    juce::String getLoadedPluginName() const;
    juce::AudioProcessorEditor* createHostedEditor();

    // ---- State persistence -----------------------------------------------
    void saveToDisk() const;
    void loadFromDisk();
    juce::String getLoadedDescXml() const;
    void tryRestoreFromXml(const juce::String& xml);

    bool chainWarningDismissed = false;

private:
    juce::AudioPluginFormatManager formatManager_;

    // entries_      – UI list built by doRefresh() (full AU + thin VST3).
    // knownPlugins_ – cache of validated VST3 descriptions, saved to disk.
    mutable std::mutex  pluginsMutex_;
    juce::Array<juce::PluginDescription> entries_;
    juce::KnownPluginList knownPlugins_;
    juce::StringArray     blacklist_;   // paths that crashed/timed-out

    // AudioProcessorGraph
    std::unique_ptr<juce::AudioProcessorGraph> graph_;
    juce::AudioProcessorGraph::Node::Ptr inputNode_, outputNode_, hostedNode_;

    bool   pluginLoaded_ = false;
    juce::PluginDescription loadedDesc_;
    double sampleRate_ = 44100.0;
    int    blockSize_  = 512;
    bool   prepared_   = false;

    // Refresh thread
    std::atomic<bool>  scanning_     { false };
    std::atomic<bool>  cancelFlag_   { false };
    std::atomic<float> scanProgress_ { 0.0f };
    mutable std::mutex statusMutex_;
    juce::String       scanStatus_;
    std::thread        scanThread_;

    void doRefresh();
    void setScanStatus(const juce::String& s);
    bool isBlacklisted(const juce::String& path) const;
    void addToBlacklist(const juce::String& path);
    juce::AudioPluginFormat* getFormatByName(const juce::String& namePart) const;

    void rebuildPassthrough();
    void rebuildWithPlugin();

    static juce::File getPluginListFile();
    static juce::File getBlacklistFile();
    static juce::File getDeadmanFile();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainHost)
};
