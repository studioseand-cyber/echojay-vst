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
// Statistics (the smallest set that serves C7 and L2): gated level (LUFS on
// the K-weighted chain in/out, dBFS RMS on the plain slot legs, see
// Weighting), p10 / p50 / p90 of the 400 ms momentary level over the
// absolute-gated hops (L2 reasons about p90, because reduction is set by
// the loud passages), plain RMS and a recent peak (both decayed) for crest.
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
    // ---- weighting: what the number IS ------------------------------------
    // Chain input and output are K-weighted (LUFS): C7 wants perceived
    // loudness in and out. Slot legs are PLAIN (dBFS RMS): a compressor's
    // detector never hears K-weighting, so its input level and its measured
    // reduction (out minus in, the filter cancels anyway) belong in the units
    // its threshold is set in. Plain also costs a quarter of K per sample,
    // which at twenty instances is the difference between a rounding error
    // and a percent of a core (measured 17 Aug 2026).
    enum class Weighting { K, Plain };
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
        bool  kWeighted = false;      // levelDb and the percentiles are LUFS (true) or dBFS RMS (false)
        float heardSeconds  = 0.0f;   // gated audio heard since reset, undecayed
        float windowSeconds = 0.0f;   // how much of it the estimate describes
        // Everything below is NaN while !known. Read known first.
        float levelDb = std::numeric_limits<float>::quiet_NaN();   // gated: LUFS (K) or dBFS RMS (Plain)
        float p10 = std::numeric_limits<float>::quiet_NaN();
        float p50 = std::numeric_limits<float>::quiet_NaN();
        float p90 = std::numeric_limits<float>::quiet_NaN();
        float rmsDb   = std::numeric_limits<float>::quiet_NaN();   // plain, per-channel-mean, decayed
        float peakDb  = std::numeric_limits<float>::quiet_NaN();   // recent sample peak, decayed hold
        float crestDb = std::numeric_limits<float>::quiet_NaN();   // peakDb - rmsDb
    };

    explicit LevelTally (Weighting w = Weighting::K) : weighting_ (w) { std::fill (bins_.begin(), bins_.end(), 0.0); }
    Weighting weighting() const noexcept { return weighting_; }

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
            if (weighting_ == Weighting::K)
            {
                // K-weighted power, summed over channels as BS.1770 does
                const double kl = biquad (l, zl1_, k1_), kr = biquad (r, zr1_, k1_);
                const double kl2 = biquad ((float) kl, zl2_, k2_), kr2 = biquad ((float) kr, zr2_, k2_);
                hopKPow_ += kl2 * kl2 + kr2 * kr2;
            }
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
        // The WORKING state as last published, sparse: only non-empty bins,
        // each as [bin, weight (3 dp), mean level of the bin (0.1 dB)], plus
        // heard, recent peak and the decayed plain power. A few hundred
        // bytes per tally; the reader re-derives everything else.
        const juce::SpinLock::ScopedLockType lock (pubLock_);
        auto* o = new juce::DynamicObject();
        o->setProperty ("w", weighting_ == Weighting::K ? "K" : "plain");
        juce::Array<juce::var> sparse;
        for (int i = 0; i < kBins; ++i)
        {
            const double w = pubBins_[(size_t) i];
            if (w <= 1e-6) continue;
            const double meanPow = pubBinPow_[(size_t) i] / w;
            const double db = meanPow > 0.0 ? 10.0 * std::log10 (meanPow) : -200.0;
            juce::Array<juce::var> e;
            e.add (i); e.add (std::round (w * 1000.0) / 1000.0); e.add (std::round (db * 10.0) / 10.0);
            sparse.add (juce::var (e));
        }
        o->setProperty ("bins", sparse);
        o->setProperty ("heard", std::round (pubHeardHops_ * 10.0) / 10.0);
        o->setProperty ("plainDb", pubPlainW_ > 0.0 && pubPlainPow_ > 0.0 ? std::round (100.0 * std::log10 (pubPlainPow_ / pubPlainW_)) / 10.0 : -200.0);
        o->setProperty ("plainW", std::round (pubPlainW_ * 1000.0) / 1000.0);
        o->setProperty ("peakDb", pubPeak_ > 0.0f ? std::round (200.0 * std::log10 (pubPeak_)) / 10.0 : -200.0);
        return juce::var (o);
    }
    // Any thread before audio, or message thread: adopted at the next push.
    bool fromVar (const juce::var& v)
    {
        auto* o = v.getDynamicObject();
        if (o == nullptr) return false;
        auto* b = o->getProperty ("bins").getArray();
        if (b == nullptr) return false;
        // A K tally never adopts a plain one or vice versa: the units differ
        const juce::String w = o->getProperty ("w").toString();
        if (w.isNotEmpty() && (w == "K") != (weighting_ == Weighting::K)) return false;
        PendingRestore p;
        for (auto& ev : *b)
        {
            auto* e = ev.getArray();
            if (e == nullptr || e->size() < 3) continue;
            const int i = (int) (*e)[0];
            if (i < 0 || i >= kBins) continue;
            const double wt = juce::jmax (0.0, (double) (*e)[1]);
            const double db = (double) (*e)[2];
            p.bins[(size_t) i]   = wt;
            p.binPow[(size_t) i] = wt * std::pow (10.0, db / 10.0);
        }
        p.heardHops = juce::jmax (0.0, (double) o->getProperty ("heard"));
        p.plainW    = juce::jmax (0.0, (double) o->getProperty ("plainW"));
        {
            const double pdb = (double) o->getProperty ("plainDb");
            p.plainPow = pdb > -190.0 ? p.plainW * std::pow (10.0, pdb / 10.0) : 0.0;
        }
        {
            const double kdb = (double) o->getProperty ("peakDb");
            p.peak = kdb > -190.0 ? (float) std::pow (10.0, kdb / 20.0) : 0.0f;
        }
        {
            const juce::SpinLock::ScopedLockType lock (restoreLock_);
            pending_ = p;
            restoreRequested_.store (true, std::memory_order_release);
        }
        return true;
    }

private:
    // ---- audio-thread state ------------------------------------------------
    const Weighting weighting_;
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
        // K: mean K-power over the hop, L+R summed (BS.1770). Plain: mean
        // per-channel power (dBFS RMS convention). Both go through the same
        // 400 ms block, gate, histogram and decay; only the unit differs.
        const double hopK = (weighting_ == Weighting::K ? hopKPow_ : hopPlainPow_) / (double) hopSamples_;
        ring_[(size_t) ringPos_] = hopK;
        ringPos_ = (ringPos_ + 1) % kHopsPerBlock;
        if (ringFill_ < kHopsPerBlock) ++ringFill_;
        if (ringFill_ == kHopsPerBlock)
        {
            double mp = 0.0;
            for (auto v : ring_) mp += v;
            mp /= (double) kHopsPerBlock;
            const double lufs = mp > 0.0 ? offsetDb() + 10.0 * std::log10 (mp) : -200.0;
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

    // LUFS carries the BS.1770 -0.691 offset; plain dBFS RMS carries none.
    double offsetDb() const noexcept { return weighting_ == Weighting::K ? -0.691 : 0.0; }

    Snapshot compute() const noexcept
    {
        Snapshot s;
        s.kWeighted     = weighting_ == Weighting::K;
        s.heardSeconds  = (float) (heardHops_ * kHopSeconds);
        s.windowSeconds = (float) juce::jmin ((double) s.heardSeconds, kEffectiveWindowSeconds);
        s.known = s.heardSeconds >= kHeardFloorSeconds;
        if (! s.known) return s;   // every number stays NaN
        // absolute-gated weighted mean (bins hold only hops above the absolute
        // gate); the K-power sums make it exact, the bins only decide the gate
        double w = 0.0, pw = 0.0;
        for (int i = 0; i < kBins; ++i) { w += bins_[(size_t) i]; pw += binPow_[(size_t) i]; }
        if (w <= 0.0 || pw <= 0.0) { s.known = false; return s; }
        const double absMean = offsetDb() + 10.0 * std::log10 (pw / w);
        // relative gate: bins below absMean - 10 LU are dropped
        const double rel = absMean + kRelGateLu;
        double w2 = 0.0, pw2 = 0.0;
        for (int i = 0; i < kBins; ++i)
            if (kBinLoLufs + i + 0.5 >= rel) { w2 += bins_[(size_t) i]; pw2 += binPow_[(size_t) i]; }
        s.levelDb = (float) (w2 > 0.0 ? offsetDb() + 10.0 * std::log10 (pw2 / w2) : absMean);
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
