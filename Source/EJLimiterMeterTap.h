#pragma once
/*
    EJLimiterMeterTap.h (session L, 8 Oct 2026): the audio-thread -> panel data path for the limiter's picture.
    JUCE-free, allocation-free after prepare(), lock-free (one producer, one consumer, atomics only). The audio side
    does O(1) work per sample: min/max of in and out per display column, the gain, two K-weighting biquads per
    channel and an energy sum per 100 ms hop. Everything that needs arithmetic over time - loudness gating, peak
    holds, the scrolling picture - is the consumer's job on the message thread.

    Two rings:
      columns  one record per `columnSamples` (the display speed; the UI sets it, the audio thread reads it once per
               column): in min/max, out min/max (linear, both channels folded), the deepest gain of the column (dB)
      hops     one record per 100 ms: the K-weighted mean square summed over channels (BS.1770 z), the true-peak
               maxima of in and out seen in the hop (linear) - the panel's momentary / short-term / integrated LUFS
               and its dBTP holds come from these

    DELIBERATELY RACY in the same way VizTap.h is: the reader may see a torn record under contention and shows a
    picture one frame old; the writer never waits.
*/
#include "EchoJayKWeighting.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace echojay::limv2 {

struct ColumnRecord { float inMin = 0, inMax = 0, outMin = 0, outMax = 0, grDb = 0; };
struct HopRecord    { double z = 0; float tpIn = 0, tpOut = 0; bool valid = false; };

class MeterTap
{
public:
    static constexpr int kColumns = 4096, kHops = 1024;   // 4096 columns at 1 ms = 4 s of the fastest picture; 1024 hops = 102 s

    void prepare (double sampleRate)
    {
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        columns_.assign ((size_t) kColumns, ColumnRecord {}); hops_.assign ((size_t) kHops, HopRecord {});
        echojay::computeKWeightingCoeffs (sr_, k1_, k2_);
        hopSamples_ = (int) std::lround (0.1 * sr_);
        reset();
    }
    void reset() noexcept
    {
        colHead_.store (0, std::memory_order_relaxed); hopHead_.store (0, std::memory_order_relaxed);
        colFill_ = 0; hopFill_ = 0; cur_ = ColumnRecord {}; cur_.inMin = cur_.outMin = 1e9f; cur_.inMax = cur_.outMax = -1e9f; cur_.grDb = 0;
        hopZ_ = 0; hopTpIn_ = hopTpOut_ = 0; for (auto& z : zl_) z = 0; for (auto& z : zr_) z = 0;
    }
    // the display speed: samples per column (the UI writes, the audio thread reads at a column boundary)
    void setColumnSamples (int n) noexcept { columnSamples_.store (std::max (8, n), std::memory_order_relaxed); }
    int  columnSamples() const noexcept { return columnSamples_.load (std::memory_order_relaxed); }

    // AUDIO THREAD. One sample: in/out per channel (mono: pass the same for r), the gain applied (linear), and the
    // true-peak envelope values the engine already computed for this sample (so nothing is interpolated twice).
    inline void push (float inL, float inR, float outL, float outR, float gainLin, float tpIn, float tpOut) noexcept
    {
        cur_.inMin = std::min (cur_.inMin, std::min (inL, inR)); cur_.inMax = std::max (cur_.inMax, std::max (inL, inR));
        cur_.outMin = std::min (cur_.outMin, std::min (outL, outR)); cur_.outMax = std::max (cur_.outMax, std::max (outL, outR));
        const float gDb = gainLin < 1.0f ? 20.0f * std::log10 (std::max (gainLin, 1e-6f)) : 0.0f; cur_.grDb = std::min (cur_.grDb, gDb);
        if (++colFill_ >= columnSamples_.load (std::memory_order_relaxed))
        {
            const int h = colHead_.load (std::memory_order_relaxed); columns_[(size_t) (h % kColumns)] = cur_; colHead_.store (h + 1, std::memory_order_release);
            colFill_ = 0; cur_ = ColumnRecord {}; cur_.inMin = cur_.outMin = 1e9f; cur_.inMax = cur_.outMax = -1e9f; cur_.grDb = 0;
        }
        // K-weighted OUTPUT power for loudness (what leaves the limiter is what the meter is for)
        const double kl = biquad (outL, zl_, k1_), kr = biquad (outR, zr_, k1_);
        const double kl2 = biquad ((float) kl, zl_ + 2, k2_), kr2 = biquad ((float) kr, zr_ + 2, k2_);
        hopZ_ += kl2 * kl2 + kr2 * kr2; hopTpIn_ = std::max (hopTpIn_, tpIn); hopTpOut_ = std::max (hopTpOut_, tpOut);
        if (++hopFill_ >= hopSamples_)
        {
            HopRecord r; r.z = hopZ_ / (double) hopSamples_; r.tpIn = hopTpIn_; r.tpOut = hopTpOut_; r.valid = true;
            const int h = hopHead_.load (std::memory_order_relaxed); hops_[(size_t) (h % kHops)] = r; hopHead_.store (h + 1, std::memory_order_release);
            hopFill_ = 0; hopZ_ = 0; hopTpIn_ = hopTpOut_ = 0;
        }
    }

    // MESSAGE THREAD. Heads only ever grow; the reader keeps its own cursor.
    int  columnHead() const noexcept { return colHead_.load (std::memory_order_acquire); }
    int  hopHead() const noexcept { return hopHead_.load (std::memory_order_acquire); }
    const ColumnRecord& column (int index) const noexcept { return columns_[(size_t) (((index % kColumns) + kColumns) % kColumns)]; }
    const HopRecord&    hop (int index) const noexcept { return hops_[(size_t) (((index % kHops) + kHops) % kHops)]; }
    double sampleRate() const noexcept { return sr_; }

private:
    static inline double biquad (float x, double* z, const BiquadCoeffs& c) noexcept { const double y = c.b0 * x + z[0]; z[0] = c.b1 * x - c.a1 * y + z[1]; z[1] = c.b2 * x - c.a2 * y; return y; }
    double sr_ = 48000.0; BiquadCoeffs k1_ {}, k2_ {}; double zl_[4] {}, zr_[4] {};
    std::vector<ColumnRecord> columns_; std::vector<HopRecord> hops_;
    std::atomic<int> colHead_ { 0 }, hopHead_ { 0 }; std::atomic<int> columnSamples_ { 96 };
    int colFill_ = 0, hopFill_ = 0, hopSamples_ = 4800; ColumnRecord cur_; double hopZ_ = 0; float hopTpIn_ = 0, hopTpOut_ = 0;
};

// The consumer's loudness: momentary (400 ms = 4 hops), short-term (3 s = 30 hops), integrated with BS.1770-4's
// two-stage gate over a 0.1 LU histogram of 400 ms blocks (so reset and the relative gate are message-thread
// arithmetic, never audio-thread work). Also the true-peak holds.
class LoudnessReader
{
public:
    void reset() noexcept { std::fill (hist_.begin(), hist_.end(), 0.0); std::fill (histZ_.begin(), histZ_.end(), 0.0); nBlocks_ = 0; momentary_ = shortTerm_ = integrated_ = -200.0; tpInHold_ = tpOutHold_ = 0; recent_.clear(); }
    void consume (const MeterTap& tap)
    {
        const int head = tap.hopHead(); if (cursor_ < head - MeterTap::kHops) cursor_ = head - MeterTap::kHops;
        for (; cursor_ < head; ++cursor_)
        {
            const auto& h = tap.hop (cursor_); if (! h.valid) continue;
            recent_.push_back (h.z); if (recent_.size() > 30) recent_.erase (recent_.begin());
            tpInHold_ = std::max (tpInHold_, h.tpIn); tpOutHold_ = std::max (tpOutHold_, h.tpOut);
            if (recent_.size() >= 4) { double s = 0; for (size_t i = recent_.size() - 4; i < recent_.size(); ++i) s += recent_[i]; const double zm = s / 4.0; momentary_ = lufs (zm);
                if (momentary_ > -70.0) { const int bin = std::clamp ((int) std::lround ((momentary_ + 70.0) * 10.0), 0, (int) hist_.size() - 1); hist_[(size_t) bin] += 1.0; histZ_[(size_t) bin] += zm; ++nBlocks_; } }
            if (recent_.size() >= 30) { double s = 0; for (double z : recent_) s += z; shortTerm_ = lufs (s / 30.0); }
        }
        // integrated: absolute gate is the histogram's floor; relative gate = mean over the gated blocks - 10 LU
        if (nBlocks_ > 0)
        {
            double n = 0, z = 0; for (size_t b = 0; b < hist_.size(); ++b) { n += hist_[b]; z += histZ_[b]; }
            const double rel = lufs (z / n) - 10.0; const int relBin = std::clamp ((int) std::lround ((rel + 70.0) * 10.0), 0, (int) hist_.size() - 1);
            double n2 = 0, z2 = 0; for (size_t b = (size_t) relBin; b < hist_.size(); ++b) { n2 += hist_[b]; z2 += histZ_[b]; }
            integrated_ = n2 > 0 ? lufs (z2 / n2) : -200.0;
        }
    }
    double momentary() const noexcept { return momentary_; }
    double shortTerm() const noexcept { return shortTerm_; }
    double integrated() const noexcept { return integrated_; }
    float  truePeakInDb() const noexcept { return tpInHold_ > 0 ? 20.0f * std::log10 (tpInHold_) : -200.0f; }
    float  truePeakOutDb() const noexcept { return tpOutHold_ > 0 ? 20.0f * std::log10 (tpOutHold_) : -200.0f; }
    void   resetHolds() noexcept { tpInHold_ = tpOutHold_ = 0; }
private:
    static double lufs (double z) noexcept { return z > 1e-30 ? -0.691 + 10.0 * std::log10 (z) : -200.0; }
    std::vector<double> hist_ = std::vector<double> (800, 0.0), histZ_ = std::vector<double> (800, 0.0), recent_;
    int cursor_ = 0; double nBlocks_ = 0, momentary_ = -200, shortTerm_ = -200, integrated_ = -200; float tpInHold_ = 0, tpOutHold_ = 0;
};

} // namespace echojay::limv2
