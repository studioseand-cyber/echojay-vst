#pragma once
#include <JuceHeader.h>
#include "PluginScanner.h"
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

// Manages plugin discovery and hosting via AudioProcessorGraph.
// Owned by EchoJayProcessor.
//
// Discovery — list-then-load-on-demand, no bulk instantiation:
//   startScan() reads names/metadata ONLY.
//   AU: CoreAudio AudioComponentFindNext() — pure registry query, no dylib
//   VST3: filesystem walk, name from bundle filename, no loading
//   Results deduplicated: AU preferred over VST3 of same name.
//
// Hosting — ordered chain of AU/VST3 plugins in series:
//   loadPluginAsync() appends a new slot to the end.
//   Bypassed slots are skipped in routing (passthrough) but remain in list.
//   rebuildGraph() runs on the message thread; JUCE's internal graph lock
//   makes message-thread modifications and audio-thread processBlock safe.
class ChainHost
{
public:
    // Lightweight slot description safe to copy to the UI thread
    struct SlotInfo {
        juce::String name;
        bool bypassed;
    };

    ChainHost();
    ~ChainHost();

    // ---- Audio thread hooks -----------------------------------------------
    void prepare(double sampleRate, int blockSize);
    void release();
    void process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    // ---- List refresh (message thread) -----------------------------------
    void startScan();
    void cancelScan();
    bool  isScanning()      const noexcept { return scanning_.load(); }
    float getScanProgress() const noexcept { return scanProgress_.load(); }
    juce::String getScanStatus() const;

    // ---- Plugin list (message thread) ------------------------------------
    int getNumPlugins() const;
    juce::Array<juce::PluginDescription> getFilteredPlugins(
        const juce::String& filter,
        const juce::String& formatFilter = {}) const;

    // ---- Settings ↔ ChainHost resolver (message thread) -----------------
    // A "recommendable" plugin is BOTH enabled in the Settings checklist AND
    // present in ChainHost's loadable entries_ list (same format filter applies).
    //
    // Call buildRecommendable() after a scan completes or after Settings are
    // saved, passing the current enabled plugin list and the active format filter.
    // The result is cached internally and returned by the two accessors below.
    struct RecommendableEntry {
        juce::String            displayName;
        juce::PluginDescription desc;
    };
    void buildRecommendable(const std::vector<ScannedPlugin>& enabledPlugins,
                            const juce::String& formatFilter);

    // Display names of resolved entries (for AI prompt injection).
    juce::StringArray getRecommendableNames() const;

    // Count stats from the last buildRecommendable() call.
    int getRecommendableCount()   const noexcept { return (int)recommendable_.size(); }
    int getEnabledInputCount()    const noexcept { return recommendableEnabledIn_; }
    int getUnmatchedCount()       const noexcept { return recommendableEnabledIn_ - (int)recommendable_.size(); }

    // Async-load the first recommendable entry whose displayName matches `name`
    // (case-insensitive). Callback: empty string on success, error message on fail.
    void loadByRecommendedName(const juce::String& name,
                               std::function<void(const juce::String&)> callback);

    // ---- Chain slot management (message thread) --------------------------
    // Async-append: loads the plugin and adds it to the end of the chain.
    // callback(error) — empty on success.
    void loadPluginAsync(const juce::PluginDescription& desc,
                         std::function<void(const juce::String& error)> callback);

    int                      getNumSlots()    const noexcept;
    std::vector<SlotInfo>    getAllSlotInfos() const;
    SlotInfo                 getSlotInfo(int i) const;

    void removeSlot(int i);
    void moveSlot(int i, int direction);    // direction: -1 = left, +1 = right
    void setSlotBypassed(int i, bool bypassed);

    juce::AudioProcessorEditor* createEditorForSlot(int i);

    // ---- State persistence -----------------------------------------------
    void saveToDisk() const;
    void loadFromDisk();
    juce::String getSlotsStateXml() const;
    void tryRestoreSlotsFromXml(const juce::String& xml);

    bool chainWarningDismissed = false;

    // ---- Public helpers for static pollVST3Validation free function ------
    // (Free functions cannot access private members in C++.)
    void asyncCreatePlugin(const juce::PluginDescription& d,
        std::function<void(std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)> cb)
    {
        formatManager_.createPluginInstanceAsync(d, sampleRate_, blockSize_, std::move(cb));
    }
    void completeLoad(std::unique_ptr<juce::AudioPluginInstance> inst,
                      const juce::PluginDescription& desc);
    void addToBlacklist(const juce::String& path);

    double sampleRate_ = 44100.0;  // public so pollVST3Validation can read
    int    blockSize_  = 512;

private:
    struct ChainSlot {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::PluginDescription              desc;
        bool                                 bypassed = false;
    };

    juce::AudioPluginFormatManager formatManager_;

    // Plugin list (from scan)
    mutable std::mutex  pluginsMutex_;
    juce::Array<juce::PluginDescription> entries_;
    juce::KnownPluginList                knownPlugins_;
    juce::StringArray                    blacklist_;

    // AudioProcessorGraph: input → [active slots in order] → output
    std::unique_ptr<juce::AudioProcessorGraph> graph_;
    juce::AudioProcessorGraph::Node::Ptr inputNode_, outputNode_;

    std::vector<ChainSlot> slots_;

    bool   prepared_  = false;

    // Resolver cache (message thread only — no mutex needed)
    std::vector<RecommendableEntry> recommendable_;
    int recommendableEnabledIn_ = 0;   // how many enabled scanner entries were fed in

    // Scan thread
    std::atomic<bool>  scanning_     { false };
    std::atomic<bool>  cancelFlag_   { false };
    std::atomic<float> scanProgress_ { 0.0f };
    mutable std::mutex statusMutex_;
    juce::String       scanStatus_;
    std::thread        scanThread_;

    // Restore helper (sequential async restore of multiple slots)
    struct RestoreItem { juce::PluginDescription desc; bool bypassed; };
    void restoreNextSlot(std::vector<RestoreItem> items, int idx);

    // Internal helpers
    void rebuildGraph();
    void doRefresh();
    void setScanStatus(const juce::String& s);
    bool isBlacklisted(const juce::String& path) const;
    juce::AudioPluginFormat* getFormatByName(const juce::String& namePart) const;

    static juce::File getPluginListFile();
    static juce::File getBlacklistFile();
    static juce::File getDeadmanFile();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainHost)
};
