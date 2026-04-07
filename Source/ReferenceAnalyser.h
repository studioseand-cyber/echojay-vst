#pragma once
#include <JuceHeader.h>
#include "MeterEngine.h"
#include <vector>
#include <mutex>

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
    
    // Get all reference results
    std::vector<ReferenceResult> getReferences() const;
    int getReferenceCount() const;
    
    // Remove a reference by index
    void removeReference(int index);
    
    // Clear all references
    void clearAll();
    
    // Get a specific reference
    ReferenceResult getReference(int index) const;

private:
    mutable std::mutex refMutex;
    std::vector<ReferenceResult> references;
    std::atomic<bool> analysing { false };
    std::atomic<float> progress { 0.0f };
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceAnalyser)
};
