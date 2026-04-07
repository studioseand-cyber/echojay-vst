#pragma once
#include <JuceHeader.h>
#include "MeterEngine.h"
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>

struct ReferenceResult {
    juce::String name;          // Filename
    juce::String path;          // Full path
    MeterData data;             // Analysed meter data
    float durationSeconds = 0;
    bool isReference = true;
    juce::int64 timestamp = 0;
    std::vector<float> waveformThumbnail; // peak values for waveform display (one per ~1024 samples)
    std::array<float, 64> eqCurve = {};   // averaged spectrum for EQ curve display
};

class ReferenceAnalyser
{
public:
    ReferenceAnalyser();
    ~ReferenceAnalyser();
    
    // Analyse an audio file on a background thread
    // Calls onComplete on the message thread when done
    void analyseFile(const juce::File& file,
                     std::function<void(bool success, const juce::String& error)> onComplete);
    
    // Check if currently analysing
    bool isAnalysing() const { return analysing.load(); }
    float getProgress() const { return progress.load(); }
    
    // Force-reset if stuck (called from UI after timeout)
    void forceResetIfStuck()
    {
        if (analysing.load())
        {
            auto now = juce::Time::currentTimeMillis();
            if (now - analysisStartTime > 30000) // 30 seconds timeout
            {
                analysing.store(false);
                progress.store(0.0f);
            }
        }
    }
    
    // Get all reference results
    std::vector<ReferenceResult> getReferences() const;
    int getReferenceCount() const;
    
    // Remove a reference by index
    void removeReference(int index);
    
    // Clear all references
    void clearAll();
    
    // Get a specific reference
    ReferenceResult getReference(int index) const;

    // Queue multiple files — analyses run sequentially
    void analyseFiles(const std::vector<juce::File>& files,
                      std::function<void(bool success, const juce::String& error)> onEachComplete);

private:
    void processQueue();

    mutable std::mutex refMutex;
    std::vector<ReferenceResult> references;
    std::atomic<bool> analysing { false };
    std::atomic<float> progress { 0.0f };
    juce::int64 analysisStartTime = 0;
    
    // Pending queue for sequential analysis
    struct QueuedFile {
        juce::File file;
        std::function<void(bool, const juce::String&)> callback;
    };
    std::mutex queueMutex;
    std::vector<QueuedFile> pendingQueue;
    
    // Shared flag: set to false in destructor so in-flight callbacks bail out
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>>(true) };
    // Tracked thread so destructor can wait for it
    std::unique_ptr<juce::Thread> analyseThread;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceAnalyser)
};
