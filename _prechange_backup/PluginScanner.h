#pragma once
#include <JuceHeader.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>

struct ScannedPlugin {
    juce::String name;
    juce::String manufacturer;
    juce::String format;       // "VST3", "AU", "VST"
    juce::String category;     // "Effect", "Instrument", "Unknown"
    juce::String path;
    juce::String uid;
};

class PluginScanner
{
public:
    PluginScanner();
    ~PluginScanner();
    
    // Scan all plugin directories (runs on background thread)
    void startScan();
    
    // Check if scan is running
    bool isScanning() const { return scanning.load(); }
    
    // Get scan progress (0.0 - 1.0)
    float getProgress() const { return progress.load(); }
    
    // Get all scanned plugins (thread-safe)
    std::vector<ScannedPlugin> getPlugins() const;
    
    // Get plugin list as JSON for the WebView
    juce::String getPluginsJSON() const;
    
    // Get plugin names as comma-separated string (for AI prompt)
    juce::String getPluginNamesString() const;
    
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
    
    // Scan a directory for plugins of one format. The cancel flag is checked
    // between entries during the walk so callers can time-bound the scan and
    // tell a stuck folder to give up on its next iteration. The cancel flag
    // does NOT interrupt blocking stat() calls (e.g. cloud-backed paths) —
    // for those we rely on the cloud-folder blacklist + the surrounding
    // timeout to move on regardless.
    void scanDirectory(const juce::File& dir, const juce::String& format,
                       const std::atomic<bool>& cancelled);
    
    // Returns true for folders that are known to hang or stall a scan —
    // iCloud Drive, Dropbox, OneDrive, Time Machine snapshots, etc. We skip
    // them outright rather than risking a blocked thread.
    static bool isLikelyCloudOrNetworkFolder(const juce::File& f);
    
    void addPlugin(const juce::String& name, const juce::String& manufacturer,
                   const juce::String& format, const juce::String& category,
                   const juce::String& path);
    
    mutable std::mutex pluginMutex;
    std::vector<ScannedPlugin> plugins;

    // User-added scan folders (in addition to platform defaults). Stored
    // here as plain absolute paths, persisted in a text file alongside
    // other EchoJay user data. The mutex above guards reads/writes so
    // both the background scanner thread and the UI can touch it safely.
    juce::StringArray customFolders;

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
