#pragma once
// ===========================================================================
// LevelTally: the running level instrument (17 Aug 2026).
//
// EchoJay is in the signal path every time the user plays, so instead of
// asking for a capture it keeps a running tally of level at every point of
// the chain: the chain input, the chain output, and each slot's input and
// output (SlotWetBlend sits on both legs). Two things read it later: C7,
// chain gain matching (chain in vs out) and L2, vocal gain reduction (a
// compressor slot's out minus in is the MEASURED reduction, and its input
// p90 is what a threshold is set against). This file is the instrument
// only; nothing here decides anything.
//
// ONE INSTRUMENT. This is new and small; MeterEngine keeps the post-chain
// display numbers it has today and is never instantiated per slot. The two
// share exactly one thing, the K-weighting coefficients (EchoJayKWeighting.h),
// so their LUFS mean the same thing.
//
// THE HONESTY CONDITIONS, each of which is the difference between a
// measurement and a number that looks like one:
//   gated      BS.1770 two-stage: 400 ms momentary blocks below the absolute
//              gate (-70 LUFS) are ignored, then a relative gate 10 LU below
//              the absolute-gated mean. Silence contributes nothing; a channel
//              that played loudly for twenty seconds reads as those twenty.
//              Identical in meaning to MeterEngine's integrated LUFS.
//   audio-seen heardSeconds counts blocks above the absolute gate x 0.1 s and
//              never decays. windowSeconds says how much of that the decayed
//              estimate actually describes (see kHalfLifeSeconds). Both go
//              to the reader, because "four seconds" and "four minutes" are
//              different claims.
//   decay      every gated hop multiplies the histogram by
//              2^(-0.1 / kHalfLifeSeconds). A fader moved upstream is not
//              detectable, and only decay heals the estimate afterwards; too
//              short and a verse forgets the chorus, too long and the stale
//              level lasts the session. 120 s: a chorus and a verse both
//              weigh in, a 6 dB fader move settles within a few minutes of
//              playing. windowSeconds = min(heard, kEffectiveWindowSeconds),
//              the rectangular-equivalent memory of that exponential
//              (2 x halfLife / ln 2, about 346 s), so the reader is told the
//              window rather than left to assume "heard" describes it.
//   loud null  known == false until kHeardFloorSeconds of gated audio have
//              been heard, and while it is false every numeric field is NaN.
//              There is no default value anywhere in this file; a caller
//              that reads a number without checking known() reads NaN,
//              which fails loudly rather than plausibly.
//
// Statistics (the smallest set that serves C7 and L2): gated loudness
// (LUFS), p10 / p50 / p90 of momentary loudness over the absolute-gated
// hops (L2 reasons about p90, because reduction is set by the loud
// passages), plain RMS and a recent peak (both decayed) for crest.
//
// Threading: push() on the audio thread only (no allocation, no lock held
// across audio work: the per-hop publish is a SpinLock TRY-lock that skips a
// hop rather than block); snapshot() on the message thread. reset() may be
// called from any thread; it lands at the next push.
//
// House rules: no em-dashes anywhere in this file.
// ===========================================================================
#include <juce_core/juce_core.h>
#include "EchoJayKWeighting.h"
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

namespace echojay {

class LevelTally
{
public:
    // ---- the named constants that will want tuning once ------------------
    static constexpr double kHalfLifeSeconds        = 120.0;
    static constexpr double kEffectiveWindowSeconds = 2.0 * kHalfLifeSeconds / 0.6931471805599453;   // 2 tau
    static constexpr float  kHeardFloorSeconds      = 3.0f;    // below this: no level known
    static constexpr float  kAbsGateLufs            = -70.0f;  // BS.1770 absolute gate
    static constexpr float  kRelGateLu              = -10.0f;  // BS.1770 relative gate
    static constexpr double kHopSeconds             = 0.1;     // 100 ms hop
    static constexpr int    kHopsPerBlock           = 4;       // 400 ms momentary window
    static constexpr int    kBins                   = 60;      // 1 dB bins, kBinLoLufs upward
    static constexpr float  kBinLoLufs              = -70.0f;

    struct Snapshot
    {
        bool  known = false;          // heardSeconds >= kHeardFloorSeconds
        float heardSeconds  = 0.0f;   // gated audio heard since reset, undecayed
        float windowSeconds = 0.0f;   // how much of it the estimate describes
        // Everything below is NaN while !known. Read known first.
        float lufsGated = std::numeric_limits<float>::quiet_NaN();
        float p10 = std::numeric_limits<float>::quiet_NaN();
        float p50 = std::numeric_limits<float>::quiet_NaN();
        float p90 = std::numeric_limits<float>::quiet_NaN();
        float rmsDb   = std::numeric_limits<float>::quiet_NaN();   // plain, per-channel-mean, decayed
        float peakDb  = std::numeric_limits<float>::quiet_NaN();   // recent sample peak, decayed hold
        float crestDb = std::numeric_limits<float>::quiet_NaN();   // peakDb - rmsDb
    };

    LevelTally() { std::fill (bins_.begin(), bins_.end(), 0.0); }

    // Message thread (or before audio starts). Also clears the state.
    void prepare (double sampleRate)
    {
        sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
        computeKWeightingCoeffs (sampleRate_, k1_, k2_);
        hopSamples_ = juce::jmax (1, (int) std::lround (sampleRate_ * kHopSeconds));
        decayPerHop_ = std::pow (2.0, -kHopSeconds / kHalfLifeSeconds);
        prepared_ = true;
        clearNow();
        publish (true);
    }
    // Any thread: lands at the next push (the audio thread owns the state).
    void reset() noexcept { resetRequested_.store (true, std::memory_order_relaxed); }

    // Audio thread. right may be null (mono). n >= 0.
    void push (const float* left, const float* right, int n) noexcept
    {
        if (! prepared_ || left == nullptr || n <= 0) return;
        if (resetRequested_.exchange (false, std::memory_order_relaxed)) { clearNow(); publish (true); }
        for (int i = 0; i < n; ++i)
        {
            const float l = left[i];
            const float r = right != nullptr ? right[i] : l;
            // plain power (per-channel mean) and sample peak
            hopPlainPow_ += 0.5 * ((double) l * l + (double) r * r);
            const float a = std::max (std::abs (l), std::abs (r));
            if (a > hopPeak_) hopPeak_ = a;
            // K-weighted power, summed over channels as BS.1770 does
            const double kl = biquad (l, zl1_, k1_), kr = biquad (r, zr1_, k1_);
            const double kl2 = biquad ((float) kl, zl2_, k2_), kr2 = biquad ((float) kr, zr2_, k2_);
            hopKPow_ += kl2 * kl2 + kr2 * kr2;
            if (++hopFill_ >= hopSamples_) closeHop();
        }
    }

    // Message thread.
    Snapshot snapshot() const
    {
        const juce::SpinLock::ScopedLockType lock (pubLock_);
        return pub_;
    }
    bool known() const { return snapshot().known; }

    // ---- persistence (used by the session state, step 4) -----------------
    juce::var toVar() const
    {
        // Snapshot of the WORKING state as last published: bins (3 dp),
        // heard, recent peak, decayed plain power. Enough to resume with the
        // same claims; the reader re-derives everything else.
        const juce::SpinLock::ScopedLockType lock (pubLock_);
        auto* o = new juce::DynamicObject();
        juce::Array<juce::var> b;
        juce::Array<juce::var> bp;
        for (int i = 0; i < kBins; ++i) { b.add (std::round (pubBins_[(size_t) i] * 1000.0) / 1000.0); bp.add (pubBinPow_[(size_t) i]); }
        o->setProperty ("bins", b);
        o->setProperty ("binPow", bp);
        o->setProperty ("heard", (double) pubHeardHops_);
        o->setProperty ("plainPow", pubPlainPow_);
        o->setProperty ("plainW", pubPlainW_);
        o->setProperty ("peak", (double) pubPeak_);
        return juce::var (o);
    }
    // Any thread before audio, or message thread: adopted at the next push.
    bool fromVar (const juce::var& v)
    {
        auto* o = v.getDynamicObject();
        if (o == nullptr) return false;
        auto* b = o->getProperty ("bins").getArray();
        if (b == nullptr || b->size() != kBins) return false;
        PendingRestore p;
        auto* bpArr = o->getProperty ("binPow").getArray();
        if (bpArr == nullptr || bpArr->size() != kBins) return false;
        for (int i = 0; i < kBins; ++i)
        {
            p.bins[(size_t) i]   = juce::jmax (0.0, (double) (*b)[i]);
            p.binPow[(size_t) i] = juce::jmax (0.0, (double) (*bpArr)[i]);
        }
        p.heardHops = juce::jmax (0.0, (double) o->getProperty ("heard"));
        p.plainPow  = juce::jmax (0.0, (double) o->getProperty ("plainPow"));
        p.plainW    = juce::jmax (0.0, (double) o->getProperty ("plainW"));
        p.peak      = juce::jlimit (0.0f, 4.0f, (float) (double) o->getProperty ("peak"));
        {
            const juce::SpinLock::ScopedLockType lock (restoreLock_);
            pending_ = p;
            restoreRequested_.store (true, std::memory_order_release);
        }
        return true;
    }

private:
    // ---- audio-thread state ------------------------------------------------
    double sampleRate_ = 48000.0;
    bool   prepared_ = false;
    int    hopSamples_ = 4800;
    double decayPerHop_ = 1.0;
    BiquadCoeffs k1_ {}, k2_ {};
    struct Z { double z1 = 0.0, z2 = 0.0; };
    Z zl1_, zr1_, zl2_, zr2_;
    static double biquad (float x, Z& z, const BiquadCoeffs& c) noexcept
    {
        // transposed direct form II
        const double y = c.b0 * x + z.z1;
        z.z1 = c.b1 * x - c.a1 * y + z.z2;
        z.z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    // current hop accumulators
    int    hopFill_ = 0;
    double hopKPow_ = 0.0, hopPlainPow_ = 0.0;
    float  hopPeak_ = 0.0f;
    // last kHopsPerBlock hop K-powers for the 400 ms momentary block
    std::array<double, kHopsPerBlock> ring_ {};
    int    ringPos_ = 0, ringFill_ = 0;
    // the tally proper
    std::array<double, kBins> bins_ {};   // decayed weights of momentary loudness (absolute-gated hops)
    std::array<double, kBins> binPow_ {}; // decayed K-power sums per bin: the gated mean is exact, not bin-centred
    double heardHops_ = 0.0;              // undecayed count of absolute-gated hops
    double plainPow_ = 0.0, plainW_ = 0.0;   // decayed plain-power sum and its weight (gated hops)
    float  peak_ = 0.0f;                  // recent peak, decayed hold

    std::atomic<bool> resetRequested_ { false };
    std::atomic<bool> restoreRequested_ { false };
    struct PendingRestore { std::array<double, kBins> bins {}, binPow {}; double heardHops = 0, plainPow = 0, plainW = 0; float peak = 0; };
    PendingRestore pending_;
    juce::SpinLock restoreLock_;

    void clearNow() noexcept
    {
        zl1_ = zr1_ = zl2_ = zr2_ = Z {};
        hopFill_ = 0; hopKPow_ = hopPlainPow_ = 0.0; hopPeak_ = 0.0f;
        ring_.fill (0.0); ringPos_ = 0; ringFill_ = 0;
        bins_.fill (0.0); binPow_.fill (0.0); heardHops_ = 0.0; plainPow_ = plainW_ = 0.0; peak_ = 0.0f;
    }

    void closeHop() noexcept
    {
        // A pending restore lands here, at a hop boundary, on the audio thread
        if (restoreRequested_.load (std::memory_order_acquire))
        {
            if (restoreLock_.tryEnter())
            {
                bins_ = pending_.bins; binPow_ = pending_.binPow; heardHops_ = pending_.heardHops;
                plainPow_ = pending_.plainPow; plainW_ = pending_.plainW; peak_ = pending_.peak;
                restoreRequested_.store (false, std::memory_order_relaxed);
                restoreLock_.exit();
            }
        }
        const double hopK = hopKPow_ / (double) hopSamples_;   // mean K-power over the hop (L+R)
        ring_[(size_t) ringPos_] = hopK;
        ringPos_ = (ringPos_ + 1) % kHopsPerBlock;
        if (ringFill_ < kHopsPerBlock) ++ringFill_;
        if (ringFill_ == kHopsPerBlock)
        {
            double mp = 0.0;
            for (auto v : ring_) mp += v;
            mp /= (double) kHopsPerBlock;
            const double lufs = mp > 0.0 ? -0.691 + 10.0 * std::log10 (mp) : -200.0;
            if (lufs > kAbsGateLufs)
            {
                // gated hop: decay everything, then add this hop
                for (auto& w : bins_) w *= decayPerHop_;
                for (auto& w : binPow_) w *= decayPerHop_;
                plainPow_ *= decayPerHop_; plainW_ *= decayPerHop_;
                peak_ = (float) (peak_ * decayPerHop_);
                const int bi = juce::jlimit (0, kBins - 1, (int) std::floor (lufs - kBinLoLufs));
                bins_[(size_t) bi]   += 1.0;
                binPow_[(size_t) bi] += mp;
                heardHops_ += 1.0;
                plainPow_ += hopPlainPow_ / (double) hopSamples_;
                plainW_   += 1.0;
                if (hopPeak_ > peak_) peak_ = hopPeak_;
            }
        }
        hopFill_ = 0; hopKPow_ = hopPlainPow_ = 0.0; hopPeak_ = 0.0f;
        publish (false);
    }

    // ---- published copy (message thread reads) ----------------------------
    mutable juce::SpinLock pubLock_;
    Snapshot pub_;
    std::array<double, kBins> pubBins_ {}, pubBinPow_ {};
    double pubHeardHops_ = 0.0, pubPlainPow_ = 0.0, pubPlainW_ = 0.0;
    float  pubPeak_ = 0.0f;

    Snapshot compute() const noexcept
    {
        Snapshot s;
        s.heardSeconds  = (float) (heardHops_ * kHopSeconds);
        s.windowSeconds = (float) juce::jmin ((double) s.heardSeconds, kEffectiveWindowSeconds);
        s.known = s.heardSeconds >= kHeardFloorSeconds;
        if (! s.known) return s;   // every number stays NaN
        // absolute-gated weighted mean (bins hold only hops above the absolute
        // gate); the K-power sums make it exact, the bins only decide the gate
        double w = 0.0, pw = 0.0;
        for (int i = 0; i < kBins; ++i) { w += bins_[(size_t) i]; pw += binPow_[(size_t) i]; }
        if (w <= 0.0 || pw <= 0.0) { s.known = false; return s; }
        const double absMean = -0.691 + 10.0 * std::log10 (pw / w);
        // relative gate: bins below absMean - 10 LU are dropped
        const double rel = absMean + kRelGateLu;
        double w2 = 0.0, pw2 = 0.0;
        for (int i = 0; i < kBins; ++i)
            if (kBinLoLufs + i + 0.5 >= rel) { w2 += bins_[(size_t) i]; pw2 += binPow_[(size_t) i]; }
        s.lufsGated = (float) (w2 > 0.0 ? -0.691 + 10.0 * std::log10 (pw2 / w2) : absMean);
        // percentiles over the absolute-gated hops (verse vs chorus)
        auto pct = [&] (double q) -> float
        {
            double acc = 0.0; const double target = q * w;
            for (int i = 0; i < kBins; ++i) { acc += bins_[(size_t) i]; if (acc >= target) return (float) (kBinLoLufs + i + 0.5); }
            return (float) (kBinLoLufs + kBins - 0.5);
        };
        s.p10 = pct (0.10); s.p50 = pct (0.50); s.p90 = pct (0.90);
        s.rmsDb  = (float) (plainW_ > 0.0 && plainPow_ > 0.0 ? 10.0 * std::log10 (plainPow_ / plainW_) : -200.0);
        s.peakDb = (float) (peak_ > 0.0f ? 20.0 * std::log10 (peak_) : -200.0);
        s.crestDb = s.peakDb - s.rmsDb;
        return s;
    }

    void publish (bool force) noexcept
    {
        // TRY-lock on the audio thread: if the message thread is mid-read,
        // this hop's publish is skipped and the next one carries it.
        if (force) pubLock_.enter(); else if (! pubLock_.tryEnter()) return;
        pub_ = compute();
        pubBins_ = bins_; pubBinPow_ = binPow_; pubHeardHops_ = heardHops_; pubPlainPow_ = plainPow_; pubPlainW_ = plainW_; pubPeak_ = peak_;
        pubLock_.exit();
    }
};

} // namespace echojay
