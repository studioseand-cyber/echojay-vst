#include "MeterEngine.h"
#include <cmath>
#include <algorithm>
#include <numeric>

MeterEngine::MeterEngine() {}
MeterEngine::~MeterEngine() {}

void MeterEngine::computeKWeightingCoeffs(double sr)
{
    // Stage 1: Pre-filter (high shelf boost ~+4dB at high frequencies)
    // ITU-R BS.1770-4 coefficients for 48kHz, adjusted for other rates
    double f0 = 1681.974450955533;
    double G = 3.999843853973347;
    double Q = 0.7071752369554196;
    
    double K = std::tan(juce::MathConstants<double>::pi * f0 / sr);
    double Vh = std::pow(10.0, G / 20.0);
    double Vb = std::pow(Vh, 0.4996667741545416);
    double a0_ = 1.0 + K / Q + K * K;
    
    kStage1.b0 = (Vh + Vb * K / Q + K * K) / a0_;
    kStage1.b1 = 2.0 * (K * K - Vh) / a0_;
    kStage1.b2 = (Vh - Vb * K / Q + K * K) / a0_;
    kStage1.a1 = 2.0 * (K * K - 1.0) / a0_;
    kStage1.a2 = (1.0 - K / Q + K * K) / a0_;
    
    // Stage 2: High-pass filter (removes DC and very low frequencies)
    double f1 = 38.13547087602444;
    double Q1 = 0.5003270373238773;
    double K1 = std::tan(juce::MathConstants<double>::pi * f1 / sr);
    double a0_1 = 1.0 + K1 / Q1 + K1 * K1;
    
    kStage2.b0 = 1.0 / a0_1;
    kStage2.b1 = -2.0 / a0_1;
    kStage2.b2 = 1.0 / a0_1;
    kStage2.a1 = 2.0 * (K1 * K1 - 1.0) / a0_1;
    kStage2.a2 = (1.0 - K1 / Q1 + K1 * K1) / a0_1;
}

void MeterEngine::computeWidthHpfCoeffs(double sr)
{
    // 2nd-order Butterworth high-pass at 150Hz
    // Removes sub-bass from width calculation
    double f0 = 150.0;
    double Q = 0.7071; // Butterworth
    double K = std::tan(juce::MathConstants<double>::pi * f0 / sr);
    double a0_ = 1.0 + K / Q + K * K;
    
    widthHpf.b0 = 1.0 / a0_;
    widthHpf.b1 = -2.0 / a0_;
    widthHpf.b2 = 1.0 / a0_;
    widthHpf.a1 = 2.0 * (K * K - 1.0) / a0_;
    widthHpf.a2 = (1.0 - K / Q + K * K) / a0_;
}

double MeterEngine::applyBiquad(double input, const BiquadCoeffs& c,
                                 double& x1, double& x2, double& y1, double& y2)
{
    double output = c.b0 * input + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
    x2 = x1; x1 = input;
    y2 = y1; y1 = output;
    return output;
}

void MeterEngine::prepare(double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;
    samplesPerBlock100ms = static_cast<int>(sampleRate * 0.1);  // 100ms blocks
    computeKWeightingCoeffs(sampleRate);
    computeWidthHpfCoeffs(sampleRate);
    
    // Initialise Hann window for FFT
    for (int i = 0; i < fftSize; ++i)
        fftWindow[(size_t)i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * (float)i / (float)fftSize));
    
    // Initialise FFT accumulator
    fftAccumulator.resize((size_t)fftSize, 0.0f);
    fftWritePos = 0;
    smoothedSpectrum.fill(-100.0f);
    
    reset();
}

void MeterEngine::reset()
{
    s1x1L = s1x2L = s1y1L = s1y2L = 0;
    s1x1R = s1x2R = s1y1R = s1y2R = 0;
    s2x1L = s2x2L = s2y1L = s2y2L = 0;
    s2x1R = s2x2R = s2y1R = s2y2R = 0;
    
    kWeightedL.clear();
    kWeightedR.clear();
    blockSampleCount = 0;
    
    momentaryBlocks.clear();
    shortTermBlocks.clear();
    allBlocks.clear();
    
    sumSqL = sumSqR = 0;
    sumL = sumR = 0;
    currentPeakL = currentPeakR = 0;
    currentTpL = currentTpR = 0;
    sampleCount = 0;
    
    sumMid = sumSide = 0;
    sumCorr = sumEnergyL = sumEnergyR = 0;
    
    // FFT state
    fftData.fill(0.0f);
    std::fill(fftAccumulator.begin(), fftAccumulator.end(), 0.0f);
    fftWritePos = 0;
    smoothedSpectrum.fill(-100.0f);
    
    std::lock_guard<std::mutex> lock(dataMutex);
    data = MeterData();
}

void MeterEngine::computeTruePeak(const float* samples, int numSamples, float& truePeak)
{
    // 4x oversampling using linear interpolation (simplified but effective)
    // For production, use proper polyphase FIR filter
    float maxVal = 0.0f;
    
    for (int i = 0; i < numSamples - 1; ++i)
    {
        float s0 = std::abs(samples[i]);
        float s1 = std::abs(samples[i + 1]);
        maxVal = std::max(maxVal, s0);
        
        // 4x interpolated samples between s0 and s1
        for (int j = 1; j < 4; ++j)
        {
            float t = j / 4.0f;
            float interp = samples[i] + t * (samples[i + 1] - samples[i]);
            maxVal = std::max(maxVal, std::abs(interp));
        }
    }
    if (numSamples > 0)
        maxVal = std::max(maxVal, std::abs(samples[numSamples - 1]));
    
    truePeak = std::max(truePeak, maxVal);
}

void MeterEngine::computeSpectrum(const float* left, const float* right, int numSamples)
{
    // Accumulate mono samples into ring buffer
    for (int i = 0; i < numSamples; ++i)
    {
        float mono = (left[i] + right[i]) * 0.5f;
        fftAccumulator[(size_t)fftWritePos] = mono;
        fftWritePos = (fftWritePos + 1) % fftSize;
    }
    
    // Copy ring buffer in order, apply Hann window
    for (int i = 0; i < fftSize; ++i)
    {
        int idx = (fftWritePos + i) % fftSize;
        fftData[(size_t)i] = fftAccumulator[(size_t)idx] * fftWindow[(size_t)i];
    }
    
    // Remove DC
    double dcSum = 0.0;
    for (int i = 0; i < fftSize; ++i)
        dcSum += fftData[(size_t)i];
    float dcMean = (float)(dcSum / fftSize);
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t)i] -= dcMean;
    
    // Zero second half
    for (int i = fftSize; i < fftSize * 2; ++i)
        fftData[(size_t)i] = 0.0f;
    
    // FFT
    fft.performFrequencyOnlyForwardTransform(fftData.data(), true);
    
    // Normalise: magnitude -> dB FS
    // After performFrequencyOnlyForwardTransform the output already contains
    // magnitude values. We normalise so a full-scale sine reads 0 dB.
    float normFactor = 2.0f / (float)fftSize;
    
    // Convert all usable bins to dB once
    int usableBins = fftSize / 2;
    std::vector<float> binDb((size_t)usableBins);
    for (int k = 0; k < usableBins; ++k)
    {
        float mag = fftData[(size_t)k] * normFactor;
        binDb[(size_t)k] = mag > 1e-10f ? 20.0f * std::log10(mag) : -120.0f;
    }
    
    // Map to 64 log-spaced display bins
    // At low frequencies multiple display bins may map to the same FFT bin.
    // Use fractional interpolation so each display bin gets a distinct value.
    double binHz = currentSampleRate / (double)fftSize;
    double minFreq = 20.0;
    double maxFreq = std::min(currentSampleRate * 0.5, 20000.0);
    double logMin = std::log2(minFreq);
    double logMax = std::log2(maxFreq);
    
    constexpr int N = MeterData::numSpecBins; // 64
    std::array<float, N> rawBins = {};
    
    for (int b = 0; b < N; ++b)
    {
        double fLo = std::pow(2.0, logMin + (logMax - logMin) * (double)b / (double)N);
        double fHi = std::pow(2.0, logMin + (logMax - logMin) * (double)(b + 1) / (double)N);
        double fCentre = (fLo + fHi) * 0.5;
        
        // Fractional bin position for the centre frequency
        double exactBin = fCentre / binHz;
        int binLo = (int)std::floor(exactBin);
        int binHiI = binLo + 1;
        
        binLo = std::max(1, std::min(binLo, usableBins - 1));
        binHiI = std::max(1, std::min(binHiI, usableBins - 1));
        
        // Linearly interpolate between the two surrounding FFT bins
        float frac = (float)(exactBin - std::floor(exactBin));
        float magLo = fftData[(size_t)binLo] * normFactor;
        float magHi = fftData[(size_t)binHiI] * normFactor;
        float mag = magLo + frac * (magHi - magLo);
        
        // Also check if we span multiple bins — if so, take the max for better peak detection
        int spanLo = std::max(1, (int)std::round(fLo / binHz));
        int spanHi = std::min(usableBins - 1, (int)std::round(fHi / binHz));
        if (spanHi > spanLo)
        {
            // We span multiple bins — use peak across the range
            float peakMag = 0.0f;
            for (int k = spanLo; k <= spanHi; ++k)
            {
                float m = fftData[(size_t)k] * normFactor;
                if (m > peakMag) peakMag = m;
            }
            if (peakMag > mag) mag = peakMag;
        }
        
        rawBins[(size_t)b] = mag > 1e-10f ? 20.0f * std::log10(mag) : -120.0f;
    }
    
    // Low-frequency rolloff compensation — reduces sensitivity below ~60Hz
    // to match how MultiMeter and similar analysers display sub content.
    // Apply a steep progressive attenuation: -12dB/octave below 60Hz
    for (int b = 0; b < N; ++b)
    {
        double fCentre = std::pow(2.0, logMin + (logMax - logMin) * ((double)b + 0.5) / (double)N);
        if (fCentre < 60.0)
        {
            // Steep rolloff: -12dB/octave below 60Hz
            double octavesBelow = std::log2(60.0 / std::max(fCentre, 10.0));
            float rolloffDb = (float)(12.0 * octavesBelow);
            rawBins[(size_t)b] -= rolloffDb;
        }
        else if (fCentre < 120.0)
        {
            // Gentle transition zone: -3dB/octave from 60-120Hz
            double octavesBelow = std::log2(120.0 / fCentre);
            float rolloffDb = (float)(3.0 * octavesBelow);
            rawBins[(size_t)b] -= rolloffDb;
        }
    }
    
    // Exponential smoothing
    float attackCoeff  = 0.6f;
    float releaseCoeff = 0.12f;
    for (int b = 0; b < N; ++b)
    {
        float coeff = (rawBins[(size_t)b] > smoothedSpectrum[(size_t)b]) ? attackCoeff : releaseCoeff;
        smoothedSpectrum[(size_t)b] += coeff * (rawBins[(size_t)b] - smoothedSpectrum[(size_t)b]);
    }
    
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        data.spectrum = smoothedSpectrum;
    }
}

void MeterEngine::processBlock(const float* left, const float* right, int numSamples)
{
    // ================================================================
    // Per-block peak detection (instantaneous, with decay)
    // ================================================================
    float blockPeakL = 0, blockPeakR = 0;
    float blockTpL = 0, blockTpR = 0;
    
    computeTruePeak(left, numSamples, blockTpL);
    computeTruePeak(right, numSamples, blockTpR);
    
    // Per-block accumulators for short-window RMS + stereo
    double blkSqL = 0, blkSqR = 0, blkSumL = 0, blkSumR = 0;
    double blkMid = 0, blkSide = 0, blkCorr = 0, blkEnL = 0, blkEnR = 0;
    
    for (int i = 0; i < numSamples; ++i)
    {
        float sL = left[i];
        float sR = right[i];
        
        blkSqL += sL * sL;
        blkSqR += sR * sR;
        blkSumL += sL;
        blkSumR += sR;
        blockPeakL = std::max(blockPeakL, std::abs(sL));
        blockPeakR = std::max(blockPeakR, std::abs(sR));
        
        float mid = (sL + sR) * 0.5f;
        float side = (sL - sR) * 0.5f;
        
        // High-pass filtered L/R for width calculation (removes bass influence)
        double hpL = applyBiquad(sL, widthHpf, whpx1L, whpx2L, whpy1L, whpy2L);
        double hpR = applyBiquad(sR, widthHpf, whpx1R, whpx2R, whpy1R, whpy2R);
        float hpMid = (float)(hpL + hpR) * 0.5f;
        float hpSide = (float)(hpL - hpR) * 0.5f;
        
        blkMid += hpMid * hpMid;   // filtered mid energy for width
        blkSide += hpSide * hpSide; // filtered side energy for width
        blkCorr += (double)sL * (double)sR;  // correlation uses full signal
        blkEnL += sL * sL;
        blkEnR += sR * sR;
        
        // Goniometer: store every 8th sample pair (decimated for display)
        if ((sampleCount + i) % 8 == 0)
        {
            int gPos = data.gonioWritePos;
            data.gonioL[(size_t)gPos] = sL;
            data.gonioR[(size_t)gPos] = sR;
            data.gonioWritePos = (gPos + 1) % MeterData::gonioSize;
        }
        
        // K-weighting for LUFS
        double kL = applyBiquad(sL, kStage1, s1x1L, s1x2L, s1y1L, s1y2L);
        kL = applyBiquad(kL, kStage2, s2x1L, s2x2L, s2y1L, s2y2L);
        double kR = applyBiquad(sR, kStage1, s1x1R, s1x2R, s1y1R, s1y2R);
        kR = applyBiquad(kR, kStage2, s2x1R, s2x2R, s2y1R, s2y2R);
        
        kWeightedL.push_back(kL);
        kWeightedR.push_back(kR);
        blockSampleCount++;
        sampleCount++;
    }
    
    // ================================================================
    // Sliding-window accumulators (~300ms for responsive meters)
    // Use exponential moving average — simple and CPU-friendly
    // ================================================================
    double blkN = std::max(1.0, (double)numSamples);
    
    // Smoothing coefficient: ~300ms window at typical buffer sizes
    // alpha ≈ 1 - exp(-bufferDuration / windowTime)
    double bufDur = blkN / currentSampleRate;
    double windowTime = 0.5; // 500ms — slower for readability
    double alpha = 1.0 - std::exp(-bufDur / windowTime);
    float af = (float)alpha;
    
    // Exponential smoothing on per-block RMS energy
    sumSqL += alpha * (blkSqL / blkN - sumSqL);
    sumSqR += alpha * (blkSqR / blkN - sumSqR);
    sumL   += alpha * (blkSumL / blkN - sumL);
    sumR   += alpha * (blkSumR / blkN - sumR);
    
    // Stereo
    sumMid    += alpha * (blkMid / blkN - sumMid);
    sumSide   += alpha * (blkSide / blkN - sumSide);
    sumCorr   += alpha * (blkCorr / blkN - sumCorr);
    sumEnergyL += alpha * (blkEnL / blkN - sumEnergyL);
    sumEnergyR += alpha * (blkEnR / blkN - sumEnergyR);
    
    // Peak: fast attack, slow release (~3s fallback for readable numbers)
    float peakDecay = (float)std::exp(-bufDur / 3.0);
    currentPeakL = std::max(blockPeakL, currentPeakL * peakDecay);
    currentPeakR = std::max(blockPeakR, currentPeakR * peakDecay);
    currentTpL   = std::max(blockTpL,   currentTpL * peakDecay);
    currentTpR   = std::max(blockTpR,   currentTpR * peakDecay);
    
    // ================================================================
    // Process 100ms LUFS blocks
    // ================================================================
    while (blockSampleCount >= samplesPerBlock100ms)
    {
        double sumKL = 0, sumKR = 0;
        for (int i = 0; i < samplesPerBlock100ms; ++i)
        {
            sumKL += kWeightedL[(size_t)i] * kWeightedL[(size_t)i];
            sumKR += kWeightedR[(size_t)i] * kWeightedR[(size_t)i];
        }
        
        double blockPower = (sumKL / samplesPerBlock100ms) + (sumKR / samplesPerBlock100ms);
        double blockLoud = blockPower > 0.0 ? -0.691 + 10.0 * std::log10(blockPower) : -100.0;
        
        auto now = juce::Time::currentTimeMillis();
        LufsBlock block { blockPower, blockLoud, now };
        
        momentaryBlocks.push_back(block);
        shortTermBlocks.push_back(block);
        allBlocks.push_back(block);
        
        while (!momentaryBlocks.empty() && (now - momentaryBlocks.front().timeMs > 400))
            momentaryBlocks.erase(momentaryBlocks.begin());
        while (!shortTermBlocks.empty() && (now - shortTermBlocks.front().timeMs > 3000))
            shortTermBlocks.erase(shortTermBlocks.begin());
        
        kWeightedL.erase(kWeightedL.begin(), kWeightedL.begin() + samplesPerBlock100ms);
        kWeightedR.erase(kWeightedR.begin(), kWeightedR.begin() + samplesPerBlock100ms);
        blockSampleCount -= samplesPerBlock100ms;
    }
    
    // ================================================================
    // Compute display values from smoothed accumulators
    // ================================================================
    auto avgPower = [](const std::vector<LufsBlock>& blocks) -> double {
        if (blocks.empty()) return 0.0;
        double sum = 0;
        for (auto& b : blocks) sum += b.power;
        return sum / blocks.size();
    };
    
    auto toLoud = [](double power) -> float {
        return power > 0.0 ? static_cast<float>(-0.691 + 10.0 * std::log10(power)) : -100.0f;
    };
    
    auto toDb = [](float linear) -> float {
        return linear > 0.0f ? 20.0f * std::log10(linear) : -100.0f;
    };
    
    // Gated integrated loudness (over entire session — this IS supposed to accumulate)
    std::vector<LufsBlock> gated;
    for (auto& b : allBlocks)
        if (b.loudness > -70.0) gated.push_back(b);
    
    float lra = 0;
    if (gated.size() > 4)
    {
        std::vector<double> sorted;
        for (auto& b : gated) sorted.push_back(b.loudness);
        std::sort(sorted.begin(), sorted.end());
        lra = static_cast<float>(sorted[sorted.size() * 95 / 100] - sorted[sorted.size() * 10 / 100]);
    }
    
    // Width & correlation from smoothed accumulators (responds in real time)
    // Perceptual width: sqrt(side/mid) * 100
    // A pure mono signal = 0%, equal mid/side ≈ 100%, pure side > 100% (clamped)
    // This matches how iZotope Insight and similar tools report width
    float smRatio = (sumMid > 1e-10) ? static_cast<float>(sumSide / sumMid) : 0.0f;
    float width = std::min(100.0f, std::sqrt(smRatio) * 100.0f);
    float corr = (sumEnergyL * sumEnergyR > 0) ? static_cast<float>(sumCorr / std::sqrt(sumEnergyL * sumEnergyR)) : 0.0f;
    
    // Crest factor from smoothed RMS
    float rmsAvg = std::sqrt(static_cast<float>((sumSqL + sumSqR) * 0.5));
    float peakMax = std::max(currentPeakL, currentPeakR);
    float crest = (rmsAvg > 0) ? 20.0f * std::log10(peakMax / rmsAvg) : 0.0f;
    
    // DC offset from smoothed mean
    float dc = static_cast<float>(((sumL + sumR) * 0.5) * 1000.0);
    
    // Spectrum (FFT-based, writes to data.spectrum directly)
    computeSpectrum(left, right, numSamples);
    
    // Update data atomically
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        data.momentary = toLoud(avgPower(momentaryBlocks));
        data.shortTerm = toLoud(avgPower(shortTermBlocks));
        data.integrated = toLoud(avgPower(gated));
        data.loudnessRange = lra;
        data.rmsL = static_cast<float>(10.0 * std::log10(std::max(1e-20, sumSqL)));
        data.rmsR = static_cast<float>(10.0 * std::log10(std::max(1e-20, sumSqR)));
        data.peakL = toDb(currentPeakL);
        data.peakR = toDb(currentPeakR);
        data.truePeakL = toDb(currentTpL);
        data.truePeakR = toDb(currentTpR);
        data.truePeakMaxL = toDb(currentTpL);
        data.truePeakMaxR = toDb(currentTpR);
        data.peakMaxL = toDb(currentPeakL);
        data.peakMaxR = toDb(currentPeakR);
        data.crestFactor = crest;
        data.dcOffset = dc;
        data.width = width;
        data.correlation = corr;
        // data.spectrum already written by computeSpectrum()
        
        // Fill goniometer ring buffer (decimate to ~512 samples per display frame)
        int step = std::max(1, numSamples / 64);
        for (int i = 0; i < numSamples; i += step)
        {
            int wp = data.gonioWritePos;
            data.gonioL[(size_t)wp] = left[i];
            data.gonioR[(size_t)wp] = right[i];
            data.gonioWritePos = (wp + 1) % MeterData::gonioSize;
        }
    }
}

MeterData MeterEngine::getMeterData() const
{
    std::lock_guard<std::mutex> lock(dataMutex);
    return data;
}

void MeterEngine::resetIntegrated()
{
    std::lock_guard<std::mutex> lock(dataMutex);
    allBlocks.clear();
    momentaryBlocks.clear();
    shortTermBlocks.clear();
    data.integrated = -100.0f;
    data.loudnessRange = 0.0f;
    data.momentary = -100.0f;
    data.shortTerm = -100.0f;
}

juce::String MeterEngine::getMeterDataJSON() const
{
    auto d = getMeterData();
    
    juce::String json = "{";
    json += "\"mom\":" + juce::String(d.momentary, 1) + ",";
    json += "\"st\":" + juce::String(d.shortTerm, 1) + ",";
    json += "\"integ\":" + juce::String(d.integrated, 1) + ",";
    json += "\"range\":" + juce::String(d.loudnessRange, 1) + ",";
    json += "\"rmsL\":" + juce::String(d.rmsL, 1) + ",";
    json += "\"rmsR\":" + juce::String(d.rmsR, 1) + ",";
    json += "\"peakL\":" + juce::String(d.peakL, 1) + ",";
    json += "\"peakR\":" + juce::String(d.peakR, 1) + ",";
    json += "\"tpL\":" + juce::String(d.truePeakL, 1) + ",";
    json += "\"tpR\":" + juce::String(d.truePeakR, 1) + ",";
    json += "\"tpMaxL\":" + juce::String(d.truePeakMaxL, 1) + ",";
    json += "\"tpMaxR\":" + juce::String(d.truePeakMaxR, 1) + ",";
    json += "\"crest\":" + juce::String(d.crestFactor, 1) + ",";
    json += "\"dc\":" + juce::String(d.dcOffset, 2) + ",";
    json += "\"width\":" + juce::String(d.width, 1) + ",";
    json += "\"corr\":" + juce::String(d.correlation, 2) + ",";
    json += "\"spectrum\":[";
    for (int i = 0; i < MeterData::numSpecBins; ++i)
    {
        json += juce::String(d.spectrum[(size_t)i], 1);
        if (i < MeterData::numSpecBins - 1) json += ",";
    }
    json += "]}";
    
    return json;
}
