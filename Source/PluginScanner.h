#pragma once
#include <JuceHeader.h>
#include <vector>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <atomic>
#include <memory>

struct ScannedPlugin {
    juce::String name;
    juce::String manufacturer;
    juce::String format;       // "VST3", "AU", "VST"
    juce::String category;     // "Effect", "Instrument", "Unknown"
    juce::String path;
    juce::String uid;
    bool enabled = true;       // DERIVED AT READ (13 Aug 2026), never stored:
                               // stamped from disabledUids by stampEnabled in
                               // getPlugins()/serialization. It was a second
                               // copy of the tick state, and the two stores
                               // disagreed live (483 uids on disk, 418 in the
                               // mirrored flags, resolver reading the stale
                               // side). disabledUids is the ONE authority.
    juce::String fxType;       // Processing type tag for effects ("EQ",
                               // "Dynamics", "Reverb", ...). Classified once at
                               // add time (echojay::classifyEffect). Used to cap
                               // the AI feed per-type for a good spread. Empty
                               // for instruments.
};

class PluginScanner
{
public:
    PluginScanner();
    ~PluginScanner();
    
    // Scan all plugin directories (runs on background thread)
    void startScan();

    // Signal any in-flight scan/load work to abort as soon as possible. Flips
    // the shared `alive` flag that loadCache/scan loops check between steps.
    // Safe to call from another thread (e.g. the processor destructor) before
    // joining a background load thread, so the worker stops touching members.
    void requestStop() { alive->store(false); }
    
    // Check if scan is running
    bool isScanning() const { return scanning.load(); }
    
    // Get scan progress (0.0 - 1.0)
    float getProgress() const { return progress.load(); }
    
    // Get all scanned plugins (thread-safe)
    std::vector<ScannedPlugin> getPlugins() const;
    
    // Get plugin list as JSON for the WebView
    juce::String getPluginsJSON() const;
    
    // Get plugin names as comma-separated string (for AI prompt). This is the
    // BALANCED, capped list (a spread across processing types). Used as the
    // per-turn plugin injection when a message needs plugins but we don't want
    // to send the entire library.
    juce::String getPluginNamesString() const;

    // ---- Plugin feed: summary + full list ------------------------------
    // The AI gets plugins in two ways. A tiny always-present SUMMARY goes in
    // the (cached) system prompt so the AI always knows the SHAPE of the
    // user's library (top manufacturers + counts) without bloating every
    // message. The FULL enabled list is injected into the user turn ONLY when
    // a message actually needs plugins (asks about a chain, a processing type,
    // "what should I use"), so the AI can see everything the user owns exactly
    // when it matters, with no per-message token cost the rest of the time.

    // Short summary line, e.g. "~1,840 plugins incl. Waves (62), FabFilter (9),
    // UAD (140), Soundtoys (11), Logic stock". Always cheap; safe for the
    // cached system prompt.
    juce::String getPluginSummary() const;

    // The COMPLETE enabled effect list (no cap), comma-separated "Name (Manu)".
    // For per-turn injection on plugin-relevant messages.
    juce::String getFullPluginList() const;

    // Count of enabled effects (for the summary and for callers deciding
    // whether the full list is large enough to bother type-scoping).
    int getEnabledEffectCount() const;

    // ---- Enabled / disabled state -------------------------------------
    // The post-scan review list and the Settings checklist let the user
    // untick plugins they don't actually own (the main case: Waves plugins
    // listed from a WaveShell that aren't licensed) or simply don't want the
    // AI to suggest. Unticked plugins stay in the list but are excluded from
    // getPluginNamesString() (the AI feed). Disabled state is keyed by uid
    // and persisted separately from the plugin cache, so it survives a
    // rescan: re-detecting a plugin the user previously unticked keeps it
    // unticked.
    void setPluginEnabled(const juce::String& uid, bool enabled);
    void setManyEnabled(const juce::StringArray& uids, bool enabled);
    bool isPluginEnabled(const juce::String& uid) const;

    // Manually add a user-entered plugin not found by the scan (the
    // "Add custom" button in the review/Settings list). Enabled by default.
    void addManualPlugin(const juce::String& name);

    // Set the DAW detected automatically from the plugin host (via
    // juce::PluginHostType in the processor) — the DAW the user is running
    // EchoJay in right now. Drives stock plugin injection on the next scan.
    // Pass a Settings-style label ("Logic Pro", "Ableton Live", "FL Studio",
    // "Pro Tools", "Studio One", "Cubase") or empty if the host couldn't be
    // identified (out-of-process / sandboxed hosts can report Unknown), in
    // which case no stock plugins are injected. Thread-safe. Public: called
    // from the processor.
    void setDetectedDaw(const juce::String& dawLabel);

    // Persist / load the set of disabled uids.
    void saveEnabledState() const;
    void loadEnabledState();
    static juce::File getEnabledStateFile();
    // Cross-instance freshness (13 Aug 2026): several plugin instances share
    // one process but hold separate PluginScanner objects, and an untick in
    // instance A only wrote A's memory and the file - B's resolver kept
    // reading B's stale state until restart. Re-reads plugin_disabled.json
    // when its mtime moves; returns true when the set changed so the caller
    // can invalidate whatever it derived from it (the recommendable feed).
    bool maybeReloadEnabledState();
    // The ONE place ScannedPlugin::enabled is ever assigned: stamps the flag
    // from the authority set. Static, pure and HEADER-INLINE so mapfps_test
    // compiles the shipped implementation directly (the gate links the
    // previous build's lib, which cannot carry a symbol added in the same
    // commit) and pins the change-set-then-restamp contract without
    // touching user files.
    static void stampEnabled(std::vector<ScannedPlugin>& list,
                             const std::set<juce::String>& disabled)
    {
        for (auto& p : list)
            p.enabled = (disabled.find(p.uid) == disabled.end());
    }
    
    // Get count
    int getPluginCount() const;
    
    // Load cached scan results from disk
    void loadCache();
    
    // Save scan results to disk
    void saveCache() const;
    
    // Get cache file path
    static juce::File getCacheFile();

    // ---- Custom user-added plugin folders ------------------------------
    // Users can extend the default scan locations with arbitrary folders
    // (e.g. shared network drives, alternative VST3 paths from DAWs that
    // use non-standard locations). Folders are persisted across sessions.
    juce::StringArray getCustomFolders() const;
    void addCustomFolder(const juce::File& folder);
    void removeCustomFolder(const juce::String& folderPath);
    void loadCustomFolders();
    void saveCustomFolders() const;
    static juce::File getCustomFoldersFile();

private:
    void scanPluginDirectories();

    // ---- WaveShell expansion -------------------------------------------
    // A WaveShell bundle (e.g. "WaveShell1-VST3 14.0.vst3") hosts every
    // installed Waves plugin behind one file. The bundle name reveals only
    // the Waves version, never its contents, so a raw scan records a single
    // meaningless "WaveShell..." entry. Returns true if the given plugin
    // bundle filename is a WaveShell. When one is seen during the scan we
    // suppress the raw entry and instead expand the curated Waves catalog
    // (see expandWavesCatalog) so the AI gets real, nameable plugins.
    static bool isWaveShell(const juce::String& bundleFileName);

    // Inject the curated Waves catalog into the plugin list. Called once if
    // any WaveShell was detected during the scan. Idempotent via addPlugin's
    // dedupe. Guarded by the alive flag like every other member-touching call.
    void expandWavesCatalog();

    // ---- Stock DAW plugin injection ------------------------------------
    // Stock plugins ship inside the DAW app bundle / private dirs, never in
    // the shared VST3/AU/VST folders the scan walks. We inject the stock
    // catalogue for the DAW auto-detected from the plugin host (see
    // setDetectedDaw). Detection is the SOLE source — the Settings DAW
    // checkboxes do not add stock plugins — so the stock list always reflects
    // the DAW the user is actually running EchoJay in, with no manual step.
    void injectStockDawPlugins();

    // Add a flag the scan sets when a WaveShell was encountered, so the
    // expansion runs exactly once after the directory walk completes.
    bool sawWaveShell = false;
    
    // Scan a directory for plugins of one format. The cancel flag is checked
    // between entries during the walk so callers can time-bound the scan and
    // tell a stuck folder to give up on its next iteration. The cancel flag
    // does NOT interrupt blocking stat() calls (e.g. cloud-backed paths) —
    // for those we rely on the cloud-folder blacklist + the surrounding
    // timeout to move on regardless.
    void scanDirectory(const juce::File& dir, const juce::String& format,
                       const std::atomic<bool>& cancelled);

    // Enumerates AU plugins directly from the macOS AudioComponent registry
    // (no-op off macOS) and adds them with their real manufacturer + name, the
    // way a host does. This replaces matching filesystem .component files back
    // to the registry, which was failing for vendors whose bundle filename did
    // not match the registry plugin name. Filtered to real plugin component
    // types (effects, music effects, instruments, generators) so codecs and
    // format converters are skipped.
    void scanAudioUnitsFromRegistry();
    
    // Returns true for folders that are known to hang or stall a scan —
    // iCloud Drive, Dropbox, OneDrive, Time Machine snapshots, etc. We skip
    // them outright rather than risking a blocked thread.
    static bool isLikelyCloudOrNetworkFolder(const juce::File& f);
    
    void addPlugin(const juce::String& name, const juce::String& manufacturer,
                   const juce::String& format, const juce::String& category,
                   const juce::String& path);
    
    mutable std::mutex pluginMutex;
    std::vector<ScannedPlugin> plugins;

    // Fast dedupe index: maps a plugin's "name|manufacturer" key to its index
    // in the plugins vector. Without this, addPlugin() does a linear scan of
    // the whole vector on every insert — O(n^2) over a scan, which at 1000s
    // of plugins (e.g. a full Waves + Komplete + UAD + stock install) means
    // millions of string comparisons and a multi-second stall on the scan
    // thread. The map makes dedupe O(1) amortised. Rebuilt by rebuildIndex()
    // whenever the vector is repopulated wholesale (loadCache, clear on
    // startScan). Guarded by pluginMutex like the vector itself.
    std::unordered_map<std::string, size_t> pluginIndex;
    // Secondary index: normalised plugin NAME -> index of the canonical entry
    // for that name. Lets addPlugin absorb an "Unknown" copy of a plugin into
    // the same-named entry that already has a real manufacturer (and vice
    // versa), which collapses the cross-format doubles (e.g. the VST3 known as
    // "Plugin Alliance" and its AU twin that came through as "Unknown").
    std::unordered_map<std::string, size_t> nameIndex;
    void rebuildIndex();                       // call under lock after bulk vector changes
    static std::string makeKey(const juce::String& name,
                               const juce::String& manufacturer);

    // User-added scan folders (in addition to platform defaults). Stored
    // here as plain absolute paths, persisted in a text file alongside
    // other EchoJay user data. The mutex above guards reads/writes so
    // both the background scanner thread and the UI can touch it safely.
    juce::StringArray customFolders;

    // DAW auto-detected from the plugin host this session (Settings-style
    // label, or empty if unidentified). This is the SOLE source for stock
    // plugin injection — the Settings DAW checkboxes do not add stock plugins.
    // Guarded by pluginMutex.
    juce::String detectedDaw;

    // Set of plugin uids the user has explicitly DISABLED (unticked). Kept
    // separate from the plugins vector so it survives a rescan and so we can
    // apply it to freshly scanned/injected plugins. A uid in this set means
    // "user unticked it"; absence means enabled (the default). Persisted to
    // disk via saveEnabledState/loadEnabledState. Guarded by pluginMutex.
    std::set<juce::String> disabledUids;
    juce::Time enabledStateMtime_;   // maybeReloadEnabledState's guard
    bool enabledWatchLogged_ = false;   // diagnosis: one watch-active line per lifetime

    // Cached shuffled plugin list — populated lazily on first call to
    // getPluginNamesString() and reused for the lifetime of this scanner
    // (i.e. until the plugin instance is unloaded). Keeps the order stable
    // within a session so server-side prompt caching works. Cache is
    // invalidated when the plugin count changes (e.g. after a rescan).
    mutable juce::String cachedShuffledNames;
    mutable size_t       cachedShuffleSize = 0;

    std::atomic<bool> scanning { false };
    std::atomic<float> progress { 0.0f };
    std::unique_ptr<juce::Thread> scanThread;
    
    // Liveness token. Detached worker threads (spawned by scanWithTimeout
    // to enforce per-folder timeouts) outlive the scope of the original
    // scan call when they get stuck on a cloud-backed stat(). If the
    // PluginScanner is destroyed before such a worker finishes, the worker
    // must not touch any members. We share this atomic via shared_ptr so
    // the worker can check it independently of the scanner's lifetime;
    // the destructor flips it to false. addPlugin and scanDirectory both
    // check it before doing any member access.
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>>(true) };
    
    // Background thread for scanning
    class ScanThread : public juce::Thread
    {
    public:
        ScanThread(PluginScanner& owner) : Thread("EchoJay Plugin Scanner"), scanner(owner) {}
        void run() override { scanner.scanPluginDirectories(); }
    private:
        PluginScanner& scanner;
    };
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginScanner)
};
