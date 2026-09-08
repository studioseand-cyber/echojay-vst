#include "WaveformRecorder.h"
#include <cmath>
#include <algorithm>

WaveformRecorder::WaveformRecorder() {}
WaveformRecorder::~WaveformRecorder() {}

void WaveformRecorder::prepare(double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;
}

void WaveformRecorder::startRecording()
{
    writePos = 0;
    totalSamplesRecorded.store(0);
    overrunSamples_.store(0);
    thumbMinAccum = 0.0f;
    thumbMaxAccum = 0.0f;
    thumbSampleCount = 0;
    thumbCount_.store(0);
    if ((int) thumbnailData.size() < kMaxThumbPoints) thumbnailData.resize((size_t) kMaxThumbPoints);   // message thread, once
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        lastSavedPath.clear();
    }
    { std::lock_guard<std::mutex> lock(finaliseMutex); finalised_ = false; audioBuffer.setSize(0, 0, false, false, false); }
    freeChunks();
    growAheadIfNeeded();                 // the first kChunksAhead chunks, on THIS (non-audio) thread
    recording.store(true);
}

void WaveformRecorder::growAheadIfNeeded()
{
    // Non-audio thread. Publish chunks with release; the audio thread reads readyChunks_ with acquire.
    const int cur = totalSamplesRecorded.load() / kChunkSamples;
    while (readyChunks_.load() - cur < kChunksAhead && readyChunks_.load() < kMaxChunks)
    {
        auto* b = new juce::AudioBuffer<float>(2, kChunkSamples);
        b->clear();
        const int idx = readyChunks_.load();
        chunks_[(size_t) idx].store(b, std::memory_order_release);
        readyChunks_.store(idx + 1, std::memory_order_release);
    }
}

void WaveformRecorder::freeChunks()
{
    const int n = readyChunks_.load();
    readyChunks_.store(0);
    for (int i = 0; i < n; ++i) { delete chunks_[(size_t) i].exchange(nullptr); }
}

void WaveformRecorder::finalise() const
{
    std::lock_guard<std::mutex> lock(finaliseMutex);
    if (finalised_) return;
    const int total = totalSamplesRecorded.load();
    audioBuffer.setSize(2, std::max(0, total), false, false, false);
    int done = 0, idx = 0;
    while (done < total)
    {
        auto* c = chunks_[(size_t) idx].load(std::memory_order_acquire);
        if (c == nullptr) break;
        const int take = std::min(kChunkSamples, total - done);
        audioBuffer.copyFrom(0, done, *c, 0, 0, take);
        audioBuffer.copyFrom(1, done, *c, 1, 0, take);
        done += take; ++idx;
    }
    finalised_ = true;
}

void WaveformRecorder::stopRecording()
{
    recording.store(false);      // atomic; the final partial thumbnail point is flushed by the first reader (non-audio)
}

void WaveformRecorder::reset()
{
    recording.store(false);
    writePos = 0;
    totalSamplesRecorded.store(0);
    overrunSamples_.store(0);
    thumbMinAccum = 0.0f;
    thumbMaxAccum = 0.0f;
    thumbSampleCount = 0;
    thumbCount_.store(0);
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        lastSavedPath.clear();
    }
    { std::lock_guard<std::mutex> lock(finaliseMutex); finalised_ = false; audioBuffer.setSize(0, 0, false, false, false); }
    freeChunks();
}

void WaveformRecorder::releaseAudioBuffer()
{
    if (recording.load()) return;
    { std::lock_guard<std::mutex> lock(finaliseMutex); audioBuffer.setSize(0, 0, false, false, false); finalised_ = false; }
    freeChunks();
    writePos = 0;
}

void WaveformRecorder::processBlock(const float* left, const float* right, int numSamples)
{
    if (!recording.load()) return;
    // AUDIO THREAD: no allocation, no copy of the recording, no lock. Write into the
    // ready chunks; if none is ready (the grow-ahead fell behind) drop and count.
    int remaining = numSamples, src = 0;
    const int ready = readyChunks_.load(std::memory_order_acquire);
    while (remaining > 0)
    {
        const int idx = writePos / kChunkSamples;
        if (idx >= ready) { overrunSamples_.fetch_add(remaining); break; }
        auto* c = chunks_[(size_t) idx].load(std::memory_order_acquire);
        if (c == nullptr) { overrunSamples_.fetch_add(remaining); break; }
        const int off  = writePos % kChunkSamples;
        const int take = std::min(remaining, kChunkSamples - off);
        auto* destL = c->getWritePointer(0) + off;
        auto* destR = c->getWritePointer(1) + off;
        for (int i = 0; i < take; ++i)
        {
            destL[i] = left[src + i];
            destR[i] = right != nullptr ? right[src + i] : left[src + i];
        }
        writePos += take; src += take; remaining -= take;
    }
    pushThumbnailSamples(left, right, numSamples);
    totalSamplesRecorded.store(writePos);
}

void WaveformRecorder::pushThumbnailSamples(const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        float mono = left[i];
        if (right != nullptr) mono = (left[i] + right[i]) * 0.5f;

        if (thumbSampleCount == 0)
        {
            thumbMinAccum = mono;
            thumbMaxAccum = mono;
        }
        else
        {
            thumbMinAccum = std::min(thumbMinAccum, mono);
            thumbMaxAccum = std::max(thumbMaxAccum, mono);
        }
        thumbSampleCount++;

        if (thumbSampleCount >= kThumbnailResolution)
            flushThumbnailPoint();
    }
}

void WaveformRecorder::flushThumbnailPoint()
{
    // AUDIO THREAD: write into the preallocated array, publish the count with release.
    const int n = thumbCount_.load(std::memory_order_relaxed);
    if (n < (int) thumbnailData.size())
    {
        thumbnailData[(size_t) n] = ThumbnailPoint { thumbMinAccum, thumbMaxAccum };
        thumbCount_.store(n + 1, std::memory_order_release);
    }
    thumbMinAccum = 0.0f;
    thumbMaxAccum = 0.0f;
    thumbSampleCount = 0;
}

int WaveformRecorder::getNumThumbnailPoints() const
{
    return thumbCount_.load(std::memory_order_acquire);
}

std::vector<WaveformRecorder::ThumbnailPoint> WaveformRecorder::getThumbnail() const
{
    const int n = std::min(thumbCount_.load(std::memory_order_acquire), (int) thumbnailData.size());
    return std::vector<ThumbnailPoint>(thumbnailData.begin(), thumbnailData.begin() + n);
}

float WaveformRecorder::getRecordedDuration() const
{
    return (float)totalSamplesRecorded.load() / (float)currentSampleRate;
}

// ============================================================================
// WAV Output
// ============================================================================

juce::String WaveformRecorder::saveToWAV(const juce::File& directory, const juce::String& passName)
{
    int numSamples = totalSamplesRecorded.load();
    if (numSamples <= 0) return {};
    finalise();   // builds the contiguous buffer once, on this (non-audio) thread

    // Sanitise filename
    juce::String safeName = passName.replaceCharacter(' ', '_')
                                     .retainCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");
    if (safeName.isEmpty()) safeName = "capture";

    // Timestamp to avoid collisions
    auto now = juce::Time::getCurrentTime();
    juce::String timestamp = now.formatted("%Y%m%d_%H%M%S");
    juce::String filename = safeName + "_" + timestamp + ".wav";

    juce::File outFile = directory.getChildFile(filename);
    directory.createDirectory(); // ensure folder exists

    // Create WAV writer (32-bit float, stereo)
    juce::WavAudioFormat wavFormat;
    auto* outputStream = new juce::FileOutputStream(outFile);
    if (outputStream->failedToOpen())
    {
        delete outputStream;
        return {};
    }

    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(outputStream,
                                   currentSampleRate,
                                   2,    // number of channels
                                   32,   // bits per sample (float)
                                   {},   // metadata
                                   0));  // quality option index
    if (writer == nullptr)
    {
        // outputStream is deleted by createWriterFor on failure
        return {};
    }

    // Write straight from the capture buffer. The old code copied the whole
    // recording into a temp buffer first, DOUBLING peak memory per save
    // (the measured spike-then-partial-release); the writer reads a
    // sub-range fine.
    writer->writeFromAudioSampleBuffer(audioBuffer, 0, numSamples);
    writer.reset(); // flush and close

    // Store last path
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        lastSavedPath = outFile.getFullPathName();
    }

    return outFile.getFullPathName();
}

juce::String WaveformRecorder::getLastSavedPath() const
{
    std::lock_guard<std::mutex> lock(pathMutex);
    return lastSavedPath;
}

const juce::AudioBuffer<float>* WaveformRecorder::getRecordedBuffer() const
{
    if (totalSamplesRecorded.load() <= 0) return nullptr;
    if (recording.load()) return nullptr;    // contiguous view exists only after the recording stopped
    finalise();
    return &audioBuffer;
}
