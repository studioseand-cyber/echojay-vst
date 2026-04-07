#include "ReferenceAnalyser.h"

ReferenceAnalyser::ReferenceAnalyser() {}
ReferenceAnalyser::~ReferenceAnalyser() {}

void ReferenceAnalyser::analyseFile(const juce::File& file,
                                     std::function<void(bool success, const juce::String& error)> onComplete)
{
    if (analysing.load())
    {
        if (onComplete) onComplete(false, "Already analysing a file");
        return;
    }
    
    if (!file.existsAsFile())
    {
        if (onComplete) onComplete(false, "File not found");
        return;
    }
    
    analysing.store(true);
    progress.store(0.0f);
    
    auto fileCopy = file;
    auto cb = std::make_shared<std::function<void(bool, const juce::String&)>>(onComplete);
    auto thisPtr = this;
    
    juce::Thread::launch([thisPtr, fileCopy, cb]()
    {
        // Create an audio format manager and register formats
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats(); // WAV, AIFF, FLAC, MP3, etc.
        
        // Create a reader for the file
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(fileCopy));
        
        if (reader == nullptr)
        {
            thisPtr->analysing.store(false);
            auto callback = cb;
            juce::MessageManager::callAsync([callback]() {
                (*callback)(false, "Unsupported audio format");
            });
            return;
        }
        
        double sampleRate = reader->sampleRate;
        juce::int64 totalSamples = reader->lengthInSamples;
        int numChannels = (int)reader->numChannels;
        float duration = (float)totalSamples / (float)sampleRate;
        
        if (totalSamples == 0 || sampleRate == 0)
        {
            thisPtr->analysing.store(false);
            auto callback = cb;
            juce::MessageManager::callAsync([callback]() {
                (*callback)(false, "Empty or invalid audio file");
            });
            return;
        }
        
        // Set up the meter engine for offline analysis
        MeterEngine engine;
        int blockSize = 2048;
        engine.prepare(sampleRate, blockSize);
        
        // Waveform thumbnail — one peak value per 1024 samples
        int thumbSamplesPerPoint = 1024;
        std::vector<float> waveformThumb;
        float currentPeak = 0;
        int thumbSampleCount = 0;
        
        // Spectrum accumulator for EQ curve
        std::array<float, 64> specSum = {};
        int specFrames = 0;
        
        // Proper width calculation — accumulate HPF mid/side energy over entire file
        // 2nd-order Butterworth HPF at 150Hz
        double f0 = 150.0, Q = 0.7071;
        double K = std::tan(juce::MathConstants<double>::pi * f0 / sampleRate);
        double a0_ = 1.0 + K / Q + K * K;
        double b0 = 1.0 / a0_, b1 = -2.0 / a0_, b2 = 1.0 / a0_;
        double a1 = 2.0 * (K * K - 1.0) / a0_, a2 = (1.0 - K / Q + K * K) / a0_;
        // Filter state for L and R
        double hx1L = 0, hx2L = 0, hy1L = 0, hy2L = 0;
        double hx1R = 0, hx2R = 0, hy1R = 0, hy2R = 0;
        double totalMidE = 0, totalSideE = 0;
        juce::int64 widthSampleCount = 0;
        // Skip first 4096 samples for HPF warmup
        juce::int64 warmupSamples = 4096;
        
        // Also accumulate correlation
        double totalCorr = 0, totalEnL = 0, totalEnR = 0;
        
        // Read and process in blocks
        juce::AudioBuffer<float> buffer(numChannels, blockSize);
        juce::int64 samplesRead = 0;
        
        while (samplesRead < totalSamples)
        {
            int samplesToRead = (int)juce::jmin((juce::int64)blockSize, totalSamples - samplesRead);
            
            buffer.clear();
            reader->read(&buffer, 0, samplesToRead, samplesRead, true, true);
            
            const float* left = buffer.getReadPointer(0);
            const float* right = numChannels >= 2 ? buffer.getReadPointer(1) : left;
            
            engine.processBlock(left, right, samplesToRead);
            
            // Width + correlation: process each sample through HPF
            for (int i = 0; i < samplesToRead; ++i)
            {
                double sL = (double)left[i];
                double sR = numChannels >= 2 ? (double)right[i] : sL;
                
                // Apply HPF to L
                double outL = b0 * sL + b1 * hx1L + b2 * hx2L - a1 * hy1L - a2 * hy2L;
                hx2L = hx1L; hx1L = sL; hy2L = hy1L; hy1L = outL;
                // Apply HPF to R
                double outR = b0 * sR + b1 * hx1R + b2 * hx2R - a1 * hy1R - a2 * hy2R;
                hx2R = hx1R; hx1R = sR; hy2R = hy1R; hy1R = outR;
                
                if (samplesRead + i >= warmupSamples) // skip warmup
                {
                    double mid = (outL + outR) * 0.5;
                    double side = (outL - outR) * 0.5;
                    totalMidE += mid * mid;
                    totalSideE += side * side;
                    totalCorr += sL * sR;
                    totalEnL += sL * sL;
                    totalEnR += sR * sR;
                    widthSampleCount++;
                }
            }
            
            // Build waveform thumbnail
            for (int i = 0; i < samplesToRead; ++i)
            {
                float s = std::abs(left[i]);
                if (numChannels >= 2) s = std::max(s, std::abs(right[i]));
                currentPeak = std::max(currentPeak, s);
                thumbSampleCount++;
                if (thumbSampleCount >= thumbSamplesPerPoint)
                {
                    waveformThumb.push_back(currentPeak);
                    currentPeak = 0;
                    thumbSampleCount = 0;
                }
            }
            
            // Accumulate spectrum for EQ curve
            auto md = engine.getMeterData();
            for (int i = 0; i < 64; ++i)
                specSum[(size_t)i] += md.spectrum[(size_t)i];
            specFrames++;
            
            samplesRead += samplesToRead;
            thisPtr->progress.store((float)samplesRead / (float)totalSamples);
        }
        if (thumbSampleCount > 0)
            waveformThumb.push_back(currentPeak);
        
        // Get the final meter data
        MeterData data = engine.getMeterData();
        
        // Override width with properly accumulated HPF mid/side over entire file
        if (widthSampleCount > 0 && totalMidE > 0.00001)
        {
            float smRatio = (float)(totalSideE / totalMidE);
            data.width = std::min(100.0f, std::sqrt(smRatio) * 100.0f);
        }
        
        // Override correlation with properly accumulated value
        if (widthSampleCount > 0 && totalEnL > 0.00001 && totalEnR > 0.00001)
        {
            data.correlation = (float)(totalCorr / std::sqrt(totalEnL * totalEnR));
        }
        
        // Store the result
        ReferenceResult ref;
        ref.name = fileCopy.getFileNameWithoutExtension();
        ref.path = fileCopy.getFullPathName();
        ref.data = data;
        ref.durationSeconds = duration;
        ref.isReference = true;
        ref.timestamp = juce::Time::currentTimeMillis();
        ref.waveformThumbnail = waveformThumb;
        
        // Average spectrum into EQ curve
        if (specFrames > 0)
            for (int i = 0; i < 64; ++i)
                ref.eqCurve[(size_t)i] = specSum[(size_t)i] / (float)specFrames;
        
        {
            std::lock_guard<std::mutex> lock(thisPtr->refMutex);
            thisPtr->references.push_back(ref);
        }
        
        thisPtr->analysing.store(false);
        thisPtr->progress.store(1.0f);
        
        auto callback = cb;
        juce::MessageManager::callAsync([callback]() {
            (*callback)(true, "");
        });
    });
}

std::vector<ReferenceResult> ReferenceAnalyser::getReferences() const
{
    std::lock_guard<std::mutex> lock(refMutex);
    return references;
}

int ReferenceAnalyser::getReferenceCount() const
{
    std::lock_guard<std::mutex> lock(refMutex);
    return (int)references.size();
}

ReferenceResult ReferenceAnalyser::getReference(int index) const
{
    std::lock_guard<std::mutex> lock(refMutex);
    if (index >= 0 && index < (int)references.size())
        return references[index];
    return {};
}

void ReferenceAnalyser::removeReference(int index)
{
    std::lock_guard<std::mutex> lock(refMutex);
    if (index >= 0 && index < (int)references.size())
        references.erase(references.begin() + index);
}

void ReferenceAnalyser::clearAll()
{
    std::lock_guard<std::mutex> lock(refMutex);
    references.clear();
}
