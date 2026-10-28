#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "EJBandScheme.h"

// =============================================================================
//  MA10's HOP SERIES, OFFLINE AND PROVISIONAL.
// =============================================================================
//
// C2 section 5's hop accumulator is A's to build and does not exist yet (it lands
// after k-base-1). MA10 and the block-size crest pin both need a hop series from
// real audio tonight, so this reproduces MeterEngine's perOct path as it stands
// at cea66b4e and groups it into sample-counted 100 ms hops. RERUN ON A's
// ACCUMULATOR when it exists: this is a copy, and a copy can drift.
//
// WHAT IS REPRODUCED, with the MeterEngine.cpp lines it copies:
//   :266-269  mono = (L + R) * 0.5f into a 2048-sample circular accumulator
//   :117-118  a full-length Hann, 0.5 * (1 - cos (2 pi i / N)), in float
//   :278-282  the most recent 2048 samples, oldest first, times the window
//   :283-286  the mean of the WINDOWED frame subtracted (DC removed after the
//             window, which is what the engine does)
//   :289      performFrequencyOnlyForwardTransform (data, true)
//   :290-294  normFactor 2/2048, binHz = sr/2048, maxFreq = min (sr/2, 20000)
//   :361,368-381  bins 1..1023 with 20 <= f < maxFreq, m = mag * normFactor,
//             band power += m * m by EJBandScheme's edges, then perOct = power
//             / log2 (max (1.01, hi/lo))
//   :556-572, MeterEngine.h:340, :617  silence: a block with any sample over
//             0.0001 resets the counter, else adds numSamples; silent once the
//             counter exceeds 0.5 s of samples; it STARTS silent (:87)
//   :111, :708-750  the hop: sampleRate * 0.1 samples, sample-counted
//
// ONE FRAME PER BLOCK, AND THE ORDER INSIDE A BLOCK. computeSpectrum runs once
// per processBlock (:968) with no hop gate, and the 100 ms while-loop (:708)
// runs BEFORE it in the same call. So a block that crosses a hop boundary first
// closes the old hop with the frames it already holds, then its own frame goes
// into the new hop. A's accumulator may choose otherwise; this is the order the
// engine's lines give today.
//
// NOT REPRODUCED: short-term LUFS and true peak. MA10 reads crest, and crest
// uses bandPower only (C4 section 6), so those two fields stay at -100.
// =============================================================================

namespace ma10
{

/** C2 section 5's fields, by name, so EJMatchWindow.h's templates read it.
    Deliberately not named HopFrame: that type is A's. */
struct Hop
{
    float shortTermLufs = -100.0f;
    float shortTermTruePeak = -100.0f;
    std::array<float, 6> bandPower {};
    std::array<float, 6> bandPeak {};
    std::uint16_t bandFrames = 0;
    std::uint16_t flags = 0;
};

class HopSim
{
public:
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder;   // 2048
    static constexpr float kSilenceThreshold = 0.0001f;

    explicit HopSim (double sampleRate)
        : sr (sampleRate), fft (fftOrder)
    {
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                          * (float) i / (float) fftSize));
        samplesPerHop  = (int) (sampleRate * 0.1);
        silenceTimeout = (int) (sampleRate * 0.5);
        silentCount    = silenceTimeout + 1;   // starts silent, as the engine does
    }

    /** One processBlock's worth. */
    void process (const float* left, const float* right, int n)
    {
        // Silence first, as processBlock does (:556), so isSilentNow() inside
        // computeSpectrum sees this block.
        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
            pk = std::max (pk, std::max (std::abs (left[i]), std::abs (right[i])));
        if (pk > kSilenceThreshold) silentCount = 0; else silentCount += n;

        // The hop boundary, before the spectrum (:708 precedes :968).
        hopSamples += n;
        while (hopSamples >= samplesPerHop)
        {
            closeHop();
            hopSamples -= samplesPerHop;
        }

        computeFrame (left, right, n);
    }

    const std::vector<Hop>& hops() const noexcept { return out; }

private:
    void closeHop()
    {
        Hop h;
        if (frames > 0)
            for (size_t b = 0; b < 6; ++b)
            {
                h.bandPower[b] = (float) (sum[b] / (double) frames);
                h.bandPeak[b]  = (float) peak[b];
            }
        h.bandFrames = (std::uint16_t) std::min (frames, 65535);
        h.flags = (frames > 0 && allSilent) ? 1 : 0;
        out.push_back (h);
        sum.fill (0.0); peak.fill (0.0); frames = 0; allSilent = true;
    }

    void computeFrame (const float* left, const float* right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            acc[(size_t) writePos] = (left[i] + right[i]) * 0.5f;
            writePos = (writePos + 1) % fftSize;
        }
        for (int i = 0; i < fftSize; ++i)
            data[(size_t) i] = acc[(size_t) ((writePos + i) % fftSize)] * window[(size_t) i];
        double dcSum = 0.0;
        for (int i = 0; i < fftSize; ++i) dcSum += data[(size_t) i];
        const float dcMean = (float) (dcSum / fftSize);
        for (int i = 0; i < fftSize; ++i) data[(size_t) i] -= dcMean;
        for (int i = fftSize; i < fftSize * 2; ++i) data[(size_t) i] = 0.0f;
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        const float  normFactor = 2.0f / (float) fftSize;
        const int    usableBins = fftSize / 2;
        const double binHz      = sr / (double) fftSize;
        const double maxFreq    = std::min (sr * 0.5, 20000.0);
        const auto   edges      = echojay::macroBandEdges (maxFreq);
        double bandPower[6] = {};
        for (int k = 1; k < usableBins; ++k)
        {
            const double f = k * binHz;
            if (f < 20.0 || f >= maxFreq) continue;
            const double m = (double) data[(size_t) k] * normFactor;
            for (int bi = 0; bi < 6; ++bi)
                if (f >= edges[(size_t) bi].lo && f < edges[(size_t) bi].hi)
                { bandPower[bi] += m * m; break; }
        }
        for (int bi = 0; bi < 6; ++bi)
        {
            const double octaves = std::log2 (std::max (1.01, edges[(size_t) bi].hi / edges[(size_t) bi].lo));
            const double perOct  = bandPower[bi] / octaves;
            sum[(size_t) bi] += perOct;
            peak[(size_t) bi] = std::max (peak[(size_t) bi], perOct);
        }
        ++frames;
        if (! (silentCount > silenceTimeout)) allSilent = false;
    }

    double sr;
    juce::dsp::FFT fft;
    std::array<float, fftSize> window {};
    std::array<float, fftSize> acc {};
    std::array<float, fftSize * 2> data {};
    int writePos = 0;

    int samplesPerHop = 0, hopSamples = 0;
    int silenceTimeout = 0, silentCount = 0;

    std::array<double, 6> sum {}, peak {};
    int  frames = 0;
    bool allSilent = true;
    std::vector<Hop> out;
};

/** A whole stereo signal through HopSim in blocks of blockSize, the last one
    short, the way ReferenceAnalyser feeds its engine. */
inline std::vector<Hop> hopsFor (const std::vector<float>& l, const std::vector<float>& r,
                                 double sampleRate, int blockSize)
{
    HopSim sim (sampleRate);
    const size_t n = std::min (l.size(), r.size());
    for (size_t i = 0; i < n; i += (size_t) blockSize)
    {
        const int len = (int) std::min ((size_t) blockSize, n - i);
        sim.process (l.data() + i, r.data() + i, len);
    }
    return sim.hops();
}

} // namespace ma10
