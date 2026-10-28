#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>
#include "EJBandScheme.h"
#include "EJMatchWindow.h"
#include "ma10_hops.h"

// =============================================================================
//  MA10 CREST VARIANTS V1 AND V2, HARNESS ONLY.
// =============================================================================
//
// V0 is C4 rev 2's crest as run (ma10_hops.h into EJMatchWindow.h::bandCrest).
// These two are candidates for Kathy to weigh against it. Nothing in Source/
// reads them.
//
// V1: the 2048-point frame of ma10_hops.h (Hann, windowed mean removed, perOct
//     by EJBandScheme's edges), but run on a SAMPLE-COUNTED cadence of one
//     frame every 256 samples, whatever block size is fed. Per hop and band,
//     10 log10 (max perOct / mean perOct) over the hop's frames; the figure is
//     the median of that over qualifying hops.
// V2: as V1, but lowMid, mid, highMid and air come from a 1024-point Hann frame
//     (same recipe, normFactor 2/1024) taken at the same instants. Sub and low
//     stay on the 2048 frame.
//
// WHAT IS KEPT FROM THE ENGINE: the per-block silence detector (MeterEngine
// :556-572), updated once per block before that block's frames, exactly as
// ma10_hops.h does. A hop is flagged silent when every frame in it was taken
// while the engine read silent. So the only way the fed block size can still
// reach V1 or V2 is through those flags.
//
// THE ORDER AT ONE SAMPLE: a frame is due after its 256th sample and a hop
// closes after its 4,800th. Both land on the same sample every 76,800 samples
// (16 hops); the frame is taken first, so it belongs to the hop that ends there.
// A trailing partial hop is never closed, as in ma10_hops.h.
// =============================================================================

namespace ma10
{

/** One frame's six perOct powers from the most recent N samples of a mono ring.
    The arithmetic is ma10_hops.h's computeFrame, with N as a parameter. */
template <int Order>
class PerOctFrame
{
public:
    static constexpr int N = 1 << Order;

    explicit PerOctFrame (double sampleRate) : sr (sampleRate), fft (Order)
    {
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                          * (float) i / (float) N));
    }

    /** ring holds ringSize samples; writePos is the next slot to write, so the
        oldest of the last N is at writePos - N. */
    std::array<double, 6> compute (const float* ring, int ringSize, int writePos)
    {
        for (int i = 0; i < N; ++i)
            data[(size_t) i] = ring[(size_t) ((writePos - N + i + ringSize) % ringSize)] * window[(size_t) i];
        double dcSum = 0.0;
        for (int i = 0; i < N; ++i) dcSum += data[(size_t) i];
        const float dcMean = (float) (dcSum / N);
        for (int i = 0; i < N; ++i) data[(size_t) i] -= dcMean;
        for (int i = N; i < N * 2; ++i) data[(size_t) i] = 0.0f;
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        const float  normFactor = 2.0f / (float) N;
        const double binHz      = sr / (double) N;
        const double maxFreq    = std::min (sr * 0.5, 20000.0);
        const auto   edges      = echojay::macroBandEdges (maxFreq);
        double bp[6] = {};
        for (int k = 1; k < N / 2; ++k)
        {
            const double f = k * binHz;
            if (f < 20.0 || f >= maxFreq) continue;
            const double m = (double) data[(size_t) k] * normFactor;
            for (int bi = 0; bi < 6; ++bi)
                if (f >= edges[(size_t) bi].lo && f < edges[(size_t) bi].hi)
                { bp[bi] += m * m; break; }
        }
        std::array<double, 6> out {};
        for (int bi = 0; bi < 6; ++bi)
            out[(size_t) bi] = bp[bi] / std::log2 (std::max (1.01, edges[(size_t) bi].hi / edges[(size_t) bi].lo));
        return out;
    }

private:
    double sr;
    juce::dsp::FFT fft;
    std::array<float, N> window {};
    std::array<float, N * 2> data {};
};

/** The V1 / V2 hop series. bandPower is the mean perOct of the hop's frames and
    bandPeak the largest, the same fields ma10_hops.h fills. */
class CadenceSim
{
public:
    static constexpr int kRing = 2048;
    static constexpr int kCadence = 256;
    static constexpr float kSilenceThreshold = 0.0001f;

    CadenceSim (double sampleRate, bool split1024)
        : split (split1024), f2048 (sampleRate), f1024 (sampleRate)
    {
        samplesPerHop  = (int) (sampleRate * 0.1);
        silenceTimeout = (int) (sampleRate * 0.5);
        silentCount    = silenceTimeout + 1;   // starts silent, as the engine does
    }

    void process (const float* left, const float* right, int n)
    {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
            pk = std::max (pk, std::max (std::abs (left[i]), std::abs (right[i])));
        if (pk > kSilenceThreshold) silentCount = 0; else silentCount += n;
        const bool silentNow = silentCount > silenceTimeout;

        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) writePos] = (left[i] + right[i]) * 0.5f;
            writePos = (writePos + 1) % kRing;
            if (++sinceFrame == kCadence) { frame (silentNow); sinceFrame = 0; }
            if (++hopSamples == samplesPerHop) { closeHop(); hopSamples = 0; }
        }
    }

    const std::vector<Hop>& hops() const noexcept { return out; }

private:
    void frame (bool silentNow)
    {
        auto p = f2048.compute (ring.data(), kRing, writePos);
        if (split)
        {
            const auto q = f1024.compute (ring.data(), kRing, writePos);
            for (size_t b = 2; b < 6; ++b) p[b] = q[b];
        }
        for (size_t b = 0; b < 6; ++b) { sum[b] += p[b]; peak[b] = std::max (peak[b], p[b]); }
        ++frames;
        if (! silentNow) allSilent = false;
    }

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

    bool split;
    PerOctFrame<11> f2048;
    PerOctFrame<10> f1024;
    std::array<float, kRing> ring {};
    int writePos = 0, sinceFrame = 0;
    int samplesPerHop = 0, hopSamples = 0;
    int silenceTimeout = 0, silentCount = 0;
    std::array<double, 6> sum {}, peak {};
    int  frames = 0;
    bool allSilent = true;
    std::vector<Hop> out;
};

inline std::vector<Hop> cadenceHopsFor (const std::vector<float>& l, const std::vector<float>& r,
                                        double sampleRate, int blockSize, bool split1024)
{
    CadenceSim sim (sampleRate, split1024);
    const size_t n = std::min (l.size(), r.size());
    for (size_t i = 0; i < n; i += (size_t) blockSize)
    {
        const int len = (int) std::min ((size_t) blockSize, n - i);
        sim.process (l.data() + i, r.data() + i, len);
    }
    return sim.hops();
}

/** V1 / V2's figure: per band, the median over qualifying hops of
    10 log10 (bandPeak / bandPower). A hop with no power in the band gives no
    ratio. An even count takes the mean of the two middle values, as psrMedian
    does. qualifyingHops counts crestHopQualifies, as bandCrest does. */
struct PeakMeanCrest
{
    std::array<std::optional<float>, 6> db {};
    int qualifyingHops = 0;
    std::array<int, 6> ratioHops {};
};

inline PeakMeanCrest peakMeanCrest (const std::vector<Hop>& hops)
{
    PeakMeanCrest c;
    std::array<std::vector<double>, 6> v;
    for (const auto& h : hops)
    {
        if (! echojay::crestHopQualifies (h)) continue;
        ++c.qualifyingHops;
        for (size_t b = 0; b < 6; ++b)
            if (h.bandPower[b] > 0.0f)
                v[b].push_back (10.0 * std::log10 ((double) h.bandPeak[b] / (double) h.bandPower[b]));
    }
    for (size_t b = 0; b < 6; ++b)
    {
        c.ratioHops[b] = (int) v[b].size();
        if (v[b].empty()) continue;
        std::sort (v[b].begin(), v[b].end());
        const size_t n = v[b].size();
        c.db[b] = (float) (n % 2 ? v[b][n / 2] : 0.5 * (v[b][n / 2 - 1] + v[b][n / 2]));
    }
    return c;
}

} // namespace ma10
