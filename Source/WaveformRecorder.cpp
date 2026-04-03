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
    // Clear previous recording
    writePos = 0;
    totalSamplesRecorded.store(0);
    thumbMinAccum = 0.0f;
    thumbMaxAccum = 0.0f;
    thumbSampleCount = 0;

    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        thumbnailData.clear();
    }
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        lastSavedPath.clear();
    }

    // Pre-allocate for ~30s of audio (will grow if needed)
    int initialCapacity = (int)(currentSampleRate * 30.0);
    if (audioBuffer.getNumSamples() < initialCapacity)
    {
        audioBuffer.setSize(2, initialCapacity, false, true, false);
        bufferCapacity = initialCapacity;
    }

    recording.store(true);
}

void WaveformRecorder::stopRecording()
{
    recording.store(false);

    // Flush any remaining thumbnail samples
    if (thumbSampleCount > 0)
        flushThumbnailPoint();
}

void WaveformRecorder::reset()
{
    recording.store(false);
    writePos = 0;
    totalSamplesRecorded.store(0);
    thumbMinAccum = 0.0f;
    thumbMaxAccum = 0.0f;
    thumbSampleCount = 0;

    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        thumbnailData.clear();
    }
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        lastSavedPath.clear();
    }

    // Don't deallocate — keep the buffer for the next recording
}

void WaveformRecorder::ensureCapacity(int requiredSamples)
{
    if (requiredSamples <= bufferCapacity) return;

    int newCapacity = bufferCapacity + kGrowChunkSamples;
    while (newCapacity < requiredSamples) newCapacity += kGrowChunkSamples;

    audioBuffer.setSize(2, newCapacity, true, true, false);
    bufferCapacity = newCapacity;
}

void WaveformRecorder::processBlock(const float* left, const float* right, int numSamples)
{
    if (!recording.load()) return;

    ensureCapacity(writePos + numSamples);

    // Copy audio into buffer
    auto* destL = audioBuffer.getWritePointer(0);
    auto* destR = audioBuffer.getWritePointer(1);

    for (int i = 0; i < numSamples; ++i)
    {
        destL[writePos + i] = left[i];
        destR[writePos + i] = right != nullptr ? right[i] : left[i];
    }

    // Update thumbnail
    pushThumbnailSamples(left, right, numSamples);

    writePos += numSamples;
    totalSamplesRecorded.store(writePos);
}

// ============================================================================
// Thumbnail
// ============================================================================

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
    ThumbnailPoint pt { thumbMinAccum, thumbMaxAccum };
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        thumbnailData.push_back(pt);
    }
    thumbMinAccum = 0.0f;
    thumbMaxAccum = 0.0f;
    thumbSampleCount = 0;
}

int WaveformRecorder::getNumThumbnailPoints() const
{
    std::lock_guard<std::mutex> lock(thumbnailMutex);
    return (int)thumbnailData.size();
}

std::vector<WaveformRecorder::ThumbnailPoint> WaveformRecorder::getThumbnail() const
{
    std::lock_guard<std::mutex> lock(thumbnailMutex);
    return thumbnailData;
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

    // Write from the audio buffer
    juce::AudioBuffer<float> tempBuf(2, numSamples);
    tempBuf.copyFrom(0, 0, audioBuffer, 0, 0, numSamples);
    tempBuf.copyFrom(1, 0, audioBuffer, 1, 0, numSamples);

    writer->writeFromAudioSampleBuffer(tempBuf, 0, numSamples);
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
    return &audioBuffer;
}
