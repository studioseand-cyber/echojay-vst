#pragma once
#include <JuceHeader.h>
#include <vector>
#include <mutex>

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

private:
    void scanPluginDirectories();
    void scanDirectory(const juce::File& dir, const juce::String& format);
    void addPlugin(const juce::String& name, const juce::String& manufacturer,
                   const juce::String& format, const juce::String& category,
                   const juce::String& path);
    
    mutable std::mutex pluginMutex;
    std::vector<ScannedPlugin> plugins;
    std::atomic<bool> scanning { false };
    std::atomic<float> progress { 0.0f };
    std::unique_ptr<juce::Thread> scanThread;
    
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
