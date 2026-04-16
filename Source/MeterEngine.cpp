#include "MeterEngine.h"
#include <cmath>
#include <algorithm>
#include <numeric>

MeterEngine::MeterEngine() {}
MeterEngine::~MeterEngine() {}

void MeterEngine::computeKWeightingCoeffs(double sr)
{
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
    double f0 = 150.0;
    double Q = 0.7071;
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
    samplesPerBlock100ms = static_cast<int>(sampleRate * 0.1);
    computeKWeightingCoeffs(sampleRate);
    computeWidthHpfCoeffs(sampleRate);
    
    for (int i = 0; i < fftSize; ++i)
        fftWindow[(size_t)i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * (float)i / (float)fftSize));
    
    fftAccumulator.resize((size_t)fftSize, 0.0f);
    fftWritePos = 0;
    smoothedSpectrum.fill(-100.0f);
    
    // Momentary ring buffer: 400ms of samples per EBU R128
    kPowerRingSize = static_cast<int>(sampleRate * 0.4);
    kPowerRing.resize((size_t)kPowerRingSize, 0.0);
    kPowerWritePos = 0;
    kPowerSum = 0;
    kPowerCount = 0;
    
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
    shortTermHistory.clear();
    if (!kPowerRing.empty()) std::fill(kPowerRing.begin(), kPowerRing.end(), 0.0);
    kPowerWritePos = 0;
    kPowerSum = 0;
    kPowerCount = 0;
    sumSqL = sumSqR = 0;
    sumL = sumR = 0;
    currentPeakL = currentPeakR = 0;
    currentTpL = currentTpR = 0;
    sampleCount = 0;
    sumMid = sumSide = 0;
    sumCorr = sumEnergyL = sumEnergyR = 0;
    fftData.fill(0.0f);
    std::fill(fftAccumulator.begin(), fftAccumulator.end(), 0.0f);
    fftWritePos = 0;
    smoothedSpectrum.fill(-100.0f);
    std::lock_guard<std::mutex> lock(dataMutex);
    data = MeterData();
}

// ============================================================================
// True Peak — Catmull-Rom 4x oversampling for inter-sample peak detection
// ============================================================================
void MeterEngine::computeTruePeak(const float* samples, int numSamples, float& truePeak)
{
    for (int i = 0; i < numSamples; ++i)
    {
        float absVal = std::abs(samples[i]);
        if (absVal > truePeak) truePeak = absVal;
        
        if (i < numSamples - 1)
        {
            float y0 = (i > 0) ? samples[i - 1] : samples[i];
            float y1 = samples[i];
            float y2 = samples[i + 1];
            float y3 = (i < numSamples - 2) ? samples[i + 2] : samples[i + 1];
            
            for (int j = 1; j <= 3; ++j)
            {
                float t = (float)j / 4.0f;
                float t2 = t * t;
                float t3 = t2 * t;
                float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
                float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                float a2 = -0.5f * y0 + 0.5f * y2;
                float a3 = y1;
                float interp = a0 * t3 + a1 * t2 + a2 * t + a3;
                absVal = std::abs(interp);
                if (absVal > truePeak) truePeak = absVal;
            }
        }
    }
}

void MeterEngine::computeSpectrum(const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        float mono = (left[i] + right[i]) * 0.5f;
        fftAccumulator[(size_t)fftWritePos] = mono;
        fftWritePos = (fftWritePos + 1) % fftSize;
    }
    for (int i = 0; i < fftSize; ++i)
    {
        int idx = (fftWritePos + i) % fftSize;
        fftData[(size_t)i] = fftAccumulator[(size_t)idx] * fftWindow[(size_t)i];
    }
    double dcSum = 0.0;
    for (int i = 0; i < fftSize; ++i) dcSum += fftData[(size_t)i];
    float dcMean = (float)(dcSum / fftSize);
    for (int i = 0; i < fftSize; ++i) fftData[(size_t)i] -= dcMean;
    for (int i = fftSize; i < fftSize * 2; ++i) fftData[(size_t)i] = 0.0f;
    
    fft.performFrequencyOnlyForwardTransform(fftData.data(), true);
    float normFactor = 2.0f / (float)fftSize;
    int usableBins = fftSize / 2;
    double binHz = currentSampleRate / (double)fftSize;
    double minFreq = 20.0;
    double maxFreq = std::min(currentSampleRate * 0.5, 20000.0);
    double logMin = std::log2(minFreq);
    double logMax = std::log2(maxFreq);
    constexpr int N = MeterData::numSpecBins;
    std::array<float, N> rawBins = {};
    
    for (int b = 0; b < N; ++b)
    {
        double fLo = std::pow(2.0, logMin + (logMax - logMin) * (double)b / (double)N);
        double fHi = std::pow(2.0, logMin + (logMax - logMin) * (double)(b + 1) / (double)N);
        double fCentre = (fLo + fHi) * 0.5;
        double exactBin = fCentre / binHz;
        int binLo2 = std::max(1, std::min((int)std::floor(exactBin), usableBins - 1));
        int binHi2 = std::max(1, std::min(binLo2 + 1, usableBins - 1));
        float frac = (float)(exactBin - std::floor(exactBin));
        float magLo = fftData[(size_t)binLo2] * normFactor;
        float magHi = fftData[(size_t)binHi2] * normFactor;
        float mag = magLo + frac * (magHi - magLo);
        int spanLo2 = std::max(1, (int)std::round(fLo / binHz));
        int spanHi2 = std::min(usableBins - 1, (int)std::round(fHi / binHz));
        if (spanHi2 > spanLo2)
        {
            float peakMag = 0.0f;
            for (int k = spanLo2; k <= spanHi2; ++k)
            {
                float m = fftData[(size_t)k] * normFactor;
                if (m > peakMag) peakMag = m;
            }
            if (peakMag > mag) mag = peakMag;
        }
        rawBins[(size_t)b] = mag > 1e-10f ? 20.0f * std::log10(mag) : -120.0f;
    }
    for (int b = 0; b < N; ++b)
    {
        double fCentre = std::pow(2.0, logMin + (logMax - logMin) * ((double)b + 0.5) / (double)N);
        if (fCentre < 60.0)
        {
            double octavesBelow = std::log2(60.0 / std::max(fCentre, 10.0));
            rawBins[(size_t)b] -= (float)(12.0 * octavesBelow);
        }
        else if (fCentre < 120.0)
        {
            double octavesBelow = std::log2(120.0 / fCentre);
            rawBins[(size_t)b] -= (float)(3.0 * octavesBelow);
        }
    }
    float attackCoeff = 0.6f, releaseCoeff = 0.12f;
    for (int b = 0; b < N; ++b)
    {
        float coeff = (rawBins[(size_t)b] > smoothedSpectrum[(size_t)b]) ? attackCoeff : releaseCoeff;
        smoothedSpectrum[(size_t)b] += coeff * (rawBins[(size_t)b] - smoothedSpectrum[(size_t)b]);
    }
    { std::lock_guard<std::mutex> lock(dataMutex); data.spectrum = smoothedSpectrum; }
}

void MeterEngine::processBlock(const float* left, const float* right, int numSamples)
{
    float blockPeakL = 0, blockPeakR = 0;
    float blockTpL = 0, blockTpR = 0;
    computeTruePeak(left, numSamples, blockTpL);
    computeTruePeak(right, numSamples, blockTpR);
    
    double blkSqL = 0, blkSqR = 0, blkSumL = 0, blkSumR = 0;
    double blkMid = 0, blkSide = 0, blkCorr = 0, blkEnL = 0, blkEnR = 0;
    
    for (int i = 0; i < numSamples; ++i)
    {
        float sL = left[i]; float sR = right[i];
        blkSqL += sL * sL; blkSqR += sR * sR;
        blkSumL += sL; blkSumR += sR;
        blockPeakL = std::max(blockPeakL, std::abs(sL));
        blockPeakR = std::max(blockPeakR, std::abs(sR));
        
        double hpL = applyBiquad(sL, widthHpf, whpx1L, whpx2L, whpy1L, whpy2L);
        double hpR = applyBiquad(sR, widthHpf, whpx1R, whpx2R, whpy1R, whpy2R);
        float hpMid = (float)(hpL + hpR) * 0.5f;
        float hpSide = (float)(hpL - hpR) * 0.5f;
        blkMid += hpMid * hpMid; blkSide += hpSide * hpSide;
        blkCorr += (double)sL * (double)sR;
        blkEnL += sL * sL; blkEnR += sR * sR;
        
        double kL = applyBiquad(sL, kStage1, s1x1L, s1x2L, s1y1L, s1y2L);
        kL = applyBiquad(kL, kStage2, s2x1L, s2x2L, s2y1L, s2y2L);
        double kR = applyBiquad(sR, kStage1, s1x1R, s1x2R, s1y1R, s1y2R);
        kR = applyBiquad(kR, kStage2, s2x1R, s2x2R, s2y1R, s2y2R);
        kWeightedL.push_back(kL);
        kWeightedR.push_back(kR);
        
        // Momentary ring buffer — running sum for instant 400ms LUFS
        double kPower = kL * kL + kR * kR;
        if (kPowerRingSize > 0) {
            kPowerSum -= kPowerRing[(size_t)kPowerWritePos]; // remove oldest
            kPowerRing[(size_t)kPowerWritePos] = kPower;
            kPowerSum += kPower; // add newest
            kPowerWritePos = (kPowerWritePos + 1) % kPowerRingSize;
            if (kPowerCount < kPowerRingSize) kPowerCount++;
        }
        
        blockSampleCount++;
        sampleCount++;
    }
    
    double blkN = std::max(1.0, (double)numSamples);
    double bufDur = blkN / currentSampleRate;
    double alpha = 1.0 - std::exp(-bufDur / 0.5);
    
    sumSqL += alpha * (blkSqL / blkN - sumSqL);
    sumSqR += alpha * (blkSqR / blkN - sumSqR);
    sumL += alpha * (blkSumL / blkN - sumL);
    sumR += alpha * (blkSumR / blkN - sumR);
    sumMid += alpha * (blkMid / blkN - sumMid);
    sumSide += alpha * (blkSide / blkN - sumSide);
    sumCorr += alpha * (blkCorr / blkN - sumCorr);
    sumEnergyL += alpha * (blkEnL / blkN - sumEnergyL);
    sumEnergyR += alpha * (blkEnR / blkN - sumEnergyR);
    
    float peakDecay = (float)std::exp(-bufDur / 3.0);
    currentPeakL = std::max(blockPeakL, currentPeakL * peakDecay);
    currentPeakR = std::max(blockPeakR, currentPeakR * peakDecay);
    // True peak: HOLD maximum (no decay) — reset only on resetIntegrated
    currentTpL = std::max(blockTpL, currentTpL);
    currentTpR = std::max(blockTpR, currentTpR);
    // Decaying display value for bar animation
    displayTpL = std::max(blockTpL, displayTpL * peakDecay);
    displayTpR = std::max(blockTpR, displayTpR * peakDecay);
    
    // 100ms LUFS blocks (sample-counted)
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
        
        LufsBlock block { blockPower, blockLoud, (int64_t)allBlocks.size() };
        momentaryBlocks.push_back(block);
        shortTermBlocks.push_back(block);
        allBlocks.push_back(block);
        
        // Momentary: 400ms = 4 x 100ms blocks
        while (momentaryBlocks.size() > 4)
            momentaryBlocks.erase(momentaryBlocks.begin());
        // Short-term: 3s = 30 x 100ms blocks
        while (shortTermBlocks.size() > 30)
            shortTermBlocks.erase(shortTermBlocks.begin());
        
        // Record short-term loudness for LRA every 100ms (overlapping 3s windows)
        // Insight updates this frequently — gating keeps the value accurate
        if (shortTermBlocks.size() >= 30)
        {
            double stPower = 0;
            for (auto& sb : shortTermBlocks) stPower += sb.power;
            stPower /= shortTermBlocks.size();
            double stLoud = stPower > 0.0 ? -0.691 + 10.0 * std::log10(stPower) : -100.0;
            shortTermHistory.push_back(stLoud);
        }
        
        kWeightedL.erase(kWeightedL.begin(), kWeightedL.begin() + samplesPerBlock100ms);
        kWeightedR.erase(kWeightedR.begin(), kWeightedR.begin() + samplesPerBlock100ms);
        blockSampleCount -= samplesPerBlock100ms;
    }
    
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
    
    // ================================================================
    // EBU R128 Integrated — TWO-PASS GATING
    // Pass 1: absolute gate at -70 LUFS
    // Pass 2: relative gate at -10 LU below absolute-gated mean
    // ================================================================
    std::vector<LufsBlock> absGated;
    for (auto& b : allBlocks)
        if (b.loudness > -70.0) absGated.push_back(b);
    
    float integratedLufs = -100.0f;
    float lra = 0.0f;
    
    if (!absGated.empty())
    {
        double absGatedPower = 0.0;
        for (auto& b : absGated) absGatedPower += b.power;
        absGatedPower /= absGated.size();
        double absGatedLoud = absGatedPower > 0.0 ? -0.691 + 10.0 * std::log10(absGatedPower) : -100.0;
        
        // Pass 2: relative gate
        double relThreshold = absGatedLoud - 10.0;
        std::vector<LufsBlock> relGated;
        for (auto& b : absGated)
            if (b.loudness > relThreshold) relGated.push_back(b);
        
        if (!relGated.empty())
        {
            double relPower = 0.0;
            for (auto& b : relGated) relPower += b.power;
            relPower /= relGated.size();
            integratedLufs = toLoud(relPower);
        }
        
        // LRA (EBU R128): uses SHORT-TERM loudness distribution (3s windows)
        // Absolute gate -70 LUFS, relative gate -20 LU, then 10th-95th percentile
        if (shortTermHistory.size() > 30)
        {
            // Absolute gate
            std::vector<double> lraAbsGated;
            for (auto v : shortTermHistory)
                if (v > -70.0) lraAbsGated.push_back(v);
            
            if (lraAbsGated.size() > 10)
            {
                // Mean of absolute-gated
                double lraMean = 0;
                for (auto v : lraAbsGated) lraMean += v;
                lraMean /= lraAbsGated.size();
                
                // Relative gate at -20 LU
                double lraRelThreshold = lraMean - 20.0;
                std::vector<double> lraSorted;
                for (auto v : lraAbsGated)
                    if (v > lraRelThreshold) lraSorted.push_back(v);
                
                if (lraSorted.size() > 4)
                {
                    std::sort(lraSorted.begin(), lraSorted.end());
                    size_t lo = lraSorted.size() * 10 / 100;
                    size_t hi = std::min(lraSorted.size() * 95 / 100, lraSorted.size() - 1);
                    lra = static_cast<float>(lraSorted[hi] - lraSorted[lo]);
                }
            }
        }
    }
    
    float smRatio = (sumMid > 1e-10) ? static_cast<float>(sumSide / sumMid) : 0.0f;
    float width = std::min(100.0f, std::sqrt(smRatio) * 100.0f);
    float corr = (sumEnergyL * sumEnergyR > 0) ? static_cast<float>(sumCorr / std::sqrt(sumEnergyL * sumEnergyR)) : 0.0f;
    float rmsAvg = std::sqrt(static_cast<float>((sumSqL + sumSqR) * 0.5));
    float peakMax = std::max(currentPeakL, currentPeakR);
    float crest = (rmsAvg > 0) ? 20.0f * std::log10(peakMax / rmsAvg) : 0.0f;
    float dc = static_cast<float>(((sumL + sumR) * 0.5) * 1000.0);
    
    computeSpectrum(left, right, numSamples);
    
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        // Momentary: instant 400ms from ring buffer with peak hold
        if (kPowerCount > 0) {
            double momPower = kPowerSum / kPowerCount;
            float instantMom = momPower > 0.0 ? static_cast<float>(-0.691 + 10.0 * std::log10(momPower)) : -100.0f;
            // Fast attack, moderate release
            if (instantMom > data.momentary)
                data.momentary = instantMom;
            else {
                float decay = 20.0f * (float)(numSamples / currentSampleRate);
                data.momentary = std::max(instantMom, data.momentary - decay);
            }
        } else {
            data.momentary = -100.0f;
        }
        data.shortTerm = toLoud(avgPower(shortTermBlocks));
        data.integrated = integratedLufs;
        data.loudnessRange = lra;
        data.rmsL = static_cast<float>(10.0 * std::log10(std::max(1e-20, sumSqL)));
        data.rmsR = static_cast<float>(10.0 * std::log10(std::max(1e-20, sumSqR)));
        data.peakL = toDb(currentPeakL);
        data.peakR = toDb(currentPeakR);
        data.truePeakL = toDb(currentTpL);
        data.truePeakR = toDb(currentTpR);
        data.truePeakBarL = toDb(displayTpL);
        data.truePeakBarR = toDb(displayTpR);
        data.truePeakMaxL = toDb(currentTpL);
        data.truePeakMaxR = toDb(currentTpR);
        data.peakMaxL = toDb(currentPeakL);
        data.peakMaxR = toDb(currentPeakR);
        data.crestFactor = crest;
        data.dcOffset = dc;
        data.width = width;
        data.correlation = corr;
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
    shortTermHistory.clear();
    currentTpL = 0; currentTpR = 0;
    displayTpL = 0; displayTpR = 0;
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
