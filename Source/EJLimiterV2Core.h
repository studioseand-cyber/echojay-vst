#pragma once
/*
    EJLimiterV2Core.h  (session L, 7 Oct 2026)  -  the Limiter v2 engine. Target: FabFilter Pro-L 2, Transparent.

    Our own algorithm, tuned to MEASUREMENTS of Pro-L 2's behaviour (docs/LIMITER_V2_PLAN.md). JUCE-free and
    allocation-free in process(), so it builds inside the offline harness tree and drops into EedLimiterProcessor
    unchanged.

    THE SIGNAL PATH, per sample, per channel c:
      1. input gain (the loudness push), eased over 20 ms.
      2. DETECTION: the true-peak envelope p_c = max |value| among the sample and its 7 inter-sample points (8x,
         96-tap Kaiser-windowed sinc per phase - the same class as the harness's independent meter: a 48-tap
         detector under-read full-band noise by 0.5 dB against it, from fractional-delay error near Nyquist),
         or plain |x| when true peak is off. The interpolator reads kDelay
         samples late; the audio delay absorbs that and the latency reports it.
      3. REQUIRED GAIN r_c = min(1, ceil_det / p_c), ceil_det = the ceiling less a small margin under true peak.
         CHANNEL LINKING: r = the smaller of the two (fully linked, link = 1) blended in dB with the channel's own
         (link = 0 is independent). Linked by default: an unlinked transient pulls the image toward the quieter side.
      4. HELD MINIMUM over the last W samples (a monotonic deque: O(1) per sample, fixed storage). This is the
         lookahead: the audio is delayed by W-1, so the gain is already down when the peak arrives.
      5. PROGRAM-DEPENDENT RELEASE, in dB of reduction d = -20log10(held):
              fast limb  e_f = max(d, e_f * decay_fast)            - exponential recovery after a transient
              slow limb  e_s -> d_win through a one-pole (attack tau_sa rising, release tau_slow falling), where
                         d_win is the LARGEST d over the last slowWindowMs - so a sustained tone, whose d pulses
                         once per half-cycle, charges the floor to its full reduction and is then held at a GAIN
                         (no waveform riding, no LF distortion), while a lone transient charges it only a little
              env        = max(e_f, e_s)
         A lone transient recovers on the fast limb; sustained over-level raises e_s, and after the next transient
         the fast limb drops only to that floor, which then recovers slowly. "Fast after transients, slow on
         sustained level" (plan §3) is these numbers.
      6. SMOOTHING: the linear gain of env through S cascaded moving averages of length M (a B-spline window of
         order S; S = 3 is C1-smooth). Support K = S(M-1)+1 samples. THE GUARANTEE: the audio delay is K-1 and the
         held-min window is W = K, so every envelope value inside the kernel when the peak sample is output has
         that peak inside its min window - the smoothed gain at the peak is <= the required gain, exactly. The
         ceiling is held in the detector's domain without any hard clipping in the path; a sample-domain clip at
         the ceiling remains as a last resort and does nothing while the detector is right.
      7. the gain is applied to the delayed audio.
      8. POST-CHECK (true peak only): the guarantee in 6 is exact for the detector's envelope of the INPUT, but the
         output is g[n]x[n] and its inter-sample values are not g times the input's where g varies across the
         interpolator's span - on white noise under a steep gain change the residual reached +0.3 dBTP. So a
         second, short (postMs) stage of the same construction reads the OUTPUT's true peak and trims it. Its own
         gain varies far less, so its residual is second order; the harness's independent meter is the judge.

    LATENCY = (K - 1) + (true peak ? kDelay + (K2 - 1) + kDelay : 0) samples, from latencySamples().

    STYLES are Tunings of the numbers above (lookahead, S, the three release constants, the link, the margin).
    `transparent()` is the only one so far; its numbers are STARTING POINTS until the Pro-L 2 renders are measured.
*/
#include "EJLimiterMeterTap.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace echojay {
namespace limv2 {

struct Tuning
{
    double lookaheadMs    = 5.0;    // the smoothing window = the pre-dip length the harness measures as pre(ms)
    int    smoothStages   = 3;      // cascaded moving averages: 1 box, 2 triangle, 3 quadratic B-spline
    double fastReleaseMs  = 40.0;   // time constant of the fast limb (dB-domain exponential)
    double slowReleaseMs  = 500.0;  // time constant of the slow limb's recovery
    double slowAttackMs   = 60.0;   // how quickly sustained over-level raises the slow floor
    double slowFraction   = 1.0;    // the slow floor's target as a fraction of the required reduction (Pro-L 2 measured ~0.72)
    double slowFraction2  = 1.0;    // a SECOND, slower charge of the floor toward this fraction (sustained material keeps charging)
    double slowAttack2Ms  = 0.0;    // its time constant; 0 disables the second stage
    double slowWindowMs   = 20.0;   // the slow limb charges from the largest reduction over this window (> one LF cycle)
    double link           = 1.0;    // 1 = fully linked channels, 0 = independent
    double tpMarginDb     = 0.1;    // detector margin under the ceiling with true peak on
    double postMs         = 1.0;    // the post-check stage's window (true peak only); 0 disables it
    double maxLookaheadMs = 20.0;   // storage sized once, in prepare()
};

// CLEAN: the first v2 behaviour - holds sustained material at a steady gain (THD below -100 dB on tones), a 5 ms
// smooth window, 40 ms fast limb, a slow floor charged to the full reduction over a 20 ms window. Not exposed yet.
inline Tuning clean() { return Tuning {}; }

// TRANSPARENT: tuned to the Pro-L 2 Transparent measurements of 7 Oct 2026 (docs/limiter_ab/SESSION_L_NOTES.md),
// step by step (C1..C6 in the overnight run); each value is a measured number, not a knob label.
inline Tuning transparent()
{
    Tuning t;
    t.slowFraction  = 0.72;    // C1: the floor settles at ~72 % of the required reduction (10 ms burst 8 %, 1 s 72 %)
    t.slowAttackMs  = 150.0;   // C1: the floor charges with a 120-185 ms constant
    t.slowReleaseMs = 180.0;   // C1: and decays with 160-185 ms (measured on every limb)
    t.slowWindowMs  = 1.0;     // C1: charged from the reduction itself, so an LF tone gets the shallower floor Pro-L 2 shows
    t.fastReleaseMs = 0.3;     // C2: the fast part is instant - after a burst Pro-L 2 is back within 0.6 dB in < 0.33 ms
    t.lookaheadMs   = 0.3;     // C3: no pre-dip beyond 0.33 ms; a 1-sample impulse gets a ~0.5 ms dip and lands at the ceiling
    t.smoothStages  = 1;       // C3: a box of that width is all the smoothing Pro-L 2 shows
    t.link          = 0.75;    // C5: a left-only burst dips the right channel 75 % as much (dB), measured on panned_transient
    t.tpMarginDb    = 0.05;    // C6: Pro-L 2 lands at -0.01..-0.04 dBTP; the 96-tap detector and the post-check keep overs at zero
    t.slowWindowMs  = 4.0;     // C7c: the floor's source holds across most of an LF cycle (8 ms cost 0.11 LU on the hot mix; Pro-L 2: 97 % at 997 Hz, 81 % at 50 Hz)
    t.slowFraction2 = 1.0;     // C7: sustained material keeps charging the floor toward the full reduction ...
    t.slowAttack2Ms = 1200.0;  // C7: ... slowly: 72 % after 1 s (the first stage), ~97 % after 4 s (measured on tone_997)
    return t;
}

// 8x true-peak interpolator: 8 phases of a 96-tap Kaiser (beta 9) windowed sinc. Fixed arrays, no allocation.
// The history is kept twice over (hist[p] and hist[p + kTaps]) so every phase is one CONTIGUOUS dot product the
// compiler can vectorise, with eight independent accumulators so it may; a circular index in the inner loop cost
// 15x the old limiter's whole process (measured 8 Oct 2026).
struct TruePeak8x
{
    static constexpr int kPhases = 8, kTaps = 96, kDelay = kTaps / 2;
    float coef[kPhases][kTaps] {}; float hist[2 * kTaps] {}; int pos = 0;
    static double i0 (double x) { double s = 1, t = 1; for (int k = 1; k < 60; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-14 * s) break; } return s; }
    void prepare() noexcept
    {
        const double pi = 3.14159265358979323846, beta = 9.0, i0b = i0 (beta);
        for (int ph = 0; ph < kPhases; ++ph)
        {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double x = ((double) k - (kTaps / 2 - 0.5)) - (double) ph / kPhases + 0.5;
                const double sinc = x == 0.0 ? 1.0 : std::sin (pi * x) / (pi * x);
                const double r = 2.0 * x / (double) kTaps; const double w = std::abs (r) >= 1.0 ? 0.0 : i0 (beta * std::sqrt (1.0 - r * r)) / i0b;
                coef[ph][k] = (float) (sinc * w); sum += sinc * w;
            }
            // tap k multiplies the sample k back in time; stored REVERSED so the window reads forward in memory
            float tmp[kTaps]; for (int k = 0; k < kTaps; ++k) tmp[k] = (float) (coef[ph][k] / sum);
            for (int k = 0; k < kTaps; ++k) coef[ph][k] = tmp[kTaps - 1 - k];
        }
        reset();
    }
    void reset() noexcept { for (auto& h : hist) h = 0.0f; pos = 0; }
    inline float maxAbs (float x) noexcept
    {
        hist[pos] = x; hist[pos + kTaps] = x;
        const float* w = hist + pos + 1;   // the last kTaps samples, oldest first, contiguous
        float m = 0.0f;
        for (int ph = 0; ph < kPhases; ++ph)
        {
            const float* c = coef[ph];
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
            for (int k = 0; k < kTaps; k += 8) { a0 += c[k] * w[k]; a1 += c[k + 1] * w[k + 1]; a2 += c[k + 2] * w[k + 2]; a3 += c[k + 3] * w[k + 3]; a4 += c[k + 4] * w[k + 4]; a5 += c[k + 5] * w[k + 5]; a6 += c[k + 6] * w[k + 6]; a7 += c[k + 7] * w[k + 7]; }
            m = std::max (m, std::abs (((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7))));
        }
        pos = (pos + 1) % kTaps; return m;
    }
};

// Running minimum over the last W values pushed: a monotonic deque in fixed storage.
struct RunningMin
{
    std::vector<float> val; std::vector<long long> idx; int cap = 0, head = 0, tail = 0, n = 0; long long t = 0; int W = 1;
    void prepare (int capacity) { cap = std::max (2, capacity + 1); val.assign ((size_t) cap, 1.0f); idx.assign ((size_t) cap, 0); reset(); }
    void reset() noexcept { head = tail = n = 0; t = 0; }
    void setWindow (int w) noexcept { W = std::max (1, std::min (w, cap - 1)); }
    inline float push (float v) noexcept
    {
        while (n > 0) { const int last = (tail + cap - 1) % cap; if (val[(size_t) last] >= v) { tail = last; --n; } else break; }
        val[(size_t) tail] = v; idx[(size_t) tail] = t; tail = (tail + 1) % cap; ++n;
        while (n > 0 && idx[(size_t) head] <= t - W) { head = (head + 1) % cap; --n; }
        ++t; return val[(size_t) head];
    }
};

// A moving average of length M with a double accumulator (no drift at float precision over a session).
struct MovingAverage
{
    std::vector<float> ring; int cap = 0, pos = 0, M = 1; double sum = 0.0;
    void prepare (int capacity) { cap = std::max (1, capacity); ring.assign ((size_t) cap, 1.0f); reset(); }
    void reset() noexcept { std::fill (ring.begin(), ring.end(), 1.0f); pos = 0; sum = (double) M; }
    // A length change keeps the current average: the ring is refilled with it, so a dial move mid-reduction does
    // not snap the gain to unity (which is a click).
    void setLength (int m) noexcept { const float cur = (float) (sum / (double) std::max (1, M)); M = std::max (1, std::min (m, cap)); std::fill (ring.begin(), ring.end(), cur); pos = 0; sum = (double) cur * (double) M; }
    inline float push (float v) noexcept
    {
        const int old = (pos + cap - M) % cap;   // the value leaving the window
        sum += (double) v - (double) ring[(size_t) old]; ring[(size_t) pos] = v; pos = (pos + 1) % cap;
        return (float) (sum / (double) M);
    }
};

class Core
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate, const Tuning& t)
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0; tuning_ = t;
        const int maxK = (int) std::ceil (t.maxLookaheadMs * 0.001 * sr_) + 8 * std::max (1, t.smoothStages);
        const int maxSlow = (int) std::ceil (std::max (1.0, t.slowWindowMs) * 0.001 * sr_) + 1;
        const int maxPost = (int) std::ceil (std::max (0.0, t.postMs) * 0.001 * sr_) + 8;
        for (int c = 0; c < kMaxChannels; ++c)
        {
            tp_[c].prepare(); held_[c].prepare (maxK + 1); for (auto& m : ma_[c]) m.prepare (maxK + 1); slowWin_[c].prepare (maxSlow + 1);
            tp2_[c].prepare(); held2_[c].prepare (maxPost + 1); ma2_[c].prepare (maxPost + 1);
        }
        const int maxDelay = maxK + TruePeak8x::kDelay + 1;
        for (auto& d : delay_) d.assign ((size_t) maxDelay, 0.0f);
        const int maxDelay2 = maxPost + TruePeak8x::kDelay + 1;
        for (auto& d : delay2_) d.assign ((size_t) maxDelay2, 0.0f);
        gaRing_.assign ((size_t) maxDelay2, 1.0f);
        delayCap_ = maxDelay; delayCap2_ = maxDelay2;
        // the fixed-latency maximum: the largest window the storage allows, true peak on, post-check on
        { const int S = std::max (1, std::min (t.smoothStages, 4)); const int laMax = std::max (1, (int) std::lround (t.maxLookaheadMs * 0.001 * sr_)); const int M = (laMax + S - 1) / S + 1; const int Kmax = S * (M - 1) + 1; const int K2 = t.postMs > 0.0 ? std::max (2, (int) std::lround (t.postMs * 0.001 * sr_) + 1) : 1; maxLatency_ = (Kmax - 1) + TruePeak8x::kDelay + (K2 > 1 ? (K2 - 1) + TruePeak8x::kDelay : 0); }
        for (auto& d : scDelay_) d.assign ((size_t) maxLatency_ + 1, 0.0f);
        scDelayCap_ = maxLatency_ + 1; prepared_ = true; ceilLin_ = ceilTarget_; applyTuning(); reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            tp_[c].reset(); held_[c].reset(); for (auto& m : ma_[c]) m.reset(); slowWin_[c].reset(); eFast_[c] = eSlow_[c] = eSlow2_[c] = 0.0; std::fill (delay_[c].begin(), delay_[c].end(), 0.0f);
            tp2_[c].reset(); held2_[c].reset(); ma2_[c].reset(); std::fill (delay2_[c].begin(), delay2_[c].end(), 0.0f);
        }
        std::fill (gaRing_.begin(), gaRing_.end(), 1.0f);
        for (int c = 0; c < kMaxChannels; ++c) { std::fill (scDelay_[c].begin(), scDelay_[c].end(), 0.0f); hpfZ_[c][0] = hpfZ_[c][1] = 0.0; }
        wpos_ = 0; wpos2_ = 0; scPos_ = 0; gainNow_ = inputGain_; grDb_ = 0.0f; bypassMix_ = bypassTarget_; ceilLin_ = ceilTarget_;
    }

    void setTuning (const Tuning& t) { tuning_ = t; applyTuning(); }
    const Tuning& tuning() const noexcept { return tuning_; }
    void setInputGainDb (double db) noexcept { inputGain_ = (float) std::pow (10.0, db / 20.0); }
    // The ceiling is smoothed over 20 ms toward its target; the detector always uses the LOWER of target and current,
    // so it is never above the clip while the clip ramps, and the output level ramps instead of stepping.
    void setCeilingDb (double db) noexcept { ceilTarget_ = (float) std::pow (10.0, db / 20.0); if (! prepared_) { ceilLin_ = ceilTarget_; } applyTuning(); }
    void setTruePeak (bool on) noexcept { truePeak_ = on; applyTuning(); }
    bool truePeak() const noexcept { return truePeak_; }

    // The latency of the CURRENT settings (window, true peak), or - with setFixedLatency (true), the plugin's choice -
    // the maximum over every setting the prepare()d storage allows, identical whatever the dials say. The detector
    // is delayed by the difference so the timing of the gain against the audio is unchanged.
    int latencySamples() const noexcept { return fixedLatency_ ? fixedLatency_samples() : naturalLatency(); }
    int naturalLatency() const noexcept { return (K_ - 1) + (truePeak_ ? TruePeak8x::kDelay + postLatency() : 0); }
    int postLatency() const noexcept { return (truePeak_ && K2_ > 1) ? (K2_ - 1) + TruePeak8x::kDelay : 0; }
    int fixedLatency_samples() const noexcept { return maxLatency_; }
    void setFixedLatency (bool on) noexcept { fixedLatency_ = on; applyTuning(); }
    bool fixedLatency() const noexcept { return fixedLatency_; }
    float gainReductionDb() const noexcept { return grDb_; }   // the most negative gain of the last block, dB (peak GR)
    float currentInputGain() const noexcept { return gainNow_; }

    // A 2nd-order high-pass on the DETECTOR only (0 = off): what the detector cannot hear can exceed the ceiling,
    // which the schema says in as many words; the sample-domain clip remains the last resort.
    void setSidechainHpfHz (double hz) noexcept { scHpfHz_ = std::max (0.0, hz); applyTuning(); }

    // Bypass keeps the delay and the detector running and crossfades the applied gain to unity over 10 ms, so
    // neither edge of a bypass clicks and the track never moves in time.
    void setBypassed (bool b) noexcept { bypassTarget_ = b ? 0.0f : 1.0f; }

    // The panel's data path (EedLimiterPanelV2): when attached, every sample is pushed with its gained input, its
    // output, the gain applied and the detector's true-peak values the engine already has. nullptr = not attached.
    void setMeterTap (MeterTap* t) noexcept { tap_ = t; }

    // In place. numCh 1 or 2; a mono input is processed as one channel.
    void process (float* const* ch, int numCh, int n) noexcept
    {
        numCh = std::max (1, std::min (numCh, kMaxChannels));
        const float gCoef = gainCoef_; float gMin = 1.0f;
        const int scD = std::max (0, maxLatency_ - naturalLatency());   // the detector's delay under fixed latency
        for (int i = 0; i < n; ++i)
        {
            gainNow_ += (inputGain_ - gainNow_) * gCoef;
            if (std::abs (ceilLin_ - ceilTarget_) > 1e-7f) { ceilLin_ += (ceilTarget_ - ceilLin_) * gCoef; if (std::abs (ceilLin_ - ceilTarget_) < 1e-6f) ceilLin_ = ceilTarget_; }
            const float ceilNow = std::min (ceilLin_, ceilTarget_);
            const float ceilDetNow = truePeak_ ? ceilNow * tpMarginLin_ : ceilNow;
            bypassMix_ += (bypassTarget_ - bypassMix_) * bypassCoef_;
            float x[kMaxChannels] { 0.0f, 0.0f }, r[kMaxChannels] { 1.0f, 1.0f };
            float tpInMax = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                x[c] = ch[c][i] * gainNow_;
                // the detector input: delayed under fixed latency (so the gain lands on the same sample it would
                // without it), then high-passed when asked
                float sc = x[c];
                if (fixedLatency_) { float* sd = scDelay_[c].data(); sd[scPos_] = x[c]; sc = sd[(scPos_ + scDelayCap_ - scD) % scDelayCap_]; }
                if (scHpfOn_) { const double y = hpfB0_ * sc + hpfZ_[c][0]; hpfZ_[c][0] = hpfB1_ * sc - hpfA1_ * y + hpfZ_[c][1]; hpfZ_[c][1] = hpfB2_ * sc - hpfA2_ * y; sc = (float) y; }
                const float p = truePeak_ ? tp_[c].maxAbs (sc) : std::abs (sc);
                tpInMax = std::max (tpInMax, p);
                r[c] = p > ceilDetNow ? ceilDetNow / p : 1.0f;
            }
            if (fixedLatency_) scPos_ = (scPos_ + 1) % scDelayCap_;
            // channel linking (dB blend of own and linked)
            if (numCh > 1)
            {
                const float linked = std::min (r[0], r[1]);
                if (tuning_.link >= 1.0) r[0] = r[1] = linked;
                else if (tuning_.link > 0.0) for (int c = 0; c < numCh; ++c) r[c] = std::pow (linked, (float) tuning_.link) * std::pow (r[c], (float) (1.0 - tuning_.link));
            }
            float g[kMaxChannels] { 1.0f, 1.0f };
            for (int c = 0; c < numCh; ++c)
            {
                if (c == 1 && tuning_.link >= 1.0) { g[1] = g[0]; break; }   // identical by construction: one envelope
                const float held = held_[c].push (r[c]);
                const double d = held < 1.0f ? -20.0 * std::log10 ((double) held) : 0.0;
                const float heldSlow = slowWin_[c].push (held);
                const double dWin = heldSlow < 1.0f ? -20.0 * std::log10 ((double) heldSlow) : 0.0;
                eFast_[c] = std::max (d, eFast_[c] * decayFast_);
                const double slowTarget = dWin * tuning_.slowFraction;
                eSlow_[c] += (slowTarget - eSlow_[c]) * (slowTarget > eSlow_[c] ? coefSlowAtk_ : coefSlowRel_);
                double floor = eSlow_[c];
                if (coefSlowAtk2_ > 0.0) { const double t2 = dWin * tuning_.slowFraction2; eSlow2_[c] += (t2 - eSlow2_[c]) * (t2 > eSlow2_[c] ? coefSlowAtk2_ : coefSlowRel_); floor = std::max (floor, eSlow2_[c]); }
                const double env = std::max (eFast_[c], floor);
                float ge = (float) std::pow (10.0, -env / 20.0);
                for (int s = 0; s < S_; ++s) ge = ma_[c][s].push (ge);
                g[c] = ge;
            }
            if (numCh > 1 && tuning_.link >= 1.0) { held_[1].push (r[1]); }   // keep the second deque's clock in step (unused output)
            // the delayed audio (by the fixed maximum when asked), then the gain, crossfaded to unity under bypass
            float y[kMaxChannels] { 0.0f, 0.0f }; float gaMin = 1.0f;
            const int audioDelay = fixedLatency_ ? maxLatency_ - postLatency() : delaySamples_;
            for (int c = 0; c < numCh; ++c)
            {
                float* dl = delay_[c].data(); dl[wpos_] = x[c];
                const int rp = (wpos_ + delayCap_ - audioDelay) % delayCap_;
                const float ga = bypassMix_ >= 1.0f ? g[c] : 1.0f + (g[c] - 1.0f) * bypassMix_;   // exact at the default: no rounding in the gain
                y[c] = dl[rp] * ga;
                gaMin = std::min (gaMin, ga);
            }
            wpos_ = (wpos_ + 1) % delayCap_;
            // the gain REPORTED for a block is the gain applied to the samples that LEAVE in that block: stage 1's
            // gain rides through the post stage's delay before it meets g2 (gaRing_), so a meter reading and a
            // measurement of the same block agree (limiter_v2_core_test proves it)
            float gOut = gaMin;
            // the post-check: the output's own true peak, trimmed by a short stage of the same construction (linked)
            float tpOutMax = 0.0f;
            if (truePeak_ && K2_ > 1)
            {
                float r2 = 1.0f;
                for (int c = 0; c < numCh; ++c) { const float p = tp2_[c].maxAbs (y[c]); tpOutMax = std::max (tpOutMax, p); r2 = std::min (r2, p > ceilDetNow ? ceilDetNow / p : 1.0f); }
                const float g2raw = ma2_[0].push (held2_[0].push (r2)); const float g2 = bypassMix_ >= 1.0f ? g2raw : 1.0f + (g2raw - 1.0f) * bypassMix_;
                gaRing_[(size_t) wpos2_] = gaMin;
                const int rp2 = (wpos2_ + delayCap2_ - delaySamples2_) % delayCap2_;
                for (int c = 0; c < numCh; ++c)
                {
                    float* dl = delay2_[c].data(); dl[wpos2_] = y[c];
                    y[c] = dl[rp2] * g2;
                }
                gOut = gaRing_[(size_t) rp2] * g2;
                wpos2_ = (wpos2_ + 1) % delayCap2_;
            }
            gMin = std::min (gMin, gOut);
            const float clipAt = bypassMix_ < 1.0f ? 1.0e9f : ceilLin_;   // the clip is the limiter's; under bypass the signal passes
            for (int c = 0; c < numCh; ++c) ch[c][i] = std::max (-clipAt, std::min (clipAt, y[c]));
            if (tap_ != nullptr) tap_->push (x[0], numCh > 1 ? x[1] : x[0], ch[0][i], numCh > 1 ? ch[1][i] : ch[0][i], gOut, tpInMax, tpOutMax > 0.0f ? tpOutMax : std::max (std::abs (ch[0][i]), numCh > 1 ? std::abs (ch[1][i]) : 0.0f));
        }
        grDb_ = gMin < 1.0f ? 20.0f * std::log10 (gMin) : 0.0f;
    }

private:
    void applyTuning() noexcept
    {
        S_ = std::max (1, std::min (tuning_.smoothStages, 4));
        const int la = std::max (1, (int) std::lround (std::min (tuning_.lookaheadMs, tuning_.maxLookaheadMs) * 0.001 * sr_));
        const int M = (la + S_ - 1) / S_ + 1;            // per-stage length so the support covers the lookahead
        K_ = S_ * (M - 1) + 1;                            // kernel support
        const int K2 = truePeak_ && tuning_.postMs > 0.0 ? std::max (2, (int) std::lround (tuning_.postMs * 0.001 * sr_) + 1) : 1;
        K2_ = K2;
        for (int c = 0; c < kMaxChannels; ++c) { held_[c].setWindow (K_); for (int s = 0; s < S_; ++s) ma_[c][s].setLength (M); slowWin_[c].setWindow (std::max (1, (int) std::lround (tuning_.slowWindowMs * 0.001 * sr_))); held2_[c].setWindow (K2_); ma2_[c].setLength (K2_); }
        delaySamples_ = std::min (delayCap_ - 1, (K_ - 1) + (truePeak_ ? TruePeak8x::kDelay : 0));
        delaySamples2_ = std::min (delayCap2_ - 1, postLatency());
        if (fixedLatency_ && delayCap_ <= maxLatency_) { delay_[0].assign ((size_t) maxLatency_ + 1, 0.0f); delay_[1].assign ((size_t) maxLatency_ + 1, 0.0f); delayCap_ = maxLatency_ + 1; }   // only reachable from prepare(): the fixed delay fits the storage sized there
        tpMarginLin_ = (float) std::pow (10.0, -tuning_.tpMarginDb / 20.0);
        bypassCoef_ = (float) (1.0 - std::exp (-1.0 / (0.01 * sr_)));
        scHpfOn_ = scHpfHz_ > 0.0;
        if (scHpfOn_)
        {   // RBJ 2nd-order high-pass, Q 0.707
            const double w0 = 2.0 * 3.14159265358979323846 * std::min (scHpfHz_, 0.45 * sr_) / sr_, cw = std::cos (w0), sw = std::sin (w0), alpha = sw / (2.0 * 0.70710678);
            const double a0 = 1.0 + alpha; hpfB0_ = (1.0 + cw) / 2.0 / a0; hpfB1_ = -(1.0 + cw) / a0; hpfB2_ = (1.0 + cw) / 2.0 / a0; hpfA1_ = -2.0 * cw / a0; hpfA2_ = (1.0 - alpha) / a0;
        }
        decayFast_   = std::exp (-1.0 / (std::max (0.1, tuning_.fastReleaseMs) * 0.001 * sr_));
        coefSlowRel_ = 1.0 - std::exp (-1.0 / (std::max (1.0, tuning_.slowReleaseMs) * 0.001 * sr_));
        coefSlowAtk_ = 1.0 - std::exp (-1.0 / (std::max (0.1, tuning_.slowAttackMs) * 0.001 * sr_));
        coefSlowAtk2_ = tuning_.slowAttack2Ms > 0.0 ? 1.0 - std::exp (-1.0 / (tuning_.slowAttack2Ms * 0.001 * sr_)) : 0.0;
        gainCoef_ = (float) (1.0 - std::exp (-1.0 / (0.02 * sr_)));
    }

    double sr_ = 48000.0; Tuning tuning_;
    TruePeak8x tp_[kMaxChannels]; RunningMin held_[kMaxChannels], slowWin_[kMaxChannels]; MovingAverage ma_[kMaxChannels][4];
    TruePeak8x tp2_[kMaxChannels]; RunningMin held2_[kMaxChannels]; MovingAverage ma2_[kMaxChannels];
    double eFast_[kMaxChannels] { 0.0, 0.0 }, eSlow_[kMaxChannels] { 0.0, 0.0 }, eSlow2_[kMaxChannels] { 0.0, 0.0 };
    std::vector<float> delay_[kMaxChannels], delay2_[kMaxChannels], gaRing_; int delayCap_ = 1, delaySamples_ = 0, wpos_ = 0, delayCap2_ = 1, delaySamples2_ = 0, wpos2_ = 0;
    int S_ = 3, K_ = 1, K2_ = 1;
    double decayFast_ = 0.0, coefSlowRel_ = 0.0, coefSlowAtk_ = 0.0, coefSlowAtk2_ = 0.0;
    float inputGain_ = 1.0f, gainNow_ = 1.0f, gainCoef_ = 0.0f, ceilLin_ = 1.0f, ceilTarget_ = 1.0f, tpMarginLin_ = 1.0f, grDb_ = 0.0f;
    bool truePeak_ = true, prepared_ = false, fixedLatency_ = false, scHpfOn_ = false;
    int maxLatency_ = 0; std::vector<float> scDelay_[kMaxChannels]; int scDelayCap_ = 1, scPos_ = 0;
    double scHpfHz_ = 0.0, hpfB0_ = 1, hpfB1_ = 0, hpfB2_ = 0, hpfA1_ = 0, hpfA2_ = 0; double hpfZ_[kMaxChannels][2] { { 0, 0 }, { 0, 0 } };
    float bypassTarget_ = 1.0f, bypassMix_ = 1.0f, bypassCoef_ = 0.0f;
    MeterTap* tap_ = nullptr;
};

} // namespace limv2
} // namespace echojay
