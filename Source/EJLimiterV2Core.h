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
    double slowRelease2Ms = 0.0;    // 9 Oct 2026 (styles): the second stage's OWN recovery constant; 0 = slowReleaseMs (Transparent, unchanged)
    double slowWindowMs   = 20.0;   // the slow limb charges from the largest reduction over this window (> one LF cycle)
    double slowCloseMs    = 0.0;    // 8 Oct 2026: a morphological CLOSING of the floor's source - dilate by slowWindowMs, then erode by this
                                    // (the smallest reduction over the last slowCloseMs of the dilated signal): an isolated kick keeps its own
                                    // length (it no longer charges the floor as if it lasted window + kick), while an LF tone's pulses, whose gaps
                                    // the dilation bridged, stay bridged. 0 = no erosion (the plain running maximum)
    double link           = 1.0;    // TRANSIENT link: 1 = the required gain is the smaller of the two channels', 0 = independent
    double linkRelease    = 1.0;    // RELEASE link: 1 = one slow floor for both channels (the deeper), 0 = each channel its own
    double tpMarginDb     = 0.1;    // detector margin under the ceiling with true peak on
    double nyquistMarginDb = 0.75;  // EXTRA margin, scaled by the input's energy fraction at Nyquist (0 for music, ~1 for white noise):
                                    // a truncated sinc under-reads full-band material by ~0.45/sqrt(512) = 0.17 dB, and no real-time
                                    // detector reads it exactly; starts at a fraction of 0.05, so music pays nothing. 0.60 rather than the
                                    // 0.30 the arbiter needs: the loudness loop's guard judges white-noise bursts with a 4x/24-tap meter
                                    // that over-reads high-frequency content by up to 0.33 dB, and that leg must pass as it stands (0.75 since the
                                    // default window became 0.18 ms: at 0.60 that meter read -0.04 against its -0.05 limit)
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
    t.slowReleaseMs = 180.0;   // C1: and decays with 160-185 ms (measured on every limb); Pro-L 2's label for it reads 400
    t.lookaheadMs   = 0.06;    // KICK FIX (8 Oct, afternoon): the window that rides a kick's body the way Pro-L 2 does - its
                               // dip at +3/+8 ms within 7 % (0.18 ms held the gain across the body's dense peaks: 1.2x deeper).
                               // Pro-L 2's LOOKAHEAD label at this behaviour reads 0.18; the knob maps label/3
    t.smoothStages  = 3;       // a B-spline of that support (a box's steps put the limiter's own products at Nyquist)
    t.fastReleaseMs = 0.05;    // KICK FIX: the fast part lets go in a sample or two (t63 0.3 ms, Pro-L 2's; 0.3 ms read 0.7-1.2)
    t.slowWindowMs  = 10.0;    // KICK FIX: the floor's source is a morphological CLOSING (dilate 10 ms, erode 10 ms): an LF tone's
    t.slowCloseMs   = 10.0;    //   pulses are bridged (bass_sustain -7.00 vs Pro-L 2 -7.09), an isolated kick keeps its own length
                               //   (between-hit GR -0.09 vs -0.06; the plain 4 ms maximum read -0.14, the raw reduction lost the bass)
    t.slowFraction2 = 1.0;     // C7: sustained material keeps charging the floor toward the full reduction ...
    t.slowAttack2Ms = 1200.0;  // C7: ... slowly: 72 % after 1 s (the first stage), ~97 % after 4 s (measured on tone_997)
    t.link          = 0.75;    // C5: a left-only burst dips the right channel 75 % as much (dB), measured on panned_transient
    t.linkRelease   = 1.0;     // Pro-L 2's release link at 100 %: one floor for both channels
    t.tpMarginDb    = 0.05;    // C6: Pro-L 2 lands at -0.01..-0.04 dBTP; the detector and the post-check keep overs at zero
    return t;
}

// THE STYLES (9 Oct 2026): three more Tunings of the same engine, our own algorithm, generic names, tuned to Sean's
// Pro-L 2 prints in each style (docs/limiter_ab/renders/styles/<style>/, targets in results/2026-10-09_styles_targets_*.txt,
// sweeps in SESSION_L_NOTES.md). Each is the best MEASURED variant of two sweeps; the third sweep was stopped for session
// A's gate, so the bass HOLD is not matched yet (stated per style). What every style has: Transparent's window, fast part,
// links and margins - the hot mix's kick shape is Pro-L 2's in all three (within 7 % at +1/+3/+8 ms).
inline Tuning modern()
{
    Tuning t = transparent();
    t.slowCloseMs    = 30.0;     // a 30 ms erosion: a kick (dilated 12 ms) VANISHES from the floor's source, a note survives - the
                                 // hot mix's between-hit GR stays at -0.11 (Pro-L 2 -0.08) while the floor can take the whole reduction
    t.slowFraction   = 1.0;      // sustained material held at the full reduction: tone THD -115 / -136 dB (Pro-L 2 -59 / -114: steadier)
    t.slowAttackMs   = 60.0;
    t.slowFraction2  = 1.0;
    t.slowAttack2Ms  = 400.0;
    t.slowRelease2Ms = 1500.0;   // the second stage's own slow release (its "modern" tail)
    // measured (M1): hot -0.29/-0.49/-0.74/-0.27 vs -0.31/-0.44/-0.73/-0.29, t63 0.3 = 0.3, level -10.11 vs -10.07; tone_50 -5.21 vs
    // -5.06, tone_997 -0.53 vs -0.69. NOT matched: bass level -7.39 vs -7.69 and the hold after a note (+20 ms -0.58 vs -1.49, t90
    // 1.3 vs 1069 ms): the 30 ms erosion delays a note's reduction by 30 ms. Next: erosion 15-20 ms with attack 5-10 ms (sweep 3).
    return t;
}
inline Tuning punchy()
{
    Tuning t = transparent();
    t.slowAttackMs   = 30.0;     // a hit charges the first stage within its own length: the body is held (+20 ms -0.47 vs Pro-L 2 -0.46)
    t.slowReleaseMs  = 50.0;     // and let go quickly (Pro-L 2's hot-mix t90 16.7 ms)
    t.slowFraction   = 0.9;
    t.slowAttack2Ms  = 400.0;
    t.slowRelease2Ms = 900.0;    // the long tail on sustained material (bass t90 666 ms)
    // measured (P1): hot -0.30/-0.51/-0.76/-0.47 vs -0.30/-0.49/-0.82/-0.46, t63 0.3 = 0.3. NOT matched: between-hit mean -0.19 vs
    // -0.09 and level -10.23 vs -10.10 (the fast charge leaks between hits), bass -7.41 vs -7.20 with the hold short (+20 ms -0.83 vs
    // -2.04, t90 1.3 vs 666). Next: a short erosion (5-8 ms) so only a hit's own length charges, attack 10 ms (sweep 3).
    return t;
}
inline Tuning allround()
{
    Tuning t = transparent();
    t.slowAttackMs   = 40.0;     // between Transparent and Punchy (+20 ms -0.39 vs Pro-L 2 -0.41)
    t.slowReleaseMs  = 60.0;
    t.slowFraction   = 0.85;
    t.slowAttack2Ms  = 800.0;
    t.slowRelease2Ms = 1000.0;   // the long tail (bass t90 761 ms)
    // measured (A3): hot -0.29/-0.49/-0.74/-0.39 vs -0.30/-0.48/-0.80/-0.41, t63 0.3 = 0.3; bass level -7.28 vs -7.11. NOT matched:
    // between-hit mean -0.15 vs -0.09, level -10.17 vs -10.10, the bass hold (+20 ms -0.64 vs -1.23, t90 1.3 vs 761), tones
    // steadier than Pro-L 2's (THD -45 / -43 vs -22 / -50). Next: erosion 6-10 ms, attack 15 ms (sweep 3).
    return t;
}
// MODE -> Tuning, the schema's values: 0 transparent, 1 punchy, 2 clip (placeholder: Transparent), 3 modern, 4 allround
inline Tuning styleTuning (int mode)
{
    switch (mode) { case 1: return punchy(); case 3: return modern(); case 4: return allround(); default: return transparent(); }
}
inline const char* styleName (int mode)
{
    switch (mode) { case 1: return "punchy"; case 2: return "clip"; case 3: return "modern"; case 4: return "allround"; default: return "transparent"; }
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

// THE DETECTOR since 8 Oct 2026 (the gate found +1 dBTP overs by exact reconstruction that the 96-tap kernel could not
// see): a true-peak meter whose accuracy near Nyquist is set by a LONG half-band stage, cheaply.
//   stage 1  2x interpolation with a Kaiser half-band sinc of half-length kHalf input samples (every other tap is
//            zero, and one of the two 2x outputs is the input itself, so the cost is kHalf MACs per input sample)
//   stage 2  4x interpolation of the 2x stream with a short Kaiser kernel (the content now sits below a quarter of
//            that rate, far from any transition band)
//   refine   a parabola through each local maximum of the 8 points per input sample - the continuous peak
// On full-band white noise a truncated sinc under-reads by about 0.45/sqrt(kHalf): 2 % (0.17 dB) at 512; on anything
// with a natural spectrum the error is far below 0.02 dB. Reads kDelay input samples late.
struct TruePeakHB
{
    static constexpr int kHalf = 512, kTaps2 = 32, kPhases2 = 4, kDelay = kHalf + kTaps2 / 4;
    float hb[kHalf * 2] {};            // the half-band's nonzero taps, reversed so the window reads forward in memory
    float c2[kPhases2][kTaps2] {};      // the 4x stage, per phase, reversed
    float hist1[kHalf * 4] {}; int pos1 = 0;          // input history, kept twice over (2*kHalf) for contiguous reads
    float hist2[kTaps2 * 2] {}; int pos2 = 0;         // 2x stream history, kept twice over
    float prevA = 0.0f, prevB = 0.0f;                 // the last two 8x points of the previous sample (parabola across the boundary)
    static double i0 (double x) { double s = 1, t = 1; for (int k = 1; k < 80; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-15 * s) break; } return s; }
    void prepare() noexcept
    {
        const double pi = 3.14159265358979323846;
        {   // half-band: c[j] = sinc(j + 0.5) * kaiser, j = -kHalf .. kHalf-1, cutoff a quarter of the 2x rate = the input's Nyquist
            const double beta = 9.0, i0b = i0 (beta); double sum = 0; float tmp[kHalf * 2];
            for (int j = -kHalf; j < kHalf; ++j) { const double x = (double) j + 0.5; const double sinc = std::sin (pi * x) / (pi * x); const double r = x / (double) kHalf; const double w = std::abs (r) >= 1.0 ? 0.0 : i0 (beta * std::sqrt (1.0 - r * r)) / i0b; tmp[j + kHalf] = (float) (sinc * w); sum += sinc * w; }
            for (int j = 0; j < kHalf * 2; ++j) hb[j] = (float) (tmp[kHalf * 2 - 1 - j] / sum);   // reversed: hb[0] multiplies the OLDEST sample in the window
        }
        {   // 4x on the 2x stream: cutoff at 3/8 of the 2x rate (the half-band left nothing above a quarter), 32 taps, beta 8
            const double beta = 8.0, i0b = i0 (beta), fc = 0.375;
            for (int ph = 0; ph < kPhases2; ++ph)
            {
                double sum = 0; float tmp[kTaps2];
                for (int k = 0; k < kTaps2; ++k) { const double x = ((double) k - (kTaps2 / 2 - 0.5)) - (double) ph / kPhases2 + 0.5; const double sinc = x == 0.0 ? 2.0 * fc : std::sin (2.0 * pi * fc * x) / (pi * x); const double r = 2.0 * x / (double) kTaps2; const double w = std::abs (r) >= 1.0 ? 0.0 : i0 (beta * std::sqrt (1.0 - r * r)) / i0b; tmp[k] = (float) (sinc * w); sum += sinc * w; }
                for (int k = 0; k < kTaps2; ++k) c2[ph][k] = (float) (tmp[kTaps2 - 1 - k] / sum);
            }
        }
        reset();
    }
    void reset() noexcept { for (auto& h : hist1) h = 0.0f; for (auto& h : hist2) h = 0.0f; pos1 = pos2 = 0; prevA = prevB = 0.0f; }
    inline void push2 (float v) noexcept { hist2[pos2] = v; hist2[pos2 + kTaps2] = v; pos2 = (pos2 + 1) % kTaps2; }
    inline float interp2 (int ph) const noexcept { const float* w = hist2 + pos2; const float* c = c2[ph]; float a0 = 0, a1 = 0, a2 = 0, a3 = 0; for (int k = 0; k < kTaps2; k += 4) { a0 += c[k] * w[k]; a1 += c[k + 1] * w[k + 1]; a2 += c[k + 2] * w[k + 2]; a3 += c[k + 3] * w[k + 3]; } return (a0 + a1) + (a2 + a3); }
    // One input sample in; the refined true-peak envelope of the input sample kDelay ago out.
    inline float maxAbs (float x) noexcept
    {
        const int N = kHalf * 2;
        hist1[pos1] = x; hist1[pos1 + N] = x;
        // the 2x stream: the input sample from kHalf ago (the half-band's centre) and the midpoint after it
        const float* w = hist1 + pos1 + 1;                      // the last N input samples, oldest first
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        for (int k = 0; k < N; k += 8) { a0 += hb[k] * w[k]; a1 += hb[k + 1] * w[k + 1]; a2 += hb[k + 2] * w[k + 2]; a3 += hb[k + 3] * w[k + 3]; a4 += hb[k + 4] * w[k + 4]; a5 += hb[k + 5] * w[k + 5]; a6 += hb[k + 6] * w[k + 6]; a7 += hb[k + 7] * w[k + 7]; }
        const float mid = ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));
        const float centre = w[kHalf - 1];                      // x[n - kHalf]
        pos1 = (pos1 + 1) % N;
        // feed the 2x stream (centre, then the midpoint after it) and read the 8 points for this input sample
        float v[10]; v[0] = prevA; v[1] = prevB;
        push2 (centre); v[2] = interp2 (0); v[3] = interp2 (1); v[4] = interp2 (2); v[5] = interp2 (3);
        push2 (mid);    v[6] = interp2 (0); v[7] = interp2 (1); v[8] = interp2 (2); v[9] = interp2 (3);
        float m = 0.0f;
        for (int i = 1; i < 9; ++i)
        {
            const float a = std::abs (v[i - 1]), b = std::abs (v[i]), c = std::abs (v[i + 1]);
            if (b < a || b < c) continue;
            const float den = a - 2.0f * b + c; float pk = b;
            if (den < 0.0f) { const float d = 0.5f * (a - c) / den; if (std::abs (d) <= 1.0f) pk = b - 0.25f * (a - c) * d; }
            m = std::max (m, pk);
        }
        prevA = v[8]; prevB = v[9];
        return m;
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

// Running MAXIMUM over the last W values pushed (the mirror of RunningMin).
struct RunningMax
{
    std::vector<float> val; std::vector<long long> idx; int cap = 0, head = 0, tail = 0, n = 0; long long t = 0; int W = 1;
    void prepare (int capacity) { cap = std::max (2, capacity + 1); val.assign ((size_t) cap, 0.0f); idx.assign ((size_t) cap, 0); reset(); }
    void reset() noexcept { head = tail = n = 0; t = 0; }
    void setWindow (int w) noexcept { W = std::max (1, std::min (w, cap - 1)); }
    inline float push (float v) noexcept
    {
        while (n > 0) { const int last = (tail + cap - 1) % cap; if (val[(size_t) last] <= v) { tail = last; --n; } else break; }
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
        const int maxSlow = (int) std::ceil (std::max (1.0, std::max (t.slowWindowMs, t.slowCloseMs)) * 0.001 * sr_) + 1;
        const int maxPost = (int) std::ceil (std::max (0.0, t.postMs) * 0.001 * sr_) + 8;
        for (int c = 0; c < kMaxChannels; ++c)
        {
            tp_[c].prepare(); held_[c].prepare (maxK + 1); for (auto& m : ma_[c]) m.prepare (maxK + 1); slowWin_[c].prepare (maxSlow + 1); slowClose_[c].prepare (maxSlow + 1);
            tp2_[c].prepare(); held2_[c].prepare (maxPost + 1); for (auto& m : ma2_[c]) m.prepare (maxPost + 1);
        }
        const int maxDelay = maxK + TruePeakHB::kDelay + 1;
        for (auto& d : delay_) d.assign ((size_t) maxDelay, 0.0f);
        const int maxDelay2 = maxPost + TruePeakHB::kDelay + 1;
        for (auto& d : delay2_) d.assign ((size_t) maxDelay2, 0.0f);
        gaRing_.assign ((size_t) maxDelay2, 1.0f);
        delayCap_ = maxDelay; delayCap2_ = maxDelay2;
        // the fixed-latency maximum: the largest window the storage allows, true peak on, post-check on
        { const int S = std::max (1, std::min (t.smoothStages, 4)); const int laMax = std::max (1, (int) std::lround (t.maxLookaheadMs * 0.001 * sr_)); const int M = (laMax + S - 1) / S + 1; const int Kmax = S * (M - 1) + 1; int K2 = 1; if (t.postMs > 0.0) { const int la2 = std::max (2, (int) std::lround (t.postMs * 0.001 * sr_)); const int M2 = (la2 + 2) / 3 + 1; K2 = 3 * (M2 - 1) + 1; } maxLatency_ = (Kmax - 1) + TruePeakHB::kDelay + (K2 > 1 ? (K2 - 1) + TruePeakHB::kDelay : 0); }
        for (auto& d : scDelay_) d.assign ((size_t) maxLatency_ + 1, 0.0f);
        scDelayCap_ = maxLatency_ + 1; prepared_ = true; ceilLin_ = ceilTarget_; applyTuning(); reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            tp_[c].reset(); held_[c].reset(); for (auto& m : ma_[c]) m.reset(); slowWin_[c].reset(); slowClose_[c].reset(); eFast_[c] = eSlow_[c] = eSlow2_[c] = 0.0; std::fill (delay_[c].begin(), delay_[c].end(), 0.0f);
            tp2_[c].reset(); held2_[c].reset(); for (auto& m : ma2_[c]) m.reset(); std::fill (delay2_[c].begin(), delay2_[c].end(), 0.0f);
        }
        std::fill (gaRing_.begin(), gaRing_.end(), 1.0f);
        for (int c = 0; c < kMaxChannels; ++c) { std::fill (scDelay_[c].begin(), scDelay_[c].end(), 0.0f); hpfZ_[c][0] = hpfZ_[c][1] = 0.0; }
        wpos_ = 0; wpos2_ = 0; scPos_ = 0; gainNow_ = inputGain_; grDb_ = 0.0f; blockGrDb_ = 0.0f; bypassMix_ = bypassTarget_; ceilLin_ = ceilTarget_;
        nyqPrev_[0] = nyqPrev_[1] = 0.0f; nyqE_ = nyqD_ = 0.0f; nyqMarginNow_ = 0.0f;
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
    int naturalLatency() const noexcept { return (K_ - 1) + (truePeak_ ? TruePeakHB::kDelay + postLatency() : 0); }
    int postLatency() const noexcept { return (truePeak_ && K2_ > 1) ? (K2_ - 1) + TruePeakHB::kDelay : 0; }
    int fixedLatency_samples() const noexcept { return maxLatency_; }
    void setFixedLatency (bool on) noexcept { fixedLatency_ = on; applyTuning(); }
    bool fixedLatency() const noexcept { return fixedLatency_; }
    float gainReductionDb() const noexcept { return grDb_; }        // PEAK GR: the most negative gain applied in the last block, dB
    // BLOCK GR: 10log10 (output energy / gained-input energy) of the last block - what a loudness meter (and the
    // loudness loop's own estimate) measures. The output is the delayed signal, so on a per-block basis this lags the
    // input by the latency; over the loop's 250 ms ticks that is immaterial. 0 when the block is near silence.
    float blockGainReductionDb() const noexcept { return blockGrDb_; }
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
        const float gCoef = gainCoef_; float gMin = 1.0f; double eIn = 0.0, eOut = 0.0;
        const int scD = std::max (0, maxLatency_ - naturalLatency());   // the detector's delay under fixed latency
        if (tap_ != nullptr) tap_->setInputDelay (latencySamples());      // the picture's IN sits under the OUT it became
        for (int i = 0; i < n; ++i)
        {
            gainNow_ += (inputGain_ - gainNow_) * gCoef;
            if (std::abs (ceilLin_ - ceilTarget_) > 1e-7f) { ceilLin_ += (ceilTarget_ - ceilLin_) * gCoef; if (std::abs (ceilLin_ - ceilTarget_) < 1e-6f) ceilLin_ = ceilTarget_; }
            const float ceilNow = std::min (ceilLin_, ceilTarget_);
            bypassMix_ += (bypassTarget_ - bypassMix_) * bypassCoef_;
            // the detector input per channel: delayed under fixed latency (so the gain lands on the same sample it
            // would without it), high-passed when asked
            float x[kMaxChannels] { 0.0f, 0.0f }, r[kMaxChannels] { 1.0f, 1.0f }, sc[kMaxChannels] { 0.0f, 0.0f };
            for (int c = 0; c < numCh; ++c)
            {
                x[c] = ch[c][i] * gainNow_; sc[c] = x[c];
                if (fixedLatency_) { float* sd = scDelay_[c].data(); sd[scPos_] = x[c]; sc[c] = sd[(scPos_ + scDelayCap_ - scD) % scDelayCap_]; }
            }
            // the Nyquist-band fraction of what the detector sees: a first difference doubles the variance of white
            // noise and leaves bass almost untouched, so nyq = E[(s[n]-s[n-1])^2] / (2 E[s^2]) runs from ~0 (music) to
            // ~1 (white); above 0.05 it scales an extra margin, because a truncated sinc under-reads full-band material
            float ceilDetNow = truePeak_ ? ceilNow * tpMarginLin_ : ceilNow;
            if (truePeak_ && tuning_.nyquistMarginDb > 0.0)
            {
                float e = 0.0f, d = 0.0f;
                for (int c = 0; c < numCh; ++c) { const float df = sc[c] - nyqPrev_[c]; nyqPrev_[c] = sc[c]; e += sc[c] * sc[c]; d += df * df; }
                nyqE_ += (e - nyqE_) * nyqCoef_; nyqD_ += (d - nyqD_) * nyqCoef_;
                const float frac = nyqE_ > 1e-9f ? std::min (1.0f, std::max (0.0f, (nyqD_ / (2.0f * nyqE_) - 0.05f) / 0.95f)) : 0.0f;   // below 0.05 (music) costs nothing
                nyqMarginNow_ = frac;
                ceilDetNow *= (float) std::pow (10.0, -tuning_.nyquistMarginDb * frac / 20.0);
            }
            float tpInMax = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                float s1 = sc[c];
                if (scHpfOn_) { const double y = hpfB0_ * s1 + hpfZ_[c][0]; hpfZ_[c][0] = hpfB1_ * s1 - hpfA1_ * y + hpfZ_[c][1]; hpfZ_[c][1] = hpfB2_ * s1 - hpfA2_ * y; s1 = (float) y; }
                const float p = truePeak_ ? tp_[c].maxAbs (s1) : std::abs (s1);
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
            float g[kMaxChannels] { 1.0f, 1.0f }; double floorC[kMaxChannels] { 0.0, 0.0 }; double envFastC[kMaxChannels] { 0.0, 0.0 };
            // pass 1: per channel, the held minimum, the fast limb and the floor (so the floors can be linked before use)
            for (int c = 0; c < numCh; ++c)
            {
                if (c == 1 && tuning_.link >= 1.0) { held_[1].push (r[1]); break; }   // one envelope: channel 0's
                const float held = held_[c].push (r[c]);
                const double d = held < 1.0f ? -20.0 * std::log10 ((double) held) : 0.0;
                float heldSlow = slowWin_[c].push (held);                 // dilation: the smallest GAIN (largest reduction) over the window
                if (closeOn_) heldSlow = slowClose_[c].push (heldSlow);     // erosion: the largest gain over the closing window of that
                const double dWin = heldSlow < 1.0f ? -20.0 * std::log10 ((double) heldSlow) : 0.0;
                eFast_[c] = std::max (d, eFast_[c] * decayFast_);
                const double slowTarget = dWin * tuning_.slowFraction;
                eSlow_[c] += (slowTarget - eSlow_[c]) * (slowTarget > eSlow_[c] ? coefSlowAtk_ : coefSlowRel_);
                double floor = eSlow_[c];
                if (coefSlowAtk2_ > 0.0) { const double t2 = dWin * tuning_.slowFraction2; eSlow2_[c] += (t2 - eSlow2_[c]) * (t2 > eSlow2_[c] ? coefSlowAtk2_ : coefSlowRel2_); floor = std::max (floor, eSlow2_[c]); }
                floorC[c] = floor; envFastC[c] = eFast_[c];
            }
            // the release link: the floors blended toward the deeper of the two (1 = one floor for both)
            if (numCh > 1 && tuning_.link < 1.0 && tuning_.linkRelease > 0.0) { const double fl = std::max (floorC[0], floorC[1]); for (int c = 0; c < 2; ++c) floorC[c] = tuning_.linkRelease * fl + (1.0 - tuning_.linkRelease) * floorC[c]; }
            // pass 2: the envelope through the smoothing window
            for (int c = 0; c < numCh; ++c)
            {
                if (c == 1 && tuning_.link >= 1.0) { g[1] = g[0]; break; }   // identical by construction: one envelope
                const double env = std::max (envFastC[c], floorC[c]);
                float ge = (float) std::pow (10.0, -env / 20.0);
                for (int s = 0; s < S_; ++s) ge = ma_[c][s].push (ge);
                g[c] = ge;
            }
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
                float g2raw = held2_[0].push (r2); for (int s = 0; s < 3; ++s) g2raw = ma2_[0][s].push (g2raw); const float g2 = bypassMix_ >= 1.0f ? g2raw : 1.0f + (g2raw - 1.0f) * bypassMix_;
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
            for (int c = 0; c < numCh; ++c) { ch[c][i] = std::max (-clipAt, std::min (clipAt, y[c])); eIn += (double) x[c] * x[c]; eOut += (double) ch[c][i] * ch[c][i]; }
            if (tap_ != nullptr) tap_->push (x[0], numCh > 1 ? x[1] : x[0], ch[0][i], numCh > 1 ? ch[1][i] : ch[0][i], gOut, tpInMax, tpOutMax > 0.0f ? tpOutMax : std::max (std::abs (ch[0][i]), numCh > 1 ? std::abs (ch[1][i]) : 0.0f));
        }
        grDb_ = gMin < 1.0f ? 20.0f * std::log10 (gMin) : 0.0f;
        blockGrDb_ = (eIn > 1e-7 * (double) n && eOut < eIn) ? (float) (10.0 * std::log10 (eOut / eIn)) : 0.0f;
    }

private:
    void applyTuning() noexcept
    {
        S_ = std::max (1, std::min (tuning_.smoothStages, 4));
        closeOn_ = tuning_.slowCloseMs > 0.0;
        const int la = std::max (1, (int) std::lround (std::min (tuning_.lookaheadMs, tuning_.maxLookaheadMs) * 0.001 * sr_));
        const int M = (la + S_ - 1) / S_ + 1;            // per-stage length so the support covers the lookahead
        K_ = S_ * (M - 1) + 1;                            // kernel support
        int K2 = 1, M2 = 1;
        if (truePeak_ && tuning_.postMs > 0.0) { const int la2 = std::max (2, (int) std::lround (tuning_.postMs * 0.001 * sr_)); M2 = (la2 + 2) / 3 + 1; K2 = 3 * (M2 - 1) + 1; }   // a quadratic B-spline over postMs
        K2_ = K2;
        for (int c = 0; c < kMaxChannels; ++c) { held_[c].setWindow (K_); for (int s = 0; s < S_; ++s) ma_[c][s].setLength (M); slowWin_[c].setWindow (std::max (1, (int) std::lround (tuning_.slowWindowMs * 0.001 * sr_))); slowClose_[c].setWindow (std::max (1, (int) std::lround (tuning_.slowCloseMs * 0.001 * sr_))); held2_[c].setWindow (K2_); for (int s = 0; s < 3; ++s) ma2_[c][s].setLength (M2); }
        delaySamples_ = std::min (delayCap_ - 1, (K_ - 1) + (truePeak_ ? TruePeakHB::kDelay : 0));
        delaySamples2_ = std::min (delayCap2_ - 1, postLatency());
        if (fixedLatency_ && delayCap_ <= maxLatency_) { delay_[0].assign ((size_t) maxLatency_ + 1, 0.0f); delay_[1].assign ((size_t) maxLatency_ + 1, 0.0f); delayCap_ = maxLatency_ + 1; }   // only reachable from prepare(): the fixed delay fits the storage sized there
        tpMarginLin_ = (float) std::pow (10.0, -tuning_.tpMarginDb / 20.0);
        bypassCoef_ = (float) (1.0 - std::exp (-1.0 / (0.01 * sr_)));
        nyqCoef_ = (float) (1.0 - std::exp (-1.0 / (0.05 * sr_)));
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
        coefSlowRel2_ = tuning_.slowRelease2Ms > 0.0 ? 1.0 - std::exp (-1.0 / (tuning_.slowRelease2Ms * 0.001 * sr_)) : coefSlowRel_;
        gainCoef_ = (float) (1.0 - std::exp (-1.0 / (0.02 * sr_)));
    }

    double sr_ = 48000.0; Tuning tuning_;
    TruePeakHB tp_[kMaxChannels]; RunningMin held_[kMaxChannels], slowWin_[kMaxChannels]; RunningMax slowClose_[kMaxChannels]; MovingAverage ma_[kMaxChannels][4]; bool closeOn_ = false;
    TruePeakHB tp2_[kMaxChannels]; RunningMin held2_[kMaxChannels]; MovingAverage ma2_[kMaxChannels][3];
    double eFast_[kMaxChannels] { 0.0, 0.0 }, eSlow_[kMaxChannels] { 0.0, 0.0 }, eSlow2_[kMaxChannels] { 0.0, 0.0 };
    std::vector<float> delay_[kMaxChannels], delay2_[kMaxChannels], gaRing_; int delayCap_ = 1, delaySamples_ = 0, wpos_ = 0, delayCap2_ = 1, delaySamples2_ = 0, wpos2_ = 0;
    int S_ = 3, K_ = 1, K2_ = 1;
    double decayFast_ = 0.0, coefSlowRel_ = 0.0, coefSlowAtk_ = 0.0, coefSlowAtk2_ = 0.0, coefSlowRel2_ = 0.0;
    float inputGain_ = 1.0f, gainNow_ = 1.0f, gainCoef_ = 0.0f, ceilLin_ = 1.0f, ceilTarget_ = 1.0f, tpMarginLin_ = 1.0f, grDb_ = 0.0f, blockGrDb_ = 0.0f;
    bool truePeak_ = true, prepared_ = false, fixedLatency_ = false, scHpfOn_ = false;
    int maxLatency_ = 0; std::vector<float> scDelay_[kMaxChannels]; int scDelayCap_ = 1, scPos_ = 0;
    double scHpfHz_ = 0.0, hpfB0_ = 1, hpfB1_ = 0, hpfB2_ = 0, hpfA1_ = 0, hpfA2_ = 0; double hpfZ_[kMaxChannels][2] { { 0, 0 }, { 0, 0 } };
    float bypassTarget_ = 1.0f, bypassMix_ = 1.0f, bypassCoef_ = 0.0f;
    float nyqPrev_[kMaxChannels] { 0.0f, 0.0f }, nyqE_ = 0.0f, nyqD_ = 0.0f, nyqCoef_ = 0.0f, nyqMarginNow_ = 0.0f;
    MeterTap* tap_ = nullptr;
};

} // namespace limv2
} // namespace echojay
