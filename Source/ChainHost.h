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
        juce::String settings;  // suggested dial-in guidance from AI (display only)
        juce::String format;    // "AudioUnit" / "VST3" — popout-only is per-format
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
    void setSlotSettings(int i, const juce::String& settings);  // store AI guidance text

    juce::AudioProcessorEditor* createEditorForSlot(int i);

    // ---- Shared name resolution (both hosts MUST behave identically) ----
    // The AI feeds names in two formats: plain ("CREAM2PRE") from the chain
    // injection, and "Name (Manufacturer)" from the general plugin feed —
    // models copy either into chain blocks. Loose matching handles both,
    // plus case/whitespace/punctuation/version drift via normalizeName.
    static juce::String stripParenthetical(const juce::String& raw);
    static bool namesMatchLoose(const juce::String& incoming,
                                const juce::String& entryName);

    // Resolve an incoming chain-entry name against the loadable entries.
    // matchLogOut (optional) receives the match path taken, or the closest
    // candidates on failure — for build-time diagnostics.
    juce::PluginDescription resolveByName(const juce::String& rawName,
                                          const juce::String& formatFilter,
                                          juce::String* matchLogOut = nullptr) const;

    // Full-entries cache (chain_entries.xml): written after every scan so
    // the OTHER host resolves against the same list without scanning.
    // maybeReloadEntriesCache() re-loads when another host refreshed it.
    static juce::File getEntriesCacheFile();
    void maybeReloadEntriesCache();

    // ---- Popout-only plugins (both hosts) ----------------------------------
    // Plugins whose editors can only render in a floating window: out-of-
    // process views (WaveShell etc.) attach as an AUv2ContainerView proxy
    // that never negotiates a real size inside a clipped container, but works
    // in its own NSWindow. Once a plugin times out inline it is remembered in
    // ~/Library/EchoJay/popout_only.txt (normalised name + format keyed,
    // mtime-reloaded so both hosts see marks immediately) and future
    // selections go straight to the pop-out with no failed inline attempt.
    // FORMAT-QUALIFIED because the same plugin's VST3 build is in-process
    // and may contain fine when the AU cannot.
    static bool isPopoutOnly(const juce::String& pluginName, const juce::String& format);
    static void markPopoutOnly(const juce::String& pluginName, const juce::String& format);

    // ---- Host session identity ---------------------------------------------
    // The user-facing HOST process this plugin instance ultimately belongs
    // to. Inside an out-of-process AU host (AUHostingServiceXPC) this is the
    // DAW itself (Logic Pro), resolved via the responsible-pid API with a
    // parent-walk backstop; for in-process hosts it is simply our own
    // process. startSec/startUsec come from PROC_PIDTBSDINFO so a recycled
    // pid can never false-match. `degraded` means resolution stopped at a
    // helper process: sharing is limited to that process, but stale values
    // from a previous session still cannot be adopted.
    struct HostIdentity
    {
        int          pid       = 0;
        juce::int64  startSec  = 0;
        juce::int64  startUsec = 0;
        juce::String name;
        bool         degraded  = false;
    };
    static const HostIdentity& getHostIdentity();   // resolved once per process

    // ---- Session project name (both hosts) --------------------------------
    // Published to session_project.json whenever a user sets/edits the
    // project name in any instance; mtime-watched by all instances (same
    // pattern as popout_only.txt). Session-scoped FOR REAL: the file is
    // stamped with the publishing host's identity, and the getter returns
    // empty unless the stamp matches the current host — a value from a
    // previous DAW session is invisible, so fresh instances prompt instead
    // of silently adopting it. Precedence lives with the CALLERS: an
    // instance's own serialised name always wins over this value.
    static juce::String getSessionProjectName();
    static void publishSessionProjectName(const juce::String& name);

    // Session genre — separate session_genre.json (NOT folded into the
    // project file: separate mtimes keep the two follow-watchers
    // independent). Same publish/adopt/follow pattern AND the same host
    // identity stamp/match rule; adoption is keyed off the genre-answered
    // FLAG in the callers, never the value (genre has a non-empty default).
    static juce::String getSessionGenre();
    static void publishSessionGenre(const juce::String& genre);

    // If `d` is a popout-only AudioUnit and a VST3 build of the same plugin
    // can be found, return the VST3 desc instead (in-process editor —
    // containable by the existing machinery). Applies at NEW instantiation
    // only; restores keep their saved format. Logs the substitution.
    juce::PluginDescription preferInlineHostableDesc(const juce::PluginDescription& d);

    // VST3 build of `pluginName`: direct entry, previously deep-scanned
    // cache, or on-demand enumeration of WaveShell VST3 modules (a single
    // .vst3 containing many plugins; the thin scan records only the shell).
    // Variant-aware: AU "X (m)"/"(s)" matches VST3 "X Mono"/"X Stereo".
    juce::PluginDescription findVst3Alternative(const juce::String& pluginName);

    // ---- Additive accessors (used by EchoJay Link hosting; main plugin
    // behaviour unchanged) ------------------------------------------------
    juce::PluginDescription getSlotDescription(int i) const;
    juce::AudioProcessor*   getSlotProcessor(int i) const;

    // Sum of the non-bypassed hosted plugins' reported latencies (message
    // thread). Link mirrors this into setLatencySamples on every change.
    int getTotalLatencySamples() const;

    // Fired on the message thread at the end of every graph rebuild
    // (load / remove / move / bypass). Unset in the main plugin.
    std::function<void()> onChainChanged;

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
        juce::String                         settings;   // AI-suggested dial-in guidance
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

    // Removed slots' nodes — kept ALIVE (disconnected, not processed) because
    // some plugins leak UI timers that fire into their AudioUnit after editor
    // close; disposing the instance makes that a use-after-free crash.
    // Freed only when the ChainHost itself is destroyed.
    std::vector<juce::AudioProcessorGraph::Node::Ptr> graveyard_;

    bool   prepared_  = false;
    std::atomic<bool> hasActiveSlots_ { false };  // true when ≥1 non-bypassed slot exists

    // Resolver cache (message thread only — no mutex needed)
    std::vector<RecommendableEntry> recommendable_;
    int recommendableEnabledIn_ = 0;   // how many enabled scanner entries were fed in
    juce::String recommendableFormat_; // filter used for the last build (fallback resolves honour it)

    // Entries-cache staleness tracking (message thread)
    juce::Time entriesCacheTime_;

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
