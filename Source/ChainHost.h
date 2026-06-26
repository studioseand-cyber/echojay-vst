#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

// Manages plugin discovery (with crash-safety for VST3 via per-file timeout threads)
// and hosting via AudioProcessorGraph. Owned by EchoJayProcessor.
//
// Stage 1 out-of-process approach:
//   AU  – AudioUnitPluginFormat uses AudioComponent registry (metadata-only, no dylib
//         loaded during scan) — crash-safe by design.
//   VST3 – Each file's findAllTypesForFile() runs in a detached std::thread with a
//           shared-ptr-guarded state. A 10 s timeout detects hangs; a deadman file
//           detects crashes on the next run. Stage 2 will upgrade to posix_spawn.
class ChainHost
{
public:
    ChainHost();
    ~ChainHost();

    // ---- Audio thread hooks (called by EchoJayProcessor) -----------------
    void prepare(double sampleRate, int blockSize);
    void release();
    // Routes buffer through the hosted plugin (or passthrough if none loaded).
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // ---- Scanning (message thread) ----------------------------------------
    void startScan();
    void cancelScan();
    bool  isScanning()      const noexcept { return scanning_.load(); }
    float getScanProgress() const noexcept { return scanProgress_.load(); }
    juce::String getScanStatus() const;

    // ---- Plugin list (message thread) ------------------------------------
    int getNumPlugins() const;
    juce::PluginDescription getPlugin(int index) const;
    juce::Array<juce::PluginDescription> getFilteredPlugins(const juce::String& filter) const;

    // ---- Hosting (message thread only) -----------------------------------
    // Returns "" on success, an error string on failure.
    juce::String loadPlugin(const juce::PluginDescription& desc);
    void unloadPlugin();
    bool isPluginLoaded() const noexcept { return pluginLoaded_; }
    juce::String getLoadedPluginName() const;
    // Creates the hosted plugin's editor. Caller owns the returned pointer.
    // Must be called (and the result deleted) on the message thread.
    juce::AudioProcessorEditor* createHostedEditor();

    // ---- State persistence -----------------------------------------------
    void saveToDisk() const;
    void loadFromDisk();
    // Serialise/restore the currently loaded plugin description across DAW sessions.
    juce::String getLoadedDescXml() const;
    void tryRestoreFromXml(const juce::String& xml);

    // Whether the user has dismissed the "experimental" warning on this machine.
    bool chainWarningDismissed = false;

private:
    juce::AudioPluginFormatManager formatManager_;

    // Discovered plugins (guarded by pluginsMutex_)
    mutable std::mutex  pluginsMutex_;
    juce::KnownPluginList knownPlugins_;
    juce::StringArray   blacklist_;   // file paths that crashed/hung

    // AudioProcessorGraph: input -> [plugin] -> output (passthrough if no plugin).
    // graph_->processBlock() is called on the audio thread; addNode/removeNode
    // are called on the message thread. JUCE's graph uses its own callback lock.
    std::unique_ptr<juce::AudioProcessorGraph> graph_;
    juce::AudioProcessorGraph::Node::Ptr inputNode_, outputNode_, hostedNode_;

    // Loaded plugin state (message thread)
    bool                pluginLoaded_ = false;
    juce::PluginDescription loadedDesc_;
    double sampleRate_ = 44100.0;
    int    blockSize_  = 512;
    bool   prepared_   = false;

    // Scan state
    std::atomic<bool>  scanning_       { false };
    std::atomic<bool>  cancelFlag_     { false };
    std::atomic<float> scanProgress_   { 0.0f };
    mutable std::mutex statusMutex_;
    juce::String       scanStatus_;
    std::thread        scanThread_;

    void doScan();
    void setScanStatus(const juce::String& s);
    bool isBlacklisted(const juce::String& path) const;
    void addToBlacklist(const juce::String& path);

    void rebuildPassthrough();
    void rebuildWithPlugin();

    static juce::File getPluginListFile();
    static juce::File getBlacklistFile();
    static juce::File getDeadmanFile();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainHost)
};
