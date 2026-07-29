/*
    VizTap.h  —  the audio-thread -> editor data path every live visualisation
    uses (VISUALS_PLAN.md, "Small tap helper").

    Two things, and deliberately only two, because the plan's whole point is that
    most visuals need NO tap at all:

      * FloatTap  — one lock-free float. GR, detector level, LFO phase, I/O peak.
      * SampleTap — a small lock-free ring of recent samples, mono or stereo.
                    Only the goniometer and the harmonic bars need one.

    This is not a new contract. It is the one SurgicalEqProcessor::AnalysisRing
    and DynamicsCore::gainReductionDb already use, factored out so a device adds
    a tap in two lines instead of re-deriving the memory ordering:

        echojay::viz::ScopeTap scopeTap_;              // a member
        scopeTap_.write (l, r, n);                     // in processBlock

    DELIBERATELY RACY, exactly like the EQ's analyzer. The writer never blocks,
    never allocates and never waits on the reader; the reader takes the newest
    span and accepts that it may tear under contention. A visualiser that tears
    one frame is invisible. An audio thread that blocks is a dropout.

    JUCE-free on purpose: this sits in processBlock, and the DSP side of EchoJay
    (EqEngine, DynamicsCore, StereoEngine) is JUCE-free so it can be g++-tested
    without a plugin build. A tap that dragged JuceHeader.h into those files
    would undo that.
*/

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace echojay::viz
{

// ---------------------------------------------------------------------------
// One float, written on the audio thread, read on the editor's timer.
//
// relaxed ordering on both sides: there is nothing else to synchronise WITH.
// The value is self-contained, and a reader that sees the previous block's
// number is showing a picture 20 ms old, which is the same latency the eye has.
// ---------------------------------------------------------------------------
class FloatTap
{
public:
    explicit FloatTap (float initial = 0.0f) noexcept : v_ (initial) {}

    void  set (float x) noexcept       { v_.store (x, std::memory_order_relaxed); }
    float get() const noexcept         { return v_.load (std::memory_order_relaxed); }

    // Peak-hold within a block: a device that wants "the loudest sample of this
    // buffer" does not need its own max() plumbing.
    void setIfLouder (float x) noexcept
    {
        if (x > v_.load (std::memory_order_relaxed)) set (x);
    }

private:
    std::atomic<float> v_;
};

// ---------------------------------------------------------------------------
// SampleTap — a fixed-size, power-of-two ring of the most recent samples.
//
// `Store` is float or short. Short halves the memory and is what a SCOPE wants:
// a goniometer plots into a few hundred pixels, so 16-bit is already far more
// precision than the display can show, and two 2048-sample channels cost 8 KB
// instead of 16. Anything that will be FFT'd (the harmonic bars) uses float.
//
// `kSize` samples per channel. `kNumChannels` is 1 or 2.
// ---------------------------------------------------------------------------
template <typename Store, int kSize, int kNumChannels = 2>
class SampleTap
{
public:
    static_assert (kSize > 0 && (kSize & (kSize - 1)) == 0, "kSize must be a power of two");
    static_assert (kNumChannels == 1 || kNumChannels == 2, "mono or stereo");
    static_assert (std::is_same_v<Store, float> || std::is_same_v<Store, short>,
                   "Store is float or short");

    static constexpr int size        = kSize;
    static constexpr int numChannels = kNumChannels;

    // ---- audio thread ------------------------------------------------------
    // `right` may be null (mono source); a stereo ring then holds the same
    // signal on both sides, which is what a goniometer should show for mono.
    // A mono ring ignores `right` entirely.
    void write (const float* left, const float* right, int numSamples) noexcept
    {
        if (left == nullptr || numSamples <= 0) return;

        const std::uint32_t w = write_.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            const std::size_t s = (std::size_t) ((w + (std::uint32_t) i) & kMask);
            ch0_[s] = encode (left[i]);
            if constexpr (kNumChannels == 2)
                ch1_[s] = encode (right != nullptr ? right[i] : left[i]);
        }

        // The one release: everything written above is visible to a reader that
        // acquires this index, so a reader can never look at a slot the writer
        // has not finished filling.
        write_.store (w + (std::uint32_t) numSamples, std::memory_order_release);
    }

    // Mono convenience — a device tapping a summed signal.
    void write (const float* mono, int numSamples) noexcept
    {
        write (mono, nullptr, numSamples);
    }

    // ---- message thread ----------------------------------------------------
    // Copies the newest `maxSamples` in CHRONOLOGICAL order (oldest first).
    // `destRight` may be null. Returns how many were written.
    int read (float* destLeft, float* destRight, int maxSamples) const noexcept
    {
        if (destLeft == nullptr || maxSamples <= 0) return 0;

        const std::uint32_t w = write_.load (std::memory_order_acquire);
        const int           n = std::min (maxSamples, kSize);
        const std::uint32_t start = w - (std::uint32_t) n;

        for (int i = 0; i < n; ++i)
        {
            const std::size_t s = (std::size_t) ((start + (std::uint32_t) i) & kMask);
            destLeft[i] = decode (ch0_[s]);

            if (destRight != nullptr)
                destRight[i] = decode (kNumChannels == 2 ? ch1_[s] : ch0_[s]);
        }

        return n;
    }

    int read (float* dest, int maxSamples) const noexcept
    {
        return read (dest, nullptr, maxSamples);
    }

    // NOT real-time safe in spirit (it touches the whole buffer) but allocation
    // free — call it from prepareToPlay, as the EQ's rings do.
    void clear() noexcept
    {
        ch0_.fill (Store {});
        if constexpr (kNumChannels == 2) ch1_.fill (Store {});
        write_.store (0, std::memory_order_release);
    }

private:
    static constexpr int kMask = kSize - 1;

    static Store encode (float x) noexcept
    {
        if constexpr (std::is_same_v<Store, short>)
        {
            // Clamped BEFORE the cast: a signal over 0 dBFS is ordinary inside a
            // chain, and an unclamped conversion wraps +1.2 round to a large
            // negative — a scope that draws garbage exactly when the user is
            // clipping, which is when they are looking at it.
            const float c = std::fmax (-1.0f, std::fmin (1.0f, x));
            return (short) std::lround (c * 32767.0f);
        }
        else
        {
            return x;
        }
    }

    static float decode (Store s) noexcept
    {
        if constexpr (std::is_same_v<Store, short>)
            return (float) s * (1.0f / 32767.0f);
        else
            return s;
    }

    // Only the index is atomic; the arrays are plain, which is all the reader
    // needs to locate the newest span. Same structure as the EQ's AnalysisRing.
    std::array<Store, (std::size_t) kSize> ch0_ {};
    std::array<Store, (std::size_t) (kNumChannels == 2 ? kSize : 1)> ch1_ {};
    std::atomic<std::uint32_t> write_ { 0 };
};

// ---------------------------------------------------------------------------
// The two rings anything in Source/viz actually needs.
// ---------------------------------------------------------------------------

// Stereo, 16-bit, ~46 ms at 44.1 kHz — the goniometer's input. Long enough that
// a frame at 20 Hz always has fresh material, short enough that the Lissajous
// shows the CURRENT image rather than an average of the last second.
using ScopeTap = SampleTap<short, 2048, 2>;

// Mono, float, one FFT frame — the harmonic bars' input.
using SpectrumTap = SampleTap<float, 4096, 1>;

} // namespace echojay::viz
