#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>

// Manages plugin discovery and hosting via AudioProcessorGraph.
// Owned by EchoJayProcessor.
//
// Discovery — list-then-load-on-demand, no bulk instantiation:
//   startScan() reads names/metadata ONLY:
//     AU:   CoreAudio AudioComponentFindNext() — pure registry query, no dylib
//     VST3: filesystem walk, name from bundle filename, no loading
//   Results deduplicated: where both AU and VST3 exist for the same name,
//   the AU entry is kept (preferred when running as an AU inside Logic).
//
//   loadPluginAsync() — called when the user picks ONE plugin:
//     AU / cached VST3: PluginDescription is already full → createPluginInstanceAsync()
//     Thin VST3: findAllTypesForFile() in a detached thread (10 s timeout +
//                deadman crash guard), result posted back to message thread via
//                callAfterDelay polling, then createPluginInstanceAsync().
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
    // Enumerates AU (CoreAudio registry) + VST3 (filesystem).
    // Does NOT instantiate any plugin. Fast, crash-safe.
    void startScan();
    void cancelScan();
    bool  isScanning()      const noexcept { return scanning_.load(); }
    float getScanProgress() const noexcept { return scanProgress_.load(); }
    juce::String getScanStatus() const;

    // ---- Plugin list (message thread) ------------------------------------
    int getNumPlugins() const;
    juce::Array<juce::PluginDescription> getFilteredPlugins(const juce::String& filter) const;

    // ---- Hosting (message thread only) -----------------------------------
    // Asynchronously loads the plugin:
    //   callback(error) — error is empty on success, non-empty on failure.
    //   On success, call createHostedEditor() to get the plugin's UI.
    // NOTE: ChainHost must outlive the callback (EchoJayProcessor guarantees this).
    void loadPluginAsync(const juce::PluginDescription& desc,
                         std::function<void(const juce::String& error)> callback);

    // Public helpers used by the static pollVST3Validation free function
    // (free functions cannot access private members).
    void asyncCreatePlugin(const juce::PluginDescription& d,
        std::function<void(std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)> cb)
    {
        formatManager_.createPluginInstanceAsync(d, sampleRate_, blockSize_, std::move(cb));
    }
    void completeLoad(std::unique_ptr<juce::AudioPluginInstance> inst,
                      const juce::PluginDescription& desc);
    void addToBlacklist(const juce::String& path);

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

    // entries_      – UI list from doRefresh() (AU + thin VST3, deduplicated)
    // knownPlugins_ – validated VST3 descriptions (disk cache)
    mutable std::mutex  pluginsMutex_;
    juce::Array<juce::PluginDescription> entries_;
    juce::KnownPluginList knownPlugins_;
    juce::StringArray     blacklist_;   // paths that crashed / timed-out

    // AudioProcessorGraph: input → [hosted plugin] → output
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

    // Internal helpers
    void doRefresh();
    void setScanStatus(const juce::String& s);
    bool isBlacklisted(const juce::String& path) const;
    juce::AudioPluginFormat* getFormatByName(const juce::String& namePart) const;

    void rebuildPassthrough();
    void rebuildWithPlugin();

    static juce::File getPluginListFile();   // validated VST3 cache
    static juce::File getBlacklistFile();
    static juce::File getDeadmanFile();      // crash detection for single load

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainHost)
};
