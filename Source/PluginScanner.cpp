#include "PluginScanner.h"
#include <algorithm>

PluginScanner::PluginScanner() {}

PluginScanner::~PluginScanner()
{
    if (scanThread && scanThread->isThreadRunning())
    {
        scanThread->stopThread(5000);
    }
}

void PluginScanner::startScan()
{
    if (scanning.load()) return;
    
    scanning.store(true);
    progress.store(0.0f);
    
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        plugins.clear();
    }
    
    scanThread = std::make_unique<ScanThread>(*this);
    scanThread->startThread();
}

void PluginScanner::scanPluginDirectories()
{
    // ============ macOS ============
#if JUCE_MAC
    // VST3
    juce::Array<juce::File> vst3Dirs;
    vst3Dirs.add(juce::File("/Library/Audio/Plug-Ins/VST3"));
    vst3Dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile("Library/Audio/Plug-Ins/VST3"));
    
    for (auto& dir : vst3Dirs)
        if (dir.isDirectory())
            scanDirectory(dir, "VST3");
    
    progress.store(0.33f);
    
    // AU (Audio Units)
    juce::Array<juce::File> auDirs;
    auDirs.add(juce::File("/Library/Audio/Plug-Ins/Components"));
    auDirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                   .getChildFile("Library/Audio/Plug-Ins/Components"));
    
    for (auto& dir : auDirs)
        if (dir.isDirectory())
            scanDirectory(dir, "AU");
    
    progress.store(0.66f);
    
    // Legacy VST (VST2)
    juce::Array<juce::File> vstDirs;
    vstDirs.add(juce::File("/Library/Audio/Plug-Ins/VST"));
    vstDirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                    .getChildFile("Library/Audio/Plug-Ins/VST"));
    
    for (auto& dir : vstDirs)
        if (dir.isDirectory())
            scanDirectory(dir, "VST");
    
    // ============ Windows ============
#elif JUCE_WINDOWS
    // VST3
    juce::Array<juce::File> vst3Dirs;
    vst3Dirs.add(juce::File("C:\\Program Files\\Common Files\\VST3"));
    vst3Dirs.add(juce::File("C:\\Program Files (x86)\\Common Files\\VST3"));
    
    for (auto& dir : vst3Dirs)
        if (dir.isDirectory())
            scanDirectory(dir, "VST3");
    
    progress.store(0.33f);
    
    // VST2
    juce::Array<juce::File> vstDirs;
    vstDirs.add(juce::File("C:\\Program Files\\VSTPlugins"));
    vstDirs.add(juce::File("C:\\Program Files\\Steinberg\\VSTPlugins"));
    vstDirs.add(juce::File("C:\\Program Files (x86)\\VSTPlugins"));
    vstDirs.add(juce::File("C:\\Program Files (x86)\\Steinberg\\VSTPlugins"));
    
    for (auto& dir : vstDirs)
        if (dir.isDirectory())
            scanDirectory(dir, "VST");
    
    // ============ Linux ============
#elif JUCE_LINUX
    juce::Array<juce::File> vst3Dirs;
    vst3Dirs.add(juce::File("/usr/lib/vst3"));
    vst3Dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile(".vst3"));
    
    for (auto& dir : vst3Dirs)
        if (dir.isDirectory())
            scanDirectory(dir, "VST3");
#endif
    
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

void PluginScanner::scanDirectory(const juce::File& dir, const juce::String& format)
{
    if (!dir.isDirectory()) return;
    
    // Determine file extensions to look for
    juce::String extension;
    if (format == "VST3")      extension = ".vst3";
    else if (format == "AU")   extension = ".component";
    else if (format == "VST")  extension = ".vst";
    
    // Find all plugin files/bundles in this directory
    for (const auto& entry : juce::RangedDirectoryIterator(dir, true, "*" + extension,
                                                             juce::File::findFilesAndDirectories))
    {
        auto file = entry.getFile();
        juce::String name = file.getFileNameWithoutExtension();
        
        // Skip obvious non-plugins
        if (name.isEmpty() || name.startsWith(".")) continue;
        
        // Try to extract manufacturer from path
        // Common patterns: /Manufacturer/PluginName.vst3
        juce::String manufacturer = "Unknown";
        auto parent = file.getParentDirectory();
        if (parent != dir)
        {
            manufacturer = parent.getFileName();
            // If the parent is just the format folder, check one level up
            if (manufacturer.endsWithIgnoreCase("VST3") ||
                manufacturer.endsWithIgnoreCase("Components") ||
                manufacturer.endsWithIgnoreCase("VST"))
            {
                manufacturer = "Unknown";
            }
        }
        
        // For VST3, try to read the moduleinfo.json if it exists
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
        
        // Determine category heuristic based on common naming
        juce::String category = "Effect";
        juce::String nameLower = name.toLowerCase();
        if (nameLower.contains("synth") || nameLower.contains("keys") ||
            nameLower.contains("piano") || nameLower.contains("organ") ||
            nameLower.contains("sampler") || nameLower.contains("drum machine"))
        {
            category = "Instrument";
        }
        
        addPlugin(name, manufacturer, format, category, file.getFullPathName());
    }
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
    
    juce::StringArray names;
    for (auto& p : plugins)
    {
        if (p.category == "Effect")  // Only list effects for mix feedback
            names.add(p.name + " (" + p.manufacturer + ")");
    }
    
    return names.joinIntoString(", ");
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
