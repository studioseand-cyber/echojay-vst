// System audio framework headers MUST be included BEFORE the JUCE headers.
// Including them afterwards pulls the system 'AudioBuffer' struct into a TU
// where juce::AudioBuffer is already visible unqualified, which makes the SDK
// header itself fail to parse ("reference to 'AudioBuffer' is ambiguous").
// Guarded with __APPLE__ (not JUCE_MAC) because JUCE_MAC isn't defined yet here.
#if defined(__APPLE__)
 #include <AudioToolbox/AudioToolbox.h>
 #include <AudioUnit/AudioUnit.h>
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include "PluginScanner.h"
#include "EJStateRoot.h"   // 6 Sep 2026: every user-state path resolves through the isolatable root
#include "PluginCatalog.h"

// Unified-log line (implemented in the ObjC side; see NativeClip.h).
extern "C" void EchoJay_NSLog(const char* msg);
#include <algorithm>
#include <functional>
#include <map>
#include <atomic>
#include <thread>
#include <chrono>

// Normalised plugin name (lowercase, alphanumerics only) used as the key for
// the name index, so "Pro-Q 3" and "ProQ3" collapse to the same name bucket.
static std::string normaliseForMatch(const juce::String& s)
{
    auto lower = s.toLowerCase();
    std::string out;
    for (int i = 0; i < lower.length(); ++i)
    {
        auto c = lower[i];
        if (juce::CharacterFunctions::isLetterOrDigit(c))
            out.push_back((char) c);
    }
    return out;
}

#if JUCE_MAC
void PluginScanner::scanAudioUnitsFromRegistry()
{
    // Walk every registered AudioComponent and add the real plugins directly.
    // AudioComponentCopyName returns "Manufacturer: Plugin", the same string a
    // host shows. We never instantiate anything, so this is fast and cannot
    // hang on a bad plugin. Filtered to real plugin types so the system codecs
    // / format converters (the long "no Manufacturer:" list) are skipped.
    AudioComponentDescription desc {}; // all-zero = wildcard (every component)
    AudioComponent comp = nullptr;

    while ((comp = AudioComponentFindNext(comp, &desc)) != nullptr)
    {
        if (! alive->load()) return;

        AudioComponentDescription got {};
        if (AudioComponentGetDescription(comp, &got) != noErr)
            continue;

        // Keep effects, music effects, instruments and generators; skip
        // converters/codecs/mixers/etc.
        if (got.componentType != kAudioUnitType_Effect
            && got.componentType != kAudioUnitType_MusicEffect
            && got.componentType != kAudioUnitType_MusicDevice
            && got.componentType != kAudioUnitType_Generator)
            continue;

        CFStringRef cfName = nullptr;
        if (AudioComponentCopyName(comp, &cfName) != noErr || cfName == nullptr)
            continue;

        auto full = juce::String::fromCFString(cfName);
        CFRelease(cfName);

        // "Manufacturer: Plugin"
        auto colon = full.indexOf(": ");
        juce::String manu, name;
        if (colon > 0)
        {
            manu = full.substring(0, colon).trim();
            name = full.substring(colon + 2).trim();
        }
        else
        {
            manu = "Unknown";
            name = full.trim();
        }
        if (name.isEmpty())
            continue;

        // Waves registers every plugin as its own AU component (hundreds of
        // them), but Waves is already injected via the curated WaveShell
        // catalog after the VST3 walk. Skip them here so the group isn't
        // doubled.
        if (manu.trim().equalsIgnoreCase("Waves"))
            continue;

        const bool isInstrument = (got.componentType == kAudioUnitType_MusicDevice);
        addPlugin(name, manu, "AU", isInstrument ? "Instrument" : "Effect",
                  "AudioUnit:" + name);
    }
}
#else
void PluginScanner::scanAudioUnitsFromRegistry() {}
#endif

#if JUCE_MAC
// Reads a bundle's CFBundleIdentifier (e.g. "com.meldaproduction.MUtility")
// WITHOUT loading the plugin's executable: CFBundleCreate only parses the
// Info.plist (and transparently handles binary plists), it does not run code,
// so there's no hang/crash risk. Returns "" if the bundle has no identifier.
static juce::String bundleIdentifierFor(const juce::File& bundle)
{
    juce::String result;
    auto path = bundle.getFullPathName();

    CFStringRef cfPath = CFStringCreateWithCString(kCFAllocatorDefault,
                                                   path.toRawUTF8(),
                                                   kCFStringEncodingUTF8);
    if (cfPath != nullptr)
    {
        CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfPath,
                                                     kCFURLPOSIXPathStyle, true);
        if (url != nullptr)
        {
            if (CFBundleRef b = CFBundleCreate(kCFAllocatorDefault, url))
            {
                if (CFStringRef ident = CFBundleGetIdentifier(b)) // not owned
                    result = juce::String::fromCFString(ident);
                CFRelease(b);
            }
            CFRelease(url);
        }
        CFRelease(cfPath);
    }
    return result;
}
#endif

// Ctor and dtor are header-inline; see PluginScanner.h for why it is
// load-bearing (stale-lib layout vs inline methods).

void PluginScanner::startScan()
{
    if (scanning.load()) return;
    
    scanning.store(true);
    progress.store(0.0f);
    
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        plugins.clear();
        pluginIndex.clear();
        nameIndex.clear();
        // Invalidate the cached shuffled names — next call to
        // getPluginNamesString() will reshuffle. We do this explicitly (not
        // just relying on size mismatch) to handle the edge case where a
        // rescan ends up with the same plugin count but different plugins.
        cachedShuffledNames = juce::String();
        cachedShuffleSize = 0;
    }

    // Reset the WaveShell-seen flag for this run (set during the walk,
    // consumed afterwards to trigger catalog expansion).
    sawWaveShell = false;
    
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
    
    // AU (Audio Units): read straight from the AudioComponent registry instead
    // of globbing /Components and matching filenames back to it. The registry
    // gives the real "Manufacturer: Plugin" for every installed AU, which is
    // how Logic/Pro Tools list them, and it sidesteps the bundle-filename
    // mismatch that was leaving Melda et al mis-grouped.
    scanAudioUnitsFromRegistry();
    
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

    // ---- WaveShell expansion -------------------------------------------
    // If any WaveShell bundle was seen during the walk, replace the (now
    // suppressed) single shell entry with the curated Waves plugin catalog.
    if (sawWaveShell && alive->load())
        expandWavesCatalog();

    // ---- Stock DAW plugins ---------------------------------------------
    // Inject stock plugins for every DAW we can detect as installed. These
    // never appear in the folder scan, so this is the only path that makes
    // a producer's everyday stock tools visible to the AI.
    if (alive->load())
        injectStockDawPlugins();

    progress.store(1.0f);

    // Sort alphabetically
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        std::sort(plugins.begin(), plugins.end(),
                  [](const ScannedPlugin& a, const ScannedPlugin& b) {
                      return a.name.compareIgnoreCase(b.name) < 0;
                  });
        // The sort reordered the vector, so every index in pluginIndex is now
        // stale. Rebuild it before anyone (a follow-up rescan merge, etc.)
        // relies on it again.
        rebuildIndex();
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

                // WaveShell special case. Waves hosts every installed plugin
                // inside this one bundle; the filename ("WaveShell1-VST3 14.0",
                // "WaveShell2-AU 11.0", etc.) only tells us the version, not
                // the contents. Recording it verbatim gives the AI a useless
                // "WaveShell..." entry. Instead we flag that a shell was seen
                // and skip the raw entry; expandWavesCatalog() runs once after
                // the walk and injects the curated Waves plugin names. The
                // flag write is racy across the multiple scanned format dirs
                // but it's a monotonic set-to-true, so no lock needed.
                if (isWaveShell(leaf))
                {
                    sawWaveShell = true;
                    continue; // suppress raw shell; do NOT descend
                }

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

                            // In the VST3 moduleinfo schema the vendor lives
                            // under "Factory Info" -> "Vendor", NOT at the top
                            // level (the old top-level read here always missed,
                            // which is why moduleinfo-only vendors fell through
                            // to "Unknown"). Fall back to a top-level "Vendor"
                            // and then the first class's "Vendor".
                            juce::String vendor;
                            auto fi = obj->getProperty("Factory Info");
                            if (auto* fiObj = fi.getDynamicObject())
                                if (fiObj->hasProperty("Vendor"))
                                    vendor = fiObj->getProperty("Vendor").toString();
                            if (vendor.isEmpty() && obj->hasProperty("Vendor"))
                                vendor = obj->getProperty("Vendor").toString();
                            if (vendor.isEmpty())
                            {
                                auto classes = obj->getProperty("Classes");
                                if (auto* arr = classes.getArray())
                                    for (auto& cls : *arr)
                                        if (auto* cObj = cls.getDynamicObject())
                                            if (cObj->hasProperty("Vendor"))
                                            {
                                                auto v = cObj->getProperty("Vendor").toString();
                                                if (v.isNotEmpty()) { vendor = v; break; }
                                            }
                            }
                            if (vendor.isNotEmpty())
                                manufacturer = vendor;
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
                
                // Correct manufacturer for vendors that organise plugins into
                // category subfolders (UAD -> "Guitar and Bass" etc.) or whose
                // brand is in the plugin name. resolveManufacturer returns a
                // better value, or empty to keep what we have.
                {
                    auto corrected = echojay::resolveManufacturer(name, manufacturer);
                    if (corrected.isNotEmpty())
                        manufacturer = corrected;
                }

                // A "manufacturer" identical to the plugin name is never a real
                // vendor: it's a per-plugin folder or a self-named vendor field
                // (Melda's VST3 layout does this, giving a hundred junk
                // single-plugin groups). Drop it to Unknown so the AU registry's
                // real vendor claims it via the name-based merge in addPlugin.
                if (normaliseForMatch(manufacturer) == normaliseForMatch(name))
                    manufacturer = "Unknown";

                // Last resort: plugins with no vendor from the folder, no VST3
                // moduleinfo vendor, and no AU twin can still be attributed from
                // their bundle identifier (reverse-DNS like com.vendor.product),
                // read without loading the plugin. macOS bundles only.
              #if JUCE_MAC
                if (manufacturer == "Unknown"
                    && (format == "VST3" || format == "VST"))
                {
                    auto bid = bundleIdentifierFor(file);
                    if (bid.isNotEmpty())
                    {
                        auto v = echojay::vendorFromBundleId(bid, name);
                        if (v.isNotEmpty())
                            manufacturer = v;
                    }
                }
              #endif

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

// ============================================================================
// WaveShell detection + expansion
// ============================================================================

bool PluginScanner::isWaveShell(const juce::String& bundleFileName)
{
    // Match the family of shell names across versions and formats:
    //   WaveShell1-VST3 14.0.vst3, WaveShell2-AU 11.0.component,
    //   WaveShell1-VST 9.x.vst, WaveShell-AAX ... etc.
    // The common, stable token is "waveshell" at the start of the leaf.
    return bundleFileName.startsWithIgnoreCase("WaveShell");
}

void PluginScanner::expandWavesCatalog()
{
    // Inject every curated Waves plugin under the "Waves" manufacturer.
    // Format is reported generically as the shell's host format set — we
    // don't know per-plugin which formats are present, so we tag "VST3/AU"
    // which is true for any modern Waves install. addPlugin dedupes by
    // name+manufacturer, so re-running a scan won't double up, and a real
    // scanned Waves entry (if one ever appears via a custom folder) merges
    // cleanly.
    for (const auto& e : echojay::wavesCatalog())
    {
        if (! alive->load()) return;
        addPlugin(juce::String(e.name), "Waves",
                  "VST3/AU", juce::String(e.category),
                  /*path*/ "WaveShell");
    }

    DBG("[PluginScanner] Expanded WaveShell into "
        << (int) echojay::wavesCatalog().size() << " Waves plugins");
}

// ============================================================================
// Stock DAW plugin injection (settings-driven)
// ============================================================================
// We inject stock catalogues for the DAWs the user selected in Settings,
// not for whatever happens to be installed. This is both more accurate
// (a producer with five DAWs installed only mixes in one or two) and far
// simpler (no fragile app-bundle probing across platforms and install
// layouts). The DAW name strings here MUST match the Settings UI labels in
// PluginEditor exactly.

void PluginScanner::setDetectedDaw(const juce::String& dawLabel)
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    detectedDaw = dawLabel.trim();
}

void PluginScanner::injectStockDawPlugins()
{
    // Stock plugins are injected ONLY for the DAW auto-detected from the host
    // (juce::PluginHostType, set via setDetectedDaw from the processor). The
    // Settings DAW checkboxes do NOT add stock plugins — detection is the sole
    // source, so the stock list always reflects the DAW the user is actually
    // running EchoJay in, with no manual step. If the host couldn't be
    // identified (out-of-process / sandboxed host reporting Unknown), nothing
    // is injected rather than guessing.
    juce::String daw;
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        daw = detectedDaw;
    }
    if (daw.isEmpty()) return; // host not identified — inject no stock plugins

    // Helper to add a whole catalogue under one manufacturer/format tag.
    auto add = [this](const std::vector<echojay::CatalogEntry>& cat,
                       const juce::String& manufacturer,
                       const juce::String& format)
    {
        for (const auto& e : cat)
        {
            if (! alive->load()) return;
            // Stock plugins of different DAWs frequently share generic names
            // ("Compressor", "Limiter", "Gate", "Chorus"). The manufacturer
            // tag keeps them distinct in addPlugin's dedupe (which keys on
            // name+manufacturer).
            addPlugin(juce::String(e.name), manufacturer,
                      format, juce::String(e.category), /*path*/ "Stock");
        }
    };

    // Inject the detected DAW's stock catalogue. Only one matches.
    if (daw == "Logic Pro")
        add(echojay::logicStockCatalog(), "Logic Pro Stock", "AU");
    else if (daw == "Ableton Live")
        add(echojay::abletonStockCatalog(), "Ableton Stock", "Stock");
    else if (daw == "FL Studio")
        add(echojay::flStudioStockCatalog(), "FL Studio Stock", "Stock");
    else if (daw == "Pro Tools")
        add(echojay::proToolsStockCatalog(), "Pro Tools Stock", "AAX");
    else if (daw == "Studio One")
        add(echojay::studioOneStockCatalog(), "Studio One Stock", "Stock");
    else if (daw == "Cubase")
        add(echojay::cubaseStockCatalog(), "Cubase Stock", "VST3");
}

std::string PluginScanner::makeKey(const juce::String& name,
                                   const juce::String& manufacturer)
{
    // Case-INsensitive name|manufacturer. Combined with canonicaliseManufacturer
    // in addPlugin, this merges entries that differ only in casing (the old
    // case-sensitive key let "Decapitator|Soundtoys" and "Decapitator|SoundToys"
    // through as two plugins). The separator can't appear in either field.
    return (name.toLowerCase().trim() + "|" + manufacturer.toLowerCase().trim()).toStdString();
}

void PluginScanner::rebuildIndex()
{
    // Caller must hold pluginMutex. Rebuilds pluginIndex and nameIndex.
    pluginIndex.clear();
    nameIndex.clear();
    pluginIndex.reserve(plugins.size() * 2);
    nameIndex.reserve(plugins.size() * 2);
    for (size_t i = 0; i < plugins.size(); ++i)
    {
        pluginIndex[makeKey(plugins[i].name, plugins[i].manufacturer)] = i;
        auto nk = normaliseForMatch(plugins[i].name);
        auto it = nameIndex.find(nk);
        // Prefer an attributed row as the canonical one for its name.
        if (it == nameIndex.end()
            || (plugins[i].manufacturer != "Unknown"
                && plugins[it->second].manufacturer == "Unknown"))
            nameIndex[nk] = i;
    }
}

void PluginScanner::addPlugin(const juce::String& nameIn, const juce::String& manufacturerIn,
                               const juce::String& format, const juce::String& category,
                               const juce::String& path)
{
    // Dedupe in O(1) via the index rather than scanning the whole vector.
    // Same plugin (name + manufacturer) seen in another format just merges
    // the format string onto the existing entry.
    std::lock_guard<std::mutex> lock(pluginMutex);

    // Canonicalise the vendor at this single chokepoint so the stored label,
    // the dedupe key, and the uid all agree no matter which source (AU
    // registry, VST3 Vendor, folder name) produced it. This is what collapses
    // the "Soundtoys"/"SoundToys" and "UAD"/"Universal Audio" splits.
    const juce::String name = echojay::stripChannelVariant(nameIn);
    const juce::String manufacturer = echojay::canonicaliseManufacturer(manufacturerIn);

    auto key = makeKey(name, manufacturer);
    auto it = pluginIndex.find(key);
    if (it != pluginIndex.end())
    {
        auto& existing = plugins[it->second];
        if (! existing.format.contains(format))
            existing.format += "/" + format;
        return;
    }

    // No exact name|manufacturer match. Before adding a new row, reconcile
    // against any existing entry with the SAME name so the same plugin seen
    // across formats with one side "Unknown" collapses into one correctly
    // attributed row instead of a duplicate (the cross-format doubles).
    const std::string nkey = normaliseForMatch(name);
    auto nIt = nameIndex.find(nkey);
    if (nIt != nameIndex.end())
    {
        auto& existing = plugins[nIt->second];
        const bool existingKnown = existing.manufacturer != "Unknown";
        const bool newKnown      = manufacturer != "Unknown";

        if (existingKnown && ! newKnown)
        {
            // New row is an "Unknown" copy of an already-attributed plugin:
            // absorb its format and drop it.
            if (! existing.format.contains(format))
                existing.format += "/" + format;
            return;
        }
        if (! existingKnown && newKnown)
        {
            // Existing row was "Unknown"; promote it to the manufacturer we
            // just learned, re-keying the exact index to match.
            pluginIndex.erase(makeKey(existing.name, existing.manufacturer));
            existing.manufacturer = manufacturer;
            existing.uid = echojay::makeUid(existing.name, manufacturer);
            if (! existing.format.contains(format))
                existing.format += "/" + format;
            pluginIndex[makeKey(existing.name, existing.manufacturer)] = nIt->second;
            return;
        }
        // One vocabulary for EQUALITY too (13 Aug 2026, the 57 doubles):
        // the same product scanned per-format can arrive under different
        // vendor strings (bx_boom as Plugin Alliance/AU and Brainworx/VST3)
        // or different name formatting (Devil-Loc Deluxe vs
        // Devil-Loc_Deluxe), and raw-string comparison called those genuine
        // collisions and kept both rows. Product identity IS uid identity:
        // if makeUid agrees, it is one plugin, and the row carries the
        // format UNION exactly as the Unknown-absorption path always has.
        // The resolver's hosted-format filter reads CHAIN entries, not
        // these rows, so the union costs nothing there.
        if (echojay::makeUid(name, manufacturer) == existing.uid)
        {
            if (! existing.format.contains(format))
                existing.format += "/" + format;
            return;
        }
        // else: two real but different vendors share this name (genuine
        // collision) -> fall through and keep them as separate rows.
    }

    ScannedPlugin plugin;
    plugin.name = name;
    plugin.manufacturer = manufacturer;
    plugin.format = format;
    plugin.category = category;
    plugin.path = path;
    plugin.uid = echojay::makeUid(name, manufacturer);

    // Classify effect type for per-type capping of the AI feed. Instruments
    // don't go in the feed, so leave their fxType empty.
    if (category == "Effect")
        plugin.fxType = echojay::fxTypeTag(echojay::classifyEffect(name));

    // Respect a prior unticking: if the user disabled this uid before (and it
    // survived in disabledUids across the rescan), keep it disabled.
    // enabled is derived at read (stampEnabled); nothing to set here.

    const size_t newIdx = plugins.size();
    pluginIndex[key] = newIdx;
    // Point the name index at this row if it's the first with this name, or if
    // this row is attributed and the current canonical one is not.
    if (nIt == nameIndex.end()
        || (manufacturer != "Unknown" && plugins[nIt->second].manufacturer == "Unknown"))
        nameIndex[nkey] = newIdx;
    plugins.push_back(plugin);
}

std::vector<ScannedPlugin> PluginScanner::getPlugins() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    auto copy = plugins;
    stampEnabled(copy, disabledUids);
    return copy;
}

bool PluginScanner::maybeReloadEnabledState()
{
    // DIAGNOSIS INSTRUMENTATION (13 Aug 2026): the 12:28 unticks wrote 500
    // uids and no instance rebuilt, including the writer. Three questions,
    // one line each, bounded volume: is this called at all (once per
    // scanner lifetime), does it see the mtime move, and what verdict does
    // the set comparison reach. NOTE the standing hypothesis this must
    // confirm or kill: the WRITER's own set is already fresh when its save
    // moves the mtime, so the changed=n early-return below eats the
    // writer's unlatch by design of this very function.
    if (! enabledWatchLogged_)
    {
        enabledWatchLogged_ = true;
        EchoJay_NSLog(("EJScan: enabledState watch active (scanner 0x"
                       + juce::String::toHexString((juce::pointer_sized_int) this)
                       + ")").toRawUTF8());
    }
    auto file = getEnabledStateFile();
    if (! file.existsAsFile()) return false;
    const auto mtime = file.getLastModificationTime();
    if (mtime == enabledStateMtime_) return false;
    enabledStateMtime_ = mtime;
    auto json = juce::JSON::parse(file.loadFileAsString());
    if (auto* arr = json.getArray())
    {
        std::set<juce::String> fresh;
        for (auto& item : *arr)
            fresh.insert(item.toString());
        return applyReloadedDisabledSet(std::move(fresh));
    }
    return false;
}

// applyReloadedDisabledSet, notifyDisabledSetChanged and setPluginEnabled
// are HEADER-INLINE (PluginScanner.h): the gate's test must compile the
// shipped trigger behaviour, not link the previous build's copy.

// ============================================================================
// Enabled / disabled state
// ============================================================================

// setPluginEnabled is header-inline; see the note above.

void PluginScanner::setManyEnabled(const juce::StringArray& uids, bool enabled)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(pluginMutex);

        for (auto& uid : uids)
        {
            if (enabled) changed = (disabledUids.erase(uid) > 0) || changed;
            else         changed = disabledUids.insert(uid).second || changed;
        }

        if (changed)
        {
            cachedShuffledNames = juce::String();
            cachedShuffleSize = 0;
        }
    }
    saveEnabledState();
    // The setter IS the writing instance's trigger: its own save cannot
    // inform it through the file watch (a writer is not a reader).
    if (changed) notifyDisabledSetChanged("setter");
}

bool PluginScanner::isPluginEnabled(const juce::String& uid) const
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    return disabledUids.find(uid) == disabledUids.end();
}

void PluginScanner::addManualPlugin(const juce::String& name)
{
    auto trimmed = name.trim();
    if (trimmed.isEmpty()) return;

    // Manual entries are tagged manufacturer "Custom" so they're visually
    // distinct in the list and never collide with scanned/stock entries.
    addPlugin(trimmed, "Custom", "Manual", "Effect", "Manual");

    // addPlugin invalidates nothing about the shuffle cache itself, so do it
    // here to make the new entry visible to the AI feed immediately.
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        cachedShuffledNames = juce::String();
        cachedShuffleSize = 0;
    }
    saveCache();
}

juce::File PluginScanner::getEnabledStateFile()
{
    auto appData = echojay::userAppData();
#if JUCE_MAC
    return appData.getChildFile("Application Support/EchoJay/plugin_disabled.json");
#elif JUCE_WINDOWS
    return appData.getChildFile("EchoJay/plugin_disabled.json");
#else
    return appData.getChildFile(".echojay/plugin_disabled.json");
#endif
}

void PluginScanner::saveEnabledState() const
{
    juce::StringArray snapshot;
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        for (auto& uid : disabledUids)
            snapshot.add(uid);
    }

    // Store as a JSON array of disabled uids.
    juce::String json = "[";
    for (int i = 0; i < snapshot.size(); ++i)
    {
        json += "\"" + snapshot[i].replace("\"", "\\\"") + "\"";
        if (i < snapshot.size() - 1) json += ",";
    }
    json += "]";

    auto file = getEnabledStateFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText(json);
}

void PluginScanner::loadEnabledState()
{
    auto file = getEnabledStateFile();
    if (! file.existsAsFile()) return;
    enabledStateMtime_ = file.getLastModificationTime();

    auto json = juce::JSON::parse(file.loadFileAsString());
    MigrationCounts mig;
    if (auto* arr = json.getArray())
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        disabledUids.clear();
        for (auto& item : *arr)
            disabledUids.insert(item.toString());
        // No apply-loop any more: rows carry no tick state, the flag is
        // stamped from this set at every read (stampEnabled).
        // Vocabulary migration: runs after loadCache (the loadThread's
        // ordering) so legacyUidMap_ is populated. See migrateDisabledSet
        // for the chosen error direction.
        mig = migrateDisabledSet(disabledUids, legacyUidMap_);
    }
    if (mig.rewritten > 0 || mig.collapsed > 0)
    {
        saveEnabledState();
        // The first user whose exclusions shift after an update gets an
        // explanation in the log rather than a mystery.
        EchoJay_NSLog(("EJScan: disabled set migrated, "
                       + juce::String(mig.rewritten) + " entr(ies) rewritten, "
                       + juce::String(mig.collapsed) + " collapsed"
                       + " (uid vocabulary unification)").toRawUTF8());
    }
    else
    {
        // The zero case announces itself too (13 Aug 2026, evening): a
        // silent no-op is indistinguishable from correct-and-unreached,
        // and this week produced four candidates for that pattern. One
        // line per load, naming how many legacy candidates existed and
        // that none were in the set, ends the ambiguity.
        EchoJay_NSLog(("EJScan: disabled set migration checked, nothing to do ("
                       + juce::String((int) legacyUidMap_.size())
                       + " legacy candidate(s), none present in the set)").toRawUTF8());
    }
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
        json += "\"category\":\"" + p.category + "\",";
        json += "\"uid\":\"" + p.uid.replace("\"", "\\\"") + "\",";
        // Derived from the authority set at serialization; the WebView
        // checklist reads it, nothing ever reads it back (loadCache ignores
        // it since 13 Aug 2026).
        json += "\"enabled\":" + juce::String(
            disabledUids.find(p.uid) == disabledUids.end() ? "true" : "false");
        json += "}";
        if (i < plugins.size() - 1) json += ",";
    }
    json += "]";
    
    return json;
}

juce::String PluginScanner::getPluginNamesString() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);

    // ------------------------------------------------------------------
    // Build the AI plugin feed: ENABLED effects only, capped and balanced.
    // ------------------------------------------------------------------
    // The raw enabled set can be 200+ entries once WaveShell expansion and
    // stock-DAW injection are in play. Dumping all of them bloats every
    // prompt and biases the model toward whichever type happens to dominate
    // the list. Instead we select up to kMaxFeedPlugins via a per-type
    // round-robin that (a) guarantees a spread across processing types so the
    // AI always has an EQ, a comp, a reverb, etc. to reach for, and (b) within
    // each type prefers plugins the user actively installed (third-party) over
    // DAW stock, over Waves-from-shell (which may not even be licensed).
    //
    // "names" ends up holding the SELECTED subset; everything downstream
    // (shuffle + cache) is unchanged, so prompt caching still works.
    static constexpr size_t kMaxFeedPlugins = 60;

    // Source priority: lower = kept first. Stock manufacturers end in "Stock";
    // Waves came from the shell; "Custom" is a manual user entry (trust it
    // like third-party). Everything else is real scanned third-party.
    auto sourcePriority = [](const juce::String& manu) -> int
    {
        if (manu == "Waves")               return 3;
        if (manu.endsWithIgnoreCase("Stock")) return 2;
        if (manu == "Custom")              return 0;
        return 0; // third-party scanned
    };

    // Bucket enabled effects by fxType, each bucket sorted by source priority
    // then name for determinism.
    struct Cand { juce::String display; int prio; juce::String sortName; };
    std::map<juce::String, std::vector<Cand>> byType;
    for (auto& p : plugins)
    {
        if (p.category != "Effect" || disabledUids.count(p.uid) > 0) continue;
        juce::String type = p.fxType.isNotEmpty() ? p.fxType : juce::String("Other");
        byType[type].push_back({ p.name + " (" + p.manufacturer + ")",
                                 sourcePriority(p.manufacturer),
                                 p.name.toLowerCase() });
    }
    for (auto& kv : byType)
    {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const Cand& a, const Cand& b)
                  {
                      if (a.prio != b.prio) return a.prio < b.prio;
                      return a.sortName.compareIgnoreCase(b.sortName) < 0;
                  });
    }

    // Round-robin across types: take one from each type per pass (the next
    // unused, highest-priority candidate), cycling until we hit the cap or
    // run out. This interleaves types so the cap can't be eaten entirely by
    // one huge category. Type visitation order is fixed (alphabetical by tag)
    // for determinism; the final shuffle randomises presentation order.
    std::vector<juce::String> names;
    std::map<juce::String, size_t> cursor;
    bool progressed = true;
    while (names.size() < kMaxFeedPlugins && progressed)
    {
        progressed = false;
        for (auto& kv : byType)
        {
            auto& bucket = kv.second;
            size_t& idx = cursor[kv.first];
            if (idx < bucket.size())
            {
                names.push_back(bucket[idx].display);
                ++idx;
                progressed = true;
                if (names.size() >= kMaxFeedPlugins) break;
            }
        }
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

int PluginScanner::getEnabledEffectCount() const
{
    std::lock_guard<std::mutex> lock(pluginMutex);
    int n = 0;
    for (auto& p : plugins)
        if (p.category == "Effect" && disabledUids.count(p.uid) == 0)
            ++n;
    return n;
}

juce::String PluginScanner::getFullPluginList() const
{
    // Complete enabled effect list, no cap. Order is the vector order (already
    // sorted alphabetically after a scan), which is fine for an injected
    // per-turn block — it's not in the cached system prompt, so a stable
    // order isn't needed for caching here.
    std::lock_guard<std::mutex> lock(pluginMutex);
    juce::StringArray arr;
    for (auto& p : plugins)
        if (p.category == "Effect" && disabledUids.count(p.uid) == 0)
            arr.add(p.name + " (" + p.manufacturer + ")");
    return arr.joinIntoString(", ");
}

juce::String PluginScanner::getPluginSummary() const
{
    // A short, cache-safe overview for the system prompt: total enabled effect
    // count plus the biggest manufacturers by count. Gives the AI the SHAPE of
    // the library ("this user has a lot of Waves and a FabFilter bundle")
    // without listing anything, so it's a handful of tokens regardless of
    // whether the user owns 50 plugins or 2000.
    std::lock_guard<std::mutex> lock(pluginMutex);

    int total = 0;
    std::map<juce::String, int> byManu;
    for (auto& p : plugins)
    {
        if (p.category != "Effect" || disabledUids.count(p.uid) > 0) continue;
        ++total;
        // Normalise the stock labels to something human ("Logic Pro Stock" ->
        // "Logic stock") and group all stock under their DAW name as-is.
        byManu[p.manufacturer]++;
    }

    if (total == 0) return {};

    // Rank manufacturers by count, take the top few.
    std::vector<std::pair<juce::String,int>> ranked(byManu.begin(), byManu.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    juce::StringArray parts;
    const int kTopManus = 6;
    for (int i = 0; i < (int) ranked.size() && i < kTopManus; ++i)
        parts.add(ranked[(size_t) i].first + " (" + juce::String(ranked[(size_t) i].second) + ")");

    juce::String summary = juce::String(total) + " plugins";
    if (! parts.isEmpty())
        summary += " incl. " + parts.joinIntoString(", ");
    if ((int) ranked.size() > kTopManus)
        summary += ", and more";
    return summary;
}

juce::File PluginScanner::getCacheFile()
{
    auto appData = echojay::userAppData();
    
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
        std::map<juce::String, size_t> loadedByUid;   // uid-equality dedupe, see below

        for (auto& item : *arr)
        {
            if (auto* obj = item.getDynamicObject())
            {
                ScannedPlugin p;
                p.name = obj->getProperty("name").toString();
                p.manufacturer = obj->getProperty("manufacturer").toString();
                // Correct legacy cache entries where a category folder name
                // ("Guitar and Bass", "Equalizers") was stored as the
                // manufacturer (UAD-style layouts). This fixes existing caches
                // without forcing a rescan.
                {
                    auto corrected = echojay::resolveManufacturer(p.name, p.manufacturer);
                    if (corrected.isNotEmpty())
                        p.manufacturer = corrected;
                    // Match the canonical label a fresh scan would store, so a
                    // stale cache shows the same vendor grouping (Soundtoys,
                    // Universal Audio, etc.) until the next rescan rewrites it.
                    p.manufacturer = echojay::canonicaliseManufacturer(p.manufacturer);
                    p.name = echojay::stripChannelVariant(p.name);
                }
                p.format = obj->getProperty("format").toString();
                p.category = obj->getProperty("category").toString();
                p.uid = echojay::makeUid(p.name, p.manufacturer);
                // Legacy spellings, derived from the DATA rather than the
                // rule tables: any uid this row would have carried under
                // less normalization (raw fields, or the cache's stored
                // uid) maps to the canonical one, and the migration in
                // loadEnabledState rewrites set entries through this map.
                // Rule enumeration is not needed and cannot rot: whatever
                // rewrote this row's identity is captured by comparing what
                // the row WAS called with what it IS called.
                {
                    const auto rawName = obj->getProperty("name").toString();
                    const auto rawManu = obj->getProperty("manufacturer").toString();
                    const auto storedUid = obj->getProperty("uid").toString();
                    const auto rawUid = rawName.toLowerCase().replaceCharacter(' ', '_')
                                      + "_" + rawManu.toLowerCase().replaceCharacter(' ', '_');
                    if (rawUid != p.uid)    legacyUidMap_[rawUid]    = p.uid;
                    if (storedUid.isNotEmpty() && storedUid != p.uid)
                                            legacyUidMap_[storedUid] = p.uid;
                }
                // The cache's "enabled" field is IGNORED on load (13 Aug
                // 2026): it is a serialization artifact for the WebView, and
                // reading it back is how the second copy of the tick state
                // existed. disabledUids is the authority; the flag is
                // stamped from it at read.
                // Reclassify effect type on load (cheap, keyword-based) rather
                // than persisting it — keeps the cache format stable and means
                // classifier improvements apply to cached entries too.
                if (p.category == "Effect")
                    p.fxType = echojay::fxTypeTag(echojay::classifyEffect(p.name));
                // Same uid-equality dedupe as addPlugin, so a cache written
                // BEFORE the dedupe existed converges at the next launch
                // rather than waiting for a rescan: a row whose canonical
                // uid already landed merges its format and is dropped.
                if (auto seen = loadedByUid.find(p.uid); seen != loadedByUid.end())
                {
                    auto& kept = plugins[seen->second];
                    if (! kept.format.contains(p.format))
                        kept.format += "/" + p.format;
                    continue;
                }
                loadedByUid[p.uid] = plugins.size();
                plugins.push_back(p);
            }
        }
        // Built the vector directly (not via addPlugin), so build the dedupe
        // index to match.
        rebuildIndex();
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
