#pragma once
#include <JuceHeader.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <array>

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

    // Free the capture audio memory (post-save). Keeps the thumbnail, the
    // recorded totals, and the last-saved path; the buffer re-allocates on
    // the next startRecording. Refuses while recording. The recorded audio
    // has NO consumers once the WAV is written (playback plays the file,
    // display uses the thumbnail), so holding tens of MB per capture until
    // the next pass was pure retention.
    void releaseAudioBuffer();

    // Currently allocated capture-buffer bytes (memdiag)
    size_t getAllocatedBytes() const { return (size_t) readyChunks_.load() * (size_t) kChunkSamples * 2 * sizeof(float) + (size_t) audioBuffer.getNumSamples() * 2 * sizeof(float); }
    /** NON-audio thread (the processor's timer): keeps kChunksAhead chunks allocated
        beyond the write position. Cheap when nothing is needed. */
    void growAheadIfNeeded();
    /** Samples the audio thread had to drop because no chunk was ready. A leg asserts 0. */
    int  getOverrunSamples() const { return overrunSamples_.load(); }
    int  getReadyChunks() const { return readyChunks_.load(); }

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

    // ---- CHUNKED STORAGE (8 Sep 2026, AAE -9173 at ~30 s): the audio thread NEVER
    // allocates, copies or locks. It writes into fixed 10 s chunks that a non-audio
    // thread allocates AHEAD (growAheadIfNeeded, from the processor's timer). The
    // contiguous buffer consumers expect is built once, off the audio thread, by
    // finalise() after recording stops. The old design grew one contiguous buffer
    // with a copying reallocation inside processBlock - 11.5 MB per recorder at
    // the 30 s mark, 41 recorders at once in Sean's session.
    static constexpr int kChunkSamples = 480000;    // 10 s at 48 kHz (the unit is samples, not seconds)
    static constexpr int kMaxChunks    = 1440;      // 4 h at 48 kHz
    static constexpr int kChunksAhead  = 2;         // always this many ready beyond the write position
    std::array<std::atomic<juce::AudioBuffer<float>*>, kMaxChunks> chunks_ {};
    std::atomic<int>  readyChunks_ { 0 };           // chunks allocated and published (release/acquire)
    std::atomic<int>  overrunSamples_ { 0 };        // samples dropped because no chunk was ready (must stay 0)
    int writePos = 0;                               // audio thread only
    void freeChunks();

    // The contiguous result: valid after finalise(); built by the first non-audio
    // consumer (saveToWAV / getRecordedBuffer) after stopRecording.
    mutable juce::AudioBuffer<float> audioBuffer;
    mutable std::mutex finaliseMutex;               // non-audio threads only
    mutable bool finalised_ = false;
    void finalise() const;

    // Thumbnail — one min/max point per kThumbnailResolution samples, written by
    // the audio thread into a PREALLOCATED array (no push_back, no mutex); readers
    // copy up to thumbCount_.
    static constexpr int kThumbnailResolution = 1024;
    static constexpr int kMaxThumbPoints = kMaxChunks * (kChunkSamples / kThumbnailResolution) + 2;
    std::vector<ThumbnailPoint> thumbnailData;      // sized kMaxThumbPoints at prepare
    std::atomic<int> thumbCount_ { 0 };

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
