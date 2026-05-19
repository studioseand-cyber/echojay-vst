#include "PluginScanner.h"
#include <algorithm>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>

PluginScanner::PluginScanner() {}

PluginScanner::~PluginScanner()
{
    // Tell any detached workers spawned by scanWithTimeout that this
    // scanner is going away. The shared `alive` flag is captured by
    // worker lambdas via a shared_ptr, so it outlives this scanner.
    // Workers check it before scanDirectory and inside the walk loop.
    alive->store(false);
    
    if (scanThread && scanThread->isThreadRunning())
    {
        scanThread->stopThread(5000);
    }
    
    // Brief grace period so any detached worker that returned from its
    // stuck syscall in the last few moments has a chance to notice the
    // alive flag and unwind before we drop member memory. Not a hard
    // guarantee — a worker mid-syscall when this runs may still race —
    // but in practice the only worker types we detach are ones stuck
    // for tens of seconds on cloud paths, and the chance of one
    // returning in the exact window this hits is vanishingly small.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void PluginScanner::startScan()
{
    if (scanning.load()) return;
    
    scanning.store(true);
    progress.store(0.0f);
    
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        plugins.clear();
        // Invalidate the cached shuffled names — next call to
        // getPluginNamesString() will reshuffle. We do this explicitly (not
        // just relying on size mismatch) to handle the edge case where a
        // rescan ends up with the same plugin count but different plugins.
        cachedShuffledNames = juce::String();
        cachedShuffleSize = 0;
    }
    
    scanThread = std::make_unique<ScanThread>(*this);
    scanThread->startThread();
}

// ============================================================================
// Cloud/network folder detection
// ============================================================================
// macOS iCloud Drive, Dropbox, OneDrive, etc. use "placeholder" files whose
// metadata exists but reading them triggers a synchronous network fetch
// from the OS. A scan walking through one of these can stall for minutes
// or hang indefinitely on a stat() call. We refuse to enter these folders
// at all and log the skip — better than a hung "Scanning..." button.
//
// The heuristic is intentionally pattern-based rather than checking
// filesystem attributes: it's much faster (no syscalls) and covers the
// real-world cases we've seen. Users with a non-cloud "iCloud" folder name
// somewhere weird will lose plugins in that one spot, which is a fair
// trade for not hanging the scan.
bool PluginScanner::isLikelyCloudOrNetworkFolder(const juce::File& f)
{
    auto path = f.getFullPathName();
    
    // macOS iCloud Drive lives under ~/Library/Mobile Documents/ — the
    // user-visible "iCloud Drive" is just a symlinked alias. Modern macOS
    // also exposes "CloudStorage" under ~/Library/CloudStorage/ for
    // Dropbox/OneDrive/Google Drive Finder integration.
    if (path.contains("/Mobile Documents/"))      return true;
    if (path.contains("/Library/CloudStorage/"))  return true;
    if (path.containsIgnoreCase("/iCloud Drive/"))return true;
    
    // Dropbox / OneDrive / Google Drive — these usually live in the user
    // home but the folder name is the canonical signal.
    if (path.containsIgnoreCase("/Dropbox/"))     return true;
    if (path.containsIgnoreCase("/OneDrive"))     return true; // matches OneDrive and OneDrive - Company
    if (path.containsIgnoreCase("/Google Drive")) return true;
    if (path.containsIgnoreCase("/pCloud Drive")) return true;
    if (path.containsIgnoreCase("/Box Sync/"))    return true;
    if (path.containsIgnoreCase("/Box/"))         return true;
    
    // Time Machine local snapshots can also stall.
    if (path.startsWith("/Volumes/com.apple.TimeMachine")) return true;
    if (path.contains(".timemachine"))            return true;
    
    return false;
}

// ============================================================================
// Timeout-bounded directory scan
// ============================================================================
// Wraps scanDirectory in a future + deadline. If the worker doesn't finish
// within kPerFolderTimeoutMs we flip the cancel flag and detach the future —
// scanDirectory checks the flag between entries and bails on its next
// iteration. A stuck stat() call can't be cancelled mid-syscall, so the
// thread may keep running for a bit after we move on; that's safe because
// addPlugin is mutex-protected. Wasted work, but no deadlock.

namespace {
    static juce::String describeFolderName(const juce::File& f)
    {
        // For logging: the leaf name unless it's empty, in which case full path.
        auto leaf = f.getFileName();
        return leaf.isNotEmpty() ? leaf : f.getFullPathName();
    }
}

static void scanWithTimeout(PluginScanner& scanner,
                            const juce::File& dir,
                            const juce::String& format,
                            int timeoutMs,
                            std::function<void(const juce::File&, const juce::String&,
                                               const std::atomic<bool>&)> fn)
{
    // Shared state between the wrapper and the worker. cancelled lets us
    // ask the worker to bail on its next iteration; done lets us wait
    // efficiently rather than spinning. shared_ptr keeps all three alive
    // until both sides have let go, which is essential because we detach
    // the worker on the timeout path.
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto done      = std::make_shared<std::atomic<bool>>(false);
    
    // Launch the worker on a plain detached thread. We can't use
    // std::async with launch::async here — its returned future has a
    // blocking destructor, defeating the whole point of a timeout.
    std::thread worker([cancelled, done, dir, format, fn]() {
        fn(dir, format, *cancelled);
        done->store(true);
    });
    
    // Poll the done flag with short sleeps until either it flips or we
    // hit the deadline. Polling is fine here because timeoutMs is in the
    // tens-of-seconds range, not microseconds, and the cost is one
    // 10ms sleep per tick.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (! done->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    if (done->load())
    {
        worker.join();
    }
    else
    {
        cancelled->store(true);
        DBG("[PluginScanner] Timed out scanning " << describeFolderName(dir)
            << " for " << format << " (>" << timeoutMs << "ms) — moving on");
        // Detach. The worker will exit on its next loop iteration (the
        // cancelled flag is checked at every directory entry). If it's
        // stuck in a stat() syscall — typical for cloud-backed folders
        // that aren't on our blacklist — it will continue running until
        // the OS finally returns, then exit cleanly. Subsequent calls to
        // addPlugin() inside the doomed worker are still safe because
        // addPlugin grabs pluginMutex.
        worker.detach();
    }
    
    juce::ignoreUnused(scanner);
}


// Per-folder timeout: how long any single scanDirectory call may run before
// we flip its cancel flag and move on. Plugin folders normally complete in
// well under a second even with hundreds of bundles; 30s is generous
// headroom for slow disks while still guaranteeing the overall scan never
// hangs more than a few minutes total no matter how many bad folders the
// user has added.
static constexpr int kPerFolderTimeoutMs = 30000;

void PluginScanner::scanPluginDirectories()
{
    // Helper: scan one folder for one format, with timeout + (optional)
    // cloud check. The cloud check is on by default for platform defaults —
    // a stalled stat() in /Library/Audio/Plug-Ins/ is always a bug, never
    // a user intent — but off for user-added folders, where the user has
    // explicitly chosen the location and may legitimately be storing
    // plugins in a Dropbox/iCloud share across machines. The 30s timeout
    // still applies in both cases, so even a cloud-backed user folder
    // won't hang the scan forever.
    auto safeScan = [this](const juce::File& dir, const juce::String& format,
                            bool skipCloud) {
        if (! dir.isDirectory()) return;
        if (skipCloud && isLikelyCloudOrNetworkFolder(dir))
        {
            DBG("[PluginScanner] Skipping cloud/network folder: "
                << dir.getFullPathName());
            return;
        }
        scanWithTimeout(*this, dir, format, kPerFolderTimeoutMs,
            [this, aliveCopy = alive](const juce::File& d, const juce::String& fmt,
                                       const std::atomic<bool>& cancelled) {
                // aliveCopy is a shared_ptr<atomic<bool>> captured by value,
                // so the atomic outlives this scanner. If the scanner has
                // been destroyed (e.g. plugin unloaded while a detached
                // worker was stuck in a cloud syscall), bail before
                // touching `this`.
                if (! aliveCopy->load()) return;
                scanDirectory(d, fmt, cancelled);
            });
    };
    
    // ============ macOS ============
#if JUCE_MAC
    // VST3
    juce::Array<juce::File> vst3Dirs;
    vst3Dirs.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
    vst3Dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile("Library/Audio/Plug-Ins/VST3"));
    
    for (auto& dir : vst3Dirs)
        safeScan(dir, "VST3", true);
    
    progress.store(0.33f);
    
    // AU (Audio Units)
    juce::Array<juce::File> auDirs;
    auDirs.add(juce::File("/Library/Audio/Plug-Ins/Components"));
    auDirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                   .getChildFile("Library/Audio/Plug-Ins/Components"));
    
    for (auto& dir : auDirs)
        safeScan(dir, "AU", true);
    
    progress.store(0.66f);
    
    // Legacy VST (VST2)
    juce::Array<juce::File> vstDirs;
    vstDirs.add(juce::File("/Library/Audio/Plug-Ins/VST"));
    vstDirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                    .getChildFile("Library/Audio/Plug-Ins/VST"));
    
    for (auto& dir : vstDirs)
        safeScan(dir, "VST", true);
    
    // ============ Windows ============
#elif JUCE_WINDOWS
    // VST3
    juce::Array<juce::File> vst3Dirs;
    vst3Dirs.add(juce::File("C:\\Program Files\\Common Files\\VST3"));
    vst3Dirs.add(juce::File("C:\\Program Files (x86)\\Common Files\\VST3"));
    
    for (auto& dir : vst3Dirs)
        safeScan(dir, "VST3", true);
    
    progress.store(0.33f);
    
    // VST2
    juce::Array<juce::File> vstDirs;
    vstDirs.add(juce::File("C:\\Program Files\\VSTPlugins"));
    vstDirs.add(juce::File("C:\\Program Files\\Steinberg\\VSTPlugins"));
    vstDirs.add(juce::File("C:\\Program Files (x86)\\VSTPlugins"));
    vstDirs.add(juce::File("C:\\Program Files (x86)\\Steinberg\\VSTPlugins"));
    
    for (auto& dir : vstDirs)
        safeScan(dir, "VST", true);
    
    // ============ Linux ============
#elif JUCE_LINUX
    juce::Array<juce::File> vst3Dirs;
    vst3Dirs.add(juce::File("/usr/lib/vst3"));
    vst3Dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile(".vst3"));
    
    for (auto& dir : vst3Dirs)
        safeScan(dir, "VST3", true);
#endif
    
    // ---- User custom folders (all platforms) ---------------------------
    // Snapshot the list under the mutex so we don't trip over the UI
    // thread editing it mid-scan. For each folder we try all three
    // formats — the scanner uses extension matching, so a folder
    // containing only AUs won't pick up phantom VST3 entries. Each call
    // goes through safeScan, which applies the cloud blacklist and
    // per-folder timeout.
    juce::StringArray customSnapshot;
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        customSnapshot = customFolders;
    }
    for (auto& path : customSnapshot)
    {
        juce::File dir(path);
        safeScan(dir, "VST3", false);
        safeScan(dir, "AU", false);
        safeScan(dir, "VST", false);
    }
    
    progress.store(1.0f);
    
    // Sort alphabetically
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        std::sort(plugins.begin(), plugins.end(),
                  [](const ScannedPlugin& a, const ScannedPlugin& b) {
                      return a.name.compareIgnoreCase(b.name) < 0;
                  });
    }
    
    // Cache to disk
    saveCache();
    
    scanning.store(false);
}

void PluginScanner::scanDirectory(const juce::File& dir, const juce::String& format,
                                    const std::atomic<bool>& cancelled)
{
    if (!dir.isDirectory()) return;
    if (cancelled.load()) return;
    
    // Determine file extensions to look for
    juce::String extension;
    if (format == "VST3")      extension = ".vst3";
    else if (format == "AU")   extension = ".component";
    else if (format == "VST")  extension = ".vst";
    
    // Manual recursive walk with a depth cap. We need this for two reasons:
    //
    // 1. User-added custom folders may point at huge trees (Documents, backup
    //    drives, network shares). Without a depth limit one bad pick can hang
    //    the scan thread for minutes.
    // 2. Once we find a plugin bundle (e.g. Foo.vst3/) we must NOT descend
    //    into it — the bundle's internal Contents/MacOS/... directories
    //    contain unrelated files and JUCE's recursive RangedDirectoryIterator
    //    would happily walk through them. The earlier implementation only
    //    accidentally avoided this because none of those inner paths match
    //    the *.vst3 / *.component glob, but it still wasted I/O.
    //
    // Depth 0 means "only files directly in dir", depth 6 covers reasonable
    // manufacturer/category nesting (e.g. /VST3/Native Instruments/Effects/
    // Komplete/.../Plugin.vst3) without running away.
    //
    // The cancelled flag is checked at each entry — callers wrap this in a
    // timeout future and flip the flag if the scan is taking too long. The
    // worker exits on its next iteration. (Currently-blocked stat() calls
    // can't be cancelled mid-syscall, but the cloud/network blacklist in
    // scanPluginDirectories tries to keep us out of those folders in the
    // first place.)
    constexpr int kMaxDepth = 6;
    
    std::function<void(const juce::File&, int)> walk =
        [&](const juce::File& currentDir, int depth)
    {
        if (depth > kMaxDepth) return;
        if (cancelled.load()) return;
        if (! alive->load()) return; // scanner destroyed; bail without touching members
        
        // List immediate children only — non-recursive.
        for (const auto& entry : juce::RangedDirectoryIterator(currentDir, false, "*",
                                                                  juce::File::findFilesAndDirectories))
        {
            if (cancelled.load()) return;
            if (! alive->load()) return;
            
            auto file = entry.getFile();
            auto leaf = file.getFileName();
            
            // Skip hidden entries (Spotlight, .DS_Store, dotfiles).
            if (leaf.startsWith(".")) continue;
            
            // Does this entry match the plugin extension? On macOS plugins
            // are folder bundles, on Windows VST3 can also be DLL files;
            // we accept either here and let the rest of the function
            // figure out if it's worth recording.
            if (leaf.endsWithIgnoreCase(extension))
            {
                // It's a plugin — record it and DO NOT recurse into it.
                juce::String name = file.getFileNameWithoutExtension();
                if (name.isEmpty()) continue;
                
                // Try to extract manufacturer from the parent dir name.
                // Common pattern: /Manufacturer/PluginName.vst3
                juce::String manufacturer = "Unknown";
                auto parent = file.getParentDirectory();
                if (parent != dir)
                {
                    manufacturer = parent.getFileName();
                    // If the parent is just the format folder, treat as unknown.
                    if (manufacturer.endsWithIgnoreCase("VST3") ||
                        manufacturer.endsWithIgnoreCase("Components") ||
                        manufacturer.endsWithIgnoreCase("VST"))
                    {
                        manufacturer = "Unknown";
                    }
                }
                
                // For VST3, try to read the moduleinfo.json if it exists —
                // most accurate source of name + vendor.
                if (format == "VST3")
                {
                    auto moduleInfo = file.getChildFile("Contents/moduleinfo.json");
                    if (moduleInfo.existsAsFile())
                    {
                        auto json = juce::JSON::parse(moduleInfo.loadFileAsString());
                        if (auto* obj = json.getDynamicObject())
                        {
                            if (obj->hasProperty("Name"))
                                name = obj->getProperty("Name").toString();
                            if (obj->hasProperty("Vendor"))
                                manufacturer = obj->getProperty("Vendor").toString();
                        }
                    }
                }
                
                // Category heuristic from common name patterns.
                juce::String category = "Effect";
                juce::String nameLower = name.toLowerCase();
                if (nameLower.contains("synth") || nameLower.contains("keys") ||
                    nameLower.contains("piano") || nameLower.contains("organ") ||
                    nameLower.contains("sampler") || nameLower.contains("drum machine"))
                {
                    category = "Instrument";
                }
                
                addPlugin(name, manufacturer, format, category, file.getFullPathName());
                continue; // do NOT descend into the bundle
            }
            
            // Plain subdirectory — recurse if we still have depth budget
            // and it's not itself a bundle of some other format we don't
            // care about right now (skip .vst3/.component/.vst/.app to
            // avoid wasted recursion through other plugin bundles or
            // application bundles).
            if (file.isDirectory()
                && ! leaf.endsWithIgnoreCase(".vst3")
                && ! leaf.endsWithIgnoreCase(".component")
                && ! leaf.endsWithIgnoreCase(".vst")
                && ! leaf.endsWithIgnoreCase(".app")
                && ! leaf.endsWithIgnoreCase(".framework"))
            {
                walk(file, depth + 1);
            }
        }
    };
    
    walk(dir, 0);
}

void PluginScanner::addPlugin(const juce::String& name, const juce::String& manufacturer,
                               const juce::String& format, const juce::String& category,
                               const juce::String& path)
{
    // Check for duplicates (same name + manufacturer = same plugin, different format)
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    for (auto& p : plugins)
    {
        if (p.name == name && p.manufacturer == manufacturer)
        {
            // Add format to existing entry
            if (!p.format.contains(format))
                p.format += "/" + format;
            return;
        }
    }
    
    ScannedPlugin plugin;
    plugin.name = name;
    plugin.manufacturer = manufacturer;
    plugin.format = format;
    plugin.category = category;
    plugin.path = path;
    plugin.uid = name.toLowerCase().replaceCharacter(' ', '_') + "_" + manufacturer.toLowerCase().replaceCharacter(' ', '_');
    
    plugins.push_back(plugin);
}

std::vector<ScannedPlugin> PluginScanner::getPlugins() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    return plugins;
}

int PluginScanner::getPluginCount() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    return static_cast<int>(plugins.size());
}

juce::String PluginScanner::getPluginsJSON() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    juce::String json = "[";
    for (size_t i = 0; i < plugins.size(); ++i)
    {
        auto& p = plugins[i];
        json += "{";
        json += "\"name\":\"" + p.name.replace("\"", "\\\"") + "\",";
        json += "\"manufacturer\":\"" + p.manufacturer.replace("\"", "\\\"") + "\",";
        json += "\"format\":\"" + p.format + "\",";
        json += "\"category\":\"" + p.category + "\"";
        json += "}";
        if (i < plugins.size() - 1) json += ",";
    }
    json += "]";
    
    return json;
}

juce::String PluginScanner::getPluginNamesString() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);

    // Build the list of effect plugin names from the current scan results.
    std::vector<juce::String> names;
    for (auto& p : plugins)
    {
        if (p.category == "Effect")  // Only list effects for mix feedback
            names.push_back(p.name + " (" + p.manufacturer + ")");
    }

    // Return the cached shuffle if it's still valid. The cache is invalidated
    // when the plugin count changes (e.g. user rescanned and added/removed
    // plugins), in which case we shuffle fresh.
    //
    // IMPORTANT: this is what keeps server-side prompt caching working. If
    // we shuffled on every call (the original behaviour), the system prompt
    // would differ on every request and Anthropic's cache could never hit.
    if (! cachedShuffledNames.isEmpty() && cachedShuffleSize == names.size())
        return cachedShuffledNames;

    // First call this session, or plugin list size has changed since the
    // last shuffle. Reshuffle and cache.
    //
    // LLMs have strong positional bias — the first few entries are most
    // likely to be referenced. Shuffling once at session-start gives the AI
    // a different starting position each session without churning the order
    // mid-conversation.
    juce::Random rng (juce::Time::currentTimeMillis());
    for (int i = (int) names.size() - 1; i > 0; --i)
    {
        int j = rng.nextInt (i + 1);
        if (j != i)
            std::swap (names[(size_t) i], names[(size_t) j]);
    }

    juce::StringArray arr;
    for (auto& n : names)
        arr.add (n);

    cachedShuffledNames = arr.joinIntoString (", ");
    cachedShuffleSize   = names.size();
    return cachedShuffledNames;
}

juce::File PluginScanner::getCacheFile()
{
    auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    
#if JUCE_MAC
    return appData.getChildFile("Application Support/EchoJay/plugin_cache.json");
#elif JUCE_WINDOWS
    return appData.getChildFile("EchoJay/plugin_cache.json");
#else
    return appData.getChildFile(".echojay/plugin_cache.json");
#endif
}

void PluginScanner::saveCache() const
{
    auto file = getCacheFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText(getPluginsJSON());
}

void PluginScanner::loadCache()
{
    auto file = getCacheFile();
    if (!file.existsAsFile()) return;
    
    auto json = juce::JSON::parse(file.loadFileAsString());
    if (auto* arr = json.getArray())
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        plugins.clear();
        
        for (auto& item : *arr)
        {
            if (auto* obj = item.getDynamicObject())
            {
                ScannedPlugin p;
                p.name = obj->getProperty("name").toString();
                p.manufacturer = obj->getProperty("manufacturer").toString();
                p.format = obj->getProperty("format").toString();
                p.category = obj->getProperty("category").toString();
                p.uid = p.name.toLowerCase().replaceCharacter(' ', '_') + "_" +
                         p.manufacturer.toLowerCase().replaceCharacter(' ', '_');
                plugins.push_back(p);
            }
        }
    }
}

// ============================================================================
// Custom user-added scan folders
// ============================================================================
// Stored as one absolute path per line in ~/Documents/EchoJay/plugin_scan_folders.txt.
// Kept in plain text rather than JSON for easy user inspection/editing.

juce::File PluginScanner::getCustomFoldersFile()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
               .getChildFile("EchoJay").getChildFile("plugin_scan_folders.txt");
}

juce::StringArray PluginScanner::getCustomFolders() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    return customFolders;
}

void PluginScanner::addCustomFolder(const juce::File& folder)
{
    if (! folder.isDirectory()) return;
    auto path = folder.getFullPathName();
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        for (auto& existing : customFolders)
            if (existing.equalsIgnoreCase(path))
                return; // already in the list
        customFolders.add(path);
    }
    saveCustomFolders();
}

void PluginScanner::removeCustomFolder(const juce::String& folderPath)
{
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        for (int i = customFolders.size() - 1; i >= 0; --i)
            if (customFolders[i].equalsIgnoreCase(folderPath))
                customFolders.remove(i);
    }
    saveCustomFolders();
}

void PluginScanner::loadCustomFolders()
{
    auto file = getCustomFoldersFile();
    if (! file.existsAsFile()) return;
    
    juce::StringArray loaded;
    loaded.addTokens(file.loadFileAsString(), "\n", "");
    loaded.removeEmptyStrings();
    loaded.trim();
    
    std::lock_guard<std::mutex> lock(pluginMutex);
    customFolders = loaded;
}

void PluginScanner::saveCustomFolders() const
{
    auto file = getCustomFoldersFile();
    file.getParentDirectory().createDirectory();
    juce::StringArray snapshot;
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        snapshot = customFolders;
    }
    file.replaceWithText(snapshot.joinIntoString("\n"));
}
