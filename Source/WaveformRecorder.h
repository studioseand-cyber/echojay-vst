#pragma once
#include <JuceHeader.h>
#include <vector>
#include <mutex>
#include <atomic>

//==============================================================================
// WaveformRecorder
//
// Records stereo audio during capture, generates a downsampled waveform
// thumbnail for real-time display, and writes 32-bit float WAV files to
// the project folder on stop.
//==============================================================================

class WaveformRecorder
{
public:
    WaveformRecorder();
    ~WaveformRecorder();

    // Call once in prepareToPlay
    void prepare(double sampleRate, int samplesPerBlock);

    // Start recording — clears previous buffer
    void startRecording();

    // Stop recording — finalises the waveform thumbnail
    void stopRecording();

    // Reset everything (clears audio + thumbnail)
    void reset();

    // Feed audio from processBlock (called on audio thread)
    void processBlock(const float* left, const float* right, int numSamples);

    // ========== Display data ==========

    // Number of thumbnail points available (each point = min/max pair)
    int getNumThumbnailPoints() const;

    struct ThumbnailPoint { float minVal; float maxVal; };

    // Get thumbnail data (thread-safe copy). Returns empty if nothing recorded.
    std::vector<ThumbnailPoint> getThumbnail() const;

    // Is currently recording?
    bool isRecording() const { return recording.load(); }

    // Duration of current recording in seconds
    float getRecordedDuration() const;

    // Total recorded sample count
    int getRecordedSampleCount() const { return totalSamplesRecorded.load(); }

    // ========== WAV file output ==========

    // Save recorded audio to WAV in the given directory.
    // Returns the full path of the written file, or empty on failure.
    // passName is used for the filename, e.g. "Pass 1" -> "Pass_1.wav"
    juce::String saveToWAV(const juce::File& directory, const juce::String& passName);

    // Get the file path of the last saved WAV (empty if none)
    juce::String getLastSavedPath() const;

    // ========== Playback helper ==========

    // Get the recorded audio buffer (for playback). Returns nullptr if empty.
    // Caller must NOT modify. Valid until next startRecording/reset.
    const juce::AudioBuffer<float>* getRecordedBuffer() const;

    double getRecordedSampleRate() const { return currentSampleRate; }

private:
    double currentSampleRate = 44100.0;
    std::atomic<bool> recording { false };
    std::atomic<int> totalSamplesRecorded { 0 };

    // Main audio buffer — grows in chunks
    juce::AudioBuffer<float> audioBuffer;
    int writePos = 0;
    int bufferCapacity = 0;
    static constexpr int kGrowChunkSamples = 441000; // ~10s at 44.1k

    void ensureCapacity(int requiredSamples);

    // Thumbnail — one min/max point per ~1024 samples (adjustable)
    static constexpr int kThumbnailResolution = 1024;
    mutable std::mutex thumbnailMutex;
    std::vector<ThumbnailPoint> thumbnailData;

    // Accumulator for current thumbnail point
    float thumbMinAccum = 0.0f, thumbMaxAccum = 0.0f;
    int thumbSampleCount = 0;

    void pushThumbnailSamples(const float* left, const float* right, int numSamples);
    void flushThumbnailPoint();

    // Last saved file path
    mutable std::mutex pathMutex;
    juce::String lastSavedPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformRecorder)
};
