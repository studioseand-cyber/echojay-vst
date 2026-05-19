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

// Compute biquad coefficients for the three banded correlation filters:
// LPF 120Hz (sub), HPF 5kHz (top), and HPF 120Hz + LPF 5kHz cascaded (mid).
// All 2nd-order Butterworth (Q ~ 0.7071). The mid band uses two filters
// in cascade rather than a true band-pass biquad. Simpler maintenance,
// and the phase quirks of cascaded HP+LP don't matter for correlation.
void MeterEngine::computeBandedCorrCoeffs(double sr)
{
    const double Q = 0.7071;
    auto computeLpf = [sr, Q](double f0) -> BiquadCoeffs {
        BiquadCoeffs c;
        double K = std::tan(juce::MathConstants<double>::pi * f0 / sr);
        double a0_ = 1.0 + K / Q + K * K;
        c.b0 = (K * K) / a0_;
        c.b1 = 2.0 * (K * K) / a0_;
        c.b2 = (K * K) / a0_;
        c.a1 = 2.0 * (K * K - 1.0) / a0_;
        c.a2 = (1.0 - K / Q + K * K) / a0_;
        return c;
    };
    auto computeHpf = [sr, Q](double f0) -> BiquadCoeffs {
        BiquadCoeffs c;
        double K = std::tan(juce::MathConstants<double>::pi * f0 / sr);
        double a0_ = 1.0 + K / Q + K * K;
        c.b0 = 1.0 / a0_;
        c.b1 = -2.0 / a0_;
        c.b2 = 1.0 / a0_;
        c.a1 = 2.0 * (K * K - 1.0) / a0_;
        c.a2 = (1.0 - K / Q + K * K) / a0_;
        return c;
    };
    corrSubLpf = computeLpf(120.0);
    corrTopHpf = computeHpf(5000.0);
    corrMidHpf = computeHpf(120.0);
    corrMidLpf = computeLpf(5000.0);
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
    // 500ms of silence at the host sample rate flips isSilent to true.
    // Long enough to ride out short gaps between phrases, short enough
    // that a stopped transport reads as silent before the user sends
    // a follow-up message.
    silenceTimeoutSamples = (int)(sampleRate * 0.5);
    silentSampleCount.store(silenceTimeoutSamples + 1); // start as silent

    currentSampleRate = sampleRate;
    samplesPerBlock100ms = static_cast<int>(sampleRate * 0.1);
    computeKWeightingCoeffs(sampleRate);
    computeWidthHpfCoeffs(sampleRate);
    computeBandedCorrCoeffs(sampleRate);
    
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
    // Just set the flag — actual reset happens on the audio thread at the
    // top of the next processBlock. This avoids a data race where the message
    // thread clears the LufsBlock vectors while the audio thread is mid-block
    // appending to them (which produced intermittent garbage LUFS readings).
    pendingReset.store(true);
}

void MeterEngine::resetState()
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
    displayWidth = 0.0f;
    displayCorr = 0.0f;
    displayStereoInit = false;
    // Banded correlation filter states
    subLpxL1=subLpxL2=subLpyL1=subLpyL2=0;
    subLpxR1=subLpxR2=subLpyR1=subLpyR2=0;
    topHpxL1=topHpxL2=topHpyL1=topHpyL2=0;
    topHpxR1=topHpxR2=topHpyR1=topHpyR2=0;
    midHpxL1=midHpxL2=midHpyL1=midHpyL2=0;
    midHpxR1=midHpxR2=midHpyR1=midHpyR2=0;
    midLpxL1=midLpxL2=midLpyL1=midLpyL2=0;
    midLpxR1=midLpxR2=midLpyR1=midLpyR2=0;
    // Banded display values and init flags
    displayCorrSub = displayCorrMid = displayCorrTop = 0.0f;
    subInit = midInit = topInit = false;
    // Side/mid display value
    displaySideToMid = 0.0f;
    sideToMidInit = false;
    lastCrest = 0.0f;
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
    // Time-based smoothing — consistent across buffer sizes and platforms
    double bufDur = (currentSampleRate > 0) ? (double)numSamples / currentSampleRate : 0.01;
    float attackCoeff = 1.0f - (float)std::exp(-bufDur / 0.01);  // ~10ms attack
    float releaseCoeff = 1.0f - (float)std::exp(-bufDur / 0.15); // ~150ms release
    for (int b = 0; b < N; ++b)
    {
        float coeff = (rawBins[(size_t)b] > smoothedSpectrum[(size_t)b]) ? attackCoeff : releaseCoeff;
        smoothedSpectrum[(size_t)b] += coeff * (rawBins[(size_t)b] - smoothedSpectrum[(size_t)b]);
    }
    { std::lock_guard<std::mutex> lock(dataMutex); data.spectrum = smoothedSpectrum; }
}

void MeterEngine::processBlock(const float* left, const float* right, int numSamples)
{
    // Run any pending reset BEFORE touching any state. This ensures state is
    // clean and consistent before we start writing to it. Cleared atomically
    // so a second reset() from message thread during this block sets it again
    // and gets handled on the NEXT block — which is fine.
    if (pendingReset.exchange(false))
        resetState();

    // ===== Silence detection: are we receiving audio right now? =====
    // Scan the block for any sample above kSilenceThreshold. If found,
    // reset the silent-sample counter. If not, accumulate. The chat
    // layer uses the resulting isSilent flag to decide whether to send
    // stale meter values to the model on follow-up messages.
    {
        float blockPeakAny = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            float a = std::abs(left[i]);
            float b = std::abs(right[i]);
            if (a > blockPeakAny) blockPeakAny = a;
            if (b > blockPeakAny) blockPeakAny = b;
        }
        if (blockPeakAny > kSilenceThreshold) {
            silentSampleCount.store(0);
        } else {
            silentSampleCount.fetch_add(numSamples);
        }
    }

    float blockPeakL = 0, blockPeakR = 0;
    float blockTpL = 0, blockTpR = 0;
    computeTruePeak(left, numSamples, blockTpL);
    computeTruePeak(right, numSamples, blockTpR);
    
    double blkSqL = 0, blkSqR = 0, blkSumL = 0, blkSumR = 0;
    double blkMid = 0, blkSide = 0, blkCorr = 0, blkEnL = 0, blkEnR = 0;

    // Per-buffer banded correlation accumulators. Same pattern as the
    // overall blkCorr/blkEnL/blkEnR: sum of LR product, sum of L², sum of R²
    // for each of the three bands. Per-buffer (not per-block) because the
    // existing correlation does the same and the EMA at the buffer boundary
    // already handles smoothing.
    double subCorrSum=0, subEnL=0, subEnR=0;
    double midCorrSum=0, midEnL=0, midEnR=0;
    double topCorrSum=0, topEnL=0, topEnR=0;

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

        // ===== Banded correlation: filter each band, accumulate LR cross =====
        // Sub (LPF 120Hz)
        double subL = applyBiquad(sL, corrSubLpf, subLpxL1, subLpxL2, subLpyL1, subLpyL2);
        double subR = applyBiquad(sR, corrSubLpf, subLpxR1, subLpxR2, subLpyR1, subLpyR2);
        subCorrSum += subL * subR;
        subEnL += subL * subL; subEnR += subR * subR;
        // Mid (HPF 120Hz then LPF 5kHz, cascaded)
        double midL = applyBiquad(sL, corrMidHpf, midHpxL1, midHpxL2, midHpyL1, midHpyL2);
        midL = applyBiquad(midL, corrMidLpf, midLpxL1, midLpxL2, midLpyL1, midLpyL2);
        double midR = applyBiquad(sR, corrMidHpf, midHpxR1, midHpxR2, midHpyR1, midHpyR2);
        midR = applyBiquad(midR, corrMidLpf, midLpxR1, midLpxR2, midLpyR1, midLpyR2);
        midCorrSum += midL * midR;
        midEnL += midL * midL; midEnR += midR * midR;
        // Top (HPF 5kHz)
        double topL = applyBiquad(sL, corrTopHpf, topHpxL1, topHpxL2, topHpyL1, topHpyL2);
        double topR = applyBiquad(sR, corrTopHpf, topHpxR1, topHpxR2, topHpyR1, topHpyR2);
        topCorrSum += topL * topR;
        topEnL += topL * topL; topEnR += topR * topR;
        
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
    
    // Width and correlation: compute INSTANTANEOUS values from this buffer's
    // mid/side and L/R cross-product, then smooth the resulting scalar with a
    // 1.5s EMA. Smoothing the components independently (sumMid, sumSide etc)
    // and then dividing produces wild swings on signals where mid and side
    // energies change on different timescales — e.g. vocal reverb tails, where
    // mid is decaying as the dry signal stops while side is rising as the
    // reverb spreads. Smoothing the ratio gives a calm, useful reading.
    //
    // We also GATE the smoothing so that silent buffers don't update the EMA
    // at all — silence contains no meaningful stereo information and would
    // otherwise produce wild spikes (gaps between words, reverb-only sections).
    float instWidth = 0.0f;
    float instCorr = 0.0f;
    bool stereoBufHasSignal = false;
    {
        if (blkMid > 1e-10)
        {
            double r = blkSide / blkMid;
            instWidth = std::min(100.0f, static_cast<float>(std::sqrt(r) * 100.0));
        }
        if (blkEnL > 1e-10 && blkEnR > 1e-10)
            instCorr = static_cast<float>(blkCorr / std::sqrt(blkEnL * blkEnR));
        // Gate: this buffer has audible signal if the louder channel's peak
        // is above a sensible threshold. Below that, hold the previous EMA value
        // rather than letting silence pollute the reading.
        float bufPeak = std::max(blockPeakL, blockPeakR);
        stereoBufHasSignal = (bufPeak > 0.003162f);  // -50 dBFS
    }
    
    // 1.5-second EMA on the scalar values, but only update during audible signal
    double widthAlpha = 1.0 - std::exp(-bufDur / 1.5);
    if (stereoBufHasSignal)
    {
        if (!displayStereoInit)
        {
            displayWidth = instWidth;
            displayCorr = instCorr;
            displayStereoInit = true;
        }
        else
        {
            displayWidth += static_cast<float>(widthAlpha * (instWidth - displayWidth));
            displayCorr += static_cast<float>(widthAlpha * (instCorr - displayCorr));
        }
    }
    // else: hold the last meaningful values — silence has no stereo info

    // ===== Banded correlation: instantaneous values, then EMA with the same
    // overall gate. Each band has its own init flag because bands can fill
    // at different times (e.g. a sub-only intro has no top-band signal).
    // We use the same overall bufPeak gate for EMA update because gating
    // per-band creates inconsistent timestamps across the readings, which
    // makes the relationship between them less useful for the model.
    auto bandedCorr = [](double cross, double enL, double enR) -> float {
        if (enL > 1e-10 && enR > 1e-10)
            return static_cast<float>(cross / std::sqrt(enL * enR));
        return 0.0f;
    };
    float instCorrSub = bandedCorr(subCorrSum, subEnL, subEnR);
    float instCorrMid = bandedCorr(midCorrSum, midEnL, midEnR);
    float instCorrTop = bandedCorr(topCorrSum, topEnL, topEnR);

    // Band-specific "has signal" gates: only update each band's EMA when
    // there's meaningful energy in THAT band. Same threshold as the overall
    // gate, but applied to the band-filtered energy. This prevents the
    // bass-only intro case from settling top correlation to a meaningless
    // value picked up from filter ringing during the loud first kick.
    const double bandEnergyGate = 1e-7;  // ~ -70 dBFS RMS in the band
    bool subHasSig = (subEnL + subEnR) > bandEnergyGate;
    bool midHasSig = (midEnL + midEnR) > bandEnergyGate;
    bool topHasSig = (topEnL + topEnR) > bandEnergyGate;

    auto updateEma = [widthAlpha](float& display, bool& init, float inst, bool hasSig) {
        if (!hasSig) return;
        if (!init) { display = inst; init = true; }
        else       { display += static_cast<float>(widthAlpha * (inst - display)); }
    };
    updateEma(displayCorrSub, subInit, instCorrSub, subHasSig);
    updateEma(displayCorrMid, midInit, instCorrMid, midHasSig);
    updateEma(displayCorrTop, topInit, instCorrTop, topHasSig);

    // ===== Side/mid ratio: derived from the SAME HPF'd mid/side accumulators
    // that drive width. Width is sqrt(side/mid)*100 (a percentage), this is
    // the raw ratio side/mid. Different numerical shape, same underlying
    // measurement. Gated and smoothed identically to width.
    float instSideToMid = 0.0f;
    if (blkMid > 1e-10)
        instSideToMid = static_cast<float>(blkSide / blkMid);
    if (stereoBufHasSignal)
    {
        if (!sideToMidInit) { displaySideToMid = instSideToMid; sideToMidInit = true; }
        else                { displaySideToMid += static_cast<float>(widthAlpha * (instSideToMid - displaySideToMid)); }
    }

    float width = displayWidth;
    float corr = displayCorr;
    float rmsAvg = std::sqrt(static_cast<float>((sumSqL + sumSqR) * 0.5));
    float peakMax = std::max(currentPeakL, currentPeakR);
    // Crest: peak/RMS in dB. Freeze when signal is inaudibly quiet, otherwise
    // crest grows without bound as RMS decays toward zero on stopped playback
    // (peak holds, RMS doesn't, so the ratio explodes). Below -60 dBFS peak
    // there's nothing useful to report.
    float crest = 0.0f;
    if (peakMax > 0.001f && rmsAvg > 0.0f)
    {
        crest = 20.0f * std::log10(peakMax / rmsAvg);
        // Sanity clamp — real-world crest is 3-30 dB; outside that means the
        // measurement is unreliable (numerical edge case).
        crest = juce::jlimit(0.0f, 40.0f, crest);
        lastCrest = crest;
    }
    else
    {
        // Silence: hold previous crest value rather than reporting nonsense
        crest = lastCrest;
    }
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
        data.sideToMidRatio = displaySideToMid;
        data.corrSub = displayCorrSub;
        data.corrMid = displayCorrMid;
        data.corrTop = displayCorrTop;
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
    // Return a copy of data with the silence flag set from the atomic
    // counter. getMeterData() is const, so we can't modify the member
    // 'data' directly. Copying is cheap and avoids needing to make
    // isSilent mutable.
    MeterData result = data;
    result.isSilent = silentSampleCount.load() > silenceTimeoutSamples;
    return result;
}

void MeterEngine::resetIntegrated()
{
    // Same race risk as reset() — defer to audio thread by setting the flag.
    // The full state reset will clear integrated LUFS along with everything
    // else, which is what the user wanted anyway.
    pendingReset.store(true);
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
    json += "\"sideMid\":" + juce::String(d.sideToMidRatio, 2) + ",";
    json += "\"corrSub\":" + juce::String(d.corrSub, 2) + ",";
    json += "\"corrMid\":" + juce::String(d.corrMid, 2) + ",";
    json += "\"corrTop\":" + juce::String(d.corrTop, 2) + ",";
    json += "\"spectrum\":[";
    for (int i = 0; i < MeterData::numSpecBins; ++i)
    {
        json += juce::String(d.spectrum[(size_t)i], 1);
        if (i < MeterData::numSpecBins - 1) json += ",";
    }
    json += "]}";
    return json;
}
