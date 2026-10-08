#pragma once
// ejmetrics.h (limiter_ab_guard, 7 Oct 2026): what the harness measures, and in what order. ALIGNMENT FIRST and
// asserted; every other number is computed on the aligned pair, against the gain the limiter was given.
//
// Conventions: `in` is the UNPROCESSED source as it sits in the file; the limiter saw in * G (G = the stated gain,
// +8.2 dB in Sean's A/B). `out` is the render. GR (gain reduction) is therefore 20log10(|out| / (G|in|)) - zero
// when the limiter is doing nothing, negative when it is working, and "static make-up" never enters: Pro-L 2's
// output dial was 0.0 dB in the A/B and the harness takes the gain and the ceiling as inputs, not estimates.
#include "ejwav.h"
#include "ejdsp.h"
#include "ejfixtures.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace ejm {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
inline bool isnan (double v) { return std::isnan (v); }
inline double median (std::vector<double> v) { v.erase (std::remove_if (v.begin(), v.end(), [] (double x) { return std::isnan (x); }), v.end()); if (v.empty()) return NaN; std::sort (v.begin(), v.end()); const size_t n = v.size(); return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]); }
inline double mean (const std::vector<double>& v) { double s = 0; size_t n = 0; for (double x : v) if (! std::isnan (x)) { s += x; ++n; } return n ? s / (double) n : NaN; }
inline double stddev (const std::vector<double>& v) { const double m = mean (v); if (std::isnan (m)) return NaN; double s = 0; size_t n = 0; for (double x : v) if (! std::isnan (x)) { s += (x - m) * (x - m); ++n; } return n > 1 ? std::sqrt (s / (double) (n - 1)) : 0.0; }

// ---------------------------------------------------------------------------------------------------------------
// 1. ALIGNMENT. Cross-correlate a mono sum of the render against the source in several windows along the file.
//    The offset must be identical in every valid window; otherwise the case is refused. A limiter has a fixed
//    latency (lookahead + any oversampling filter); a varying offset means the render is not comparable at all.
// ---------------------------------------------------------------------------------------------------------------
struct AlignResult
{
    bool ok = false; bool inverted = false; int offset = 0; double tailSeconds = 0;   // tailSeconds: silent tail beyond the source's length
    std::vector<int> windowOffsets; std::vector<double> windowPeaks; std::vector<size_t> windowStarts; std::vector<bool> windowValid; std::vector<bool> windowAmbiguous;
    std::string why;
};

inline std::vector<double> monoSum (const ejwav::Audio& a)
{
    std::vector<double> m (a.frames(), 0.0);
    for (const auto& c : a.ch) for (size_t n = 0; n < m.size(); ++n) m[n] += c[n] / (double) a.channels();
    return m;
}

// `anchors`: window positions (start sample, length) to use INSTEAD of the evenly spaced ones - the synthetic
// fixtures' broadband markers. A window whose correlation has more than one peak within 2% of the best (periodic
// material) is AMBIGUOUS and ignored; a case needs two unambiguous windows that agree.
inline AlignResult align (const ejwav::Audio& src, const ejwav::Audio& proc, const std::vector<std::pair<size_t, size_t>>& anchors = {}, int maxLag = 16384)
{
    AlignResult r;
    const auto s = monoSum (src), q = monoSum (proc);
    const size_t N = s.size();
    if (N < 8192 || q.size() < 8192) { r.why = "file too short to align (< 8192 samples)"; return r; }
    // A render may be LONGER than its source (a fixed-length bounce: material, then silence); it may not be shorter by
    // more than the lag window. The tail beyond the source's length must be silent (< -80 dBFS RMS), else refuse.
    if (q.size() < N && N - q.size() > (size_t) maxLag) { r.why = "render is shorter than the source by more than the lag window (" + std::to_string (N) + " vs " + std::to_string (q.size()) + " frames)"; return r; }
    if (q.size() > N + (size_t) maxLag)
    {
        double e = 0; for (size_t n = N + (size_t) maxLag; n < q.size(); ++n) e += q[n] * q[n]; const double tailDb = ejdsp::dB (std::sqrt (e / (double) (q.size() - N - (size_t) maxLag)));
        if (tailDb > -80.0) { r.why = "render is longer than the source and the tail beyond it is NOT silent (" + std::to_string ((int) tailDb) + " dBFS RMS)"; return r; }
        r.tailSeconds = (double) (q.size() - N) / src.sampleRate;
    }
    size_t W = 1 << 17; while (W > N / 2 && W > 4096) W >>= 1;
    const size_t L = (size_t) maxLag;
    std::vector<std::pair<size_t, size_t>> windows = anchors;
    if (windows.empty()) { const size_t span = N - W; const size_t k = std::min<size_t> (8, std::max<size_t> (3, N / W)); for (size_t wi = 0; wi < k; ++wi) windows.push_back ({ (size_t) ((double) span * (double) wi / (double) (k - 1)), W }); }
    for (const auto& win : windows)
    {
        const size_t p = win.first; W = win.second; if (p + W > N) continue;
        const size_t M = ejdsp::nextPow2 (W + 2 * L + W);
        // source window energy
        double es = 0; for (size_t n = 0; n < W; ++n) es += s[p + n] * s[p + n];
        const double rmsDb = ejdsp::dB (std::sqrt (es / (double) W));
        std::vector<std::complex<double>> A (M), B (M);
        for (size_t n = 0; n < W; ++n) A[n] = s[p + n];
        std::vector<double> qseg (W + 2 * L, 0.0);
        for (size_t n = 0; n < W + 2 * L; ++n) { const long long idx = (long long) p + (long long) n - (long long) L; qseg[n] = (idx >= 0 && idx < (long long) q.size()) ? q[(size_t) idx] : 0.0; B[n] = qseg[n]; }
        ejdsp::fft (A, false); ejdsp::fft (B, false);
        for (size_t i = 0; i < M; ++i) A[i] = std::conj (A[i]) * B[i];
        ejdsp::fft (A, true);
        std::vector<double> pre (qseg.size() + 1, 0.0); for (size_t n = 0; n < qseg.size(); ++n) pre[n + 1] = pre[n] + qseg[n] * qseg[n];
        std::vector<double> corr (2 * L + 1); double best = 0; size_t bestM = 0;
        for (size_t m = 0; m <= 2 * L; ++m) { const double eq = pre[m + W] - pre[m]; const double norm = std::sqrt (es * eq); corr[m] = norm > 0 ? A[m].real() / norm : 0.0; if (std::abs (corr[m]) > std::abs (best)) { best = corr[m]; bestM = m; } }
        int peaksNear = 0; for (size_t m = 1; m + 1 <= 2 * L; ++m) if (std::abs (corr[m]) >= 0.98 * std::abs (best) && std::abs (corr[m]) >= std::abs (corr[m - 1]) && std::abs (corr[m]) >= std::abs (corr[m + 1])) ++peaksNear;
        const bool ambiguous = peaksNear > 1;
        const bool valid = rmsDb > -60.0 && std::abs (best) >= 0.3 && ! ambiguous;
        r.windowStarts.push_back (p); r.windowOffsets.push_back ((int) bestM - (int) L); r.windowPeaks.push_back (best); r.windowValid.push_back (valid); r.windowAmbiguous.push_back (ambiguous);
    }
    // Windows may disagree by ONE sample: a limiter that reshapes the waveform (Pro-L 2 and v2 Transparent on bass)
    // moves the correlation peak by a sample either way. The offset is the MEDIAN of the valid windows; a spread of
    // more than one sample is a slip and is refused.
    int nValid = 0, nAmbiguous = 0; bool anyInverted = false; std::vector<int> offs;
    for (size_t i = 0; i < r.windowValid.size(); ++i) { if (r.windowAmbiguous[i]) ++nAmbiguous; if (r.windowValid[i]) { ++nValid; offs.push_back (r.windowOffsets[i]); if (r.windowPeaks[i] < 0) anyInverted = true; } }
    bool consistent = true; int off = 0;
    if (! offs.empty()) { std::sort (offs.begin(), offs.end()); off = offs[offs.size() / 2]; consistent = offs.back() - offs.front() <= 1; }
    r.offset = off; r.inverted = anyInverted;
    if (nValid < 2) { r.why = nAmbiguous > 0 ? "alignment AMBIGUOUS: periodic material gives more than one correlation peak in " + std::to_string (nAmbiguous) + " window(s) and fewer than 2 unambiguous windows remain" : "fewer than 2 windows with signal and a correlation peak >= 0.3 (silence, or not the same material)"; return r; }
    if (! consistent) { r.why = "offset is NOT constant across the file - the render is not sample-comparable (resampled, slipped, or a different take)"; return r; }
    if (anyInverted) { r.why = "polarity inverted against the source"; return r; }
    r.ok = true; return r;
}

// The aligned pair: in[n] is what the limiter saw (before its gain), out[n] is what came out at the same instant.
struct Pair
{
    double sr = 0; int offset = 0; double gainDb = 0, ceilingDb = 0; double G = 1, ceilLin = 1;
    std::vector<std::vector<double>> in, out;   // [ch][n], equal lengths, same channel count
    size_t frames() const { return in.empty() ? 0 : in[0].size(); }
    int channels() const { return (int) in.size(); }
};

inline Pair makePair (const ejwav::Audio& src, const ejwav::Audio& proc, int offset, double gainDb, double ceilingDb)
{
    Pair p; p.sr = src.sampleRate; p.offset = offset; p.gainDb = gainDb; p.ceilingDb = ceilingDb; p.G = ejdsp::lin (gainDb); p.ceilLin = ejdsp::lin (ceilingDb);
    const size_t i0in = (size_t) std::max (0, -offset), i0out = (size_t) std::max (0, offset);
    const size_t len = std::min (src.frames() - i0in, proc.frames() - i0out);
    const int nch = std::min (src.channels(), proc.channels());
    for (int c = 0; c < nch; ++c) { p.in.emplace_back (src.ch[(size_t) c].begin() + (long) i0in, src.ch[(size_t) c].begin() + (long) (i0in + len)); p.out.emplace_back (proc.ch[(size_t) c].begin() + (long) i0out, proc.ch[(size_t) c].begin() + (long) (i0out + len)); }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// 2. LOUDNESS (BS.1770-4): momentary 400 ms / 100 ms hop, two-stage gated integrated, short-term 3 s.
// ---------------------------------------------------------------------------------------------------------------
struct Loudness { double integrated = NaN, maxMomentary = NaN, maxShortTerm = NaN, stdShortTerm = NaN; std::vector<double> momentary, shortTerm; };

inline Loudness loudness (const std::vector<std::vector<double>>& ch, double sr, double extraGainDb = 0.0)
{
    Loudness r; if (ch.empty() || ch[0].size() < (size_t) (0.4 * sr)) return r;
    const double g2 = std::pow (ejdsp::lin (extraGainDb), 2.0);
    std::vector<std::vector<double>> kw; for (const auto& c : ch) kw.push_back (ejdsp::kWeight (c, sr));
    const size_t N = kw[0].size(), hop = (size_t) std::llround (0.1 * sr), blk = (size_t) std::llround (0.4 * sr);
    std::vector<double> z;   // mean square summed over channels per 100 ms hop (so blocks are sums of 4 hops)
    for (size_t p = 0; p + hop <= N; p += hop) { double s = 0; for (const auto& c : kw) for (size_t n = p; n < p + hop; ++n) s += c[n] * c[n]; z.push_back (g2 * s / (double) hop); }
    const int bh = (int) (blk / hop);
    std::vector<double> zm;   // momentary block mean squares
    for (size_t i = 0; i + (size_t) bh <= z.size(); ++i) { double s = 0; for (int j = 0; j < bh; ++j) s += z[i + (size_t) j]; zm.push_back (s / bh); }
    for (double v : zm) r.momentary.push_back (-0.691 + ejdsp::dBp (v));
    double sumAbs = 0; size_t nAbs = 0; for (double v : zm) if (-0.691 + ejdsp::dBp (v) > -70.0) { sumAbs += v; ++nAbs; }
    if (nAbs) { const double rel = -0.691 + ejdsp::dBp (sumAbs / (double) nAbs) - 10.0; double s = 0; size_t n = 0; for (double v : zm) { const double l = -0.691 + ejdsp::dBp (v); if (l > -70.0 && l > rel) { s += v; ++n; } } if (n) r.integrated = -0.691 + ejdsp::dBp (s / (double) n); }
    for (double l : r.momentary) if (isnan (r.maxMomentary) || l > r.maxMomentary) r.maxMomentary = l;
    const int sh = 30;
    for (size_t i = 0; i + (size_t) sh <= z.size(); ++i) { double s = 0; for (int j = 0; j < sh; ++j) s += z[i + (size_t) j]; r.shortTerm.push_back (-0.691 + ejdsp::dBp (s / sh)); }
    for (double l : r.shortTerm) if (isnan (r.maxShortTerm) || l > r.maxShortTerm) r.maxShortTerm = l;
    { std::vector<double> loud; for (double l : r.shortTerm) if (l > -40.0) loud.push_back (l); r.stdShortTerm = stddev (loud); }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// 3. PEAKS: sample peak, 4x true peak, and the count of inter-sample overs above the ceiling (+0.05 dB margin
//    for the interpolator's own ripple; the dBTP maximum is printed so the margin is never hidden).
// ---------------------------------------------------------------------------------------------------------------
// Since 8 Oct 2026 the ARBITER (exact band-limited reconstruction, margin 0.02 dB) decides every overs result; the
// 96-tap meter's reading is kept alongside for comparison (`meter*`), never for a verdict.
struct Peaks { double truePeakDb = -600, samplePeakDb = -600; size_t overs = 0; double worstSec = 0; size_t edgeOvers = 0; double edgePeakDb = -600;
               double meterPeakDb = -600; size_t meterOvers = 0; };
inline Peaks peaks (const std::vector<std::vector<double>>& ch, double sr, double ceilingDb, double marginDb = 0.02)
{
    Peaks p; const double cl = ejdsp::lin (ceilingDb);
    for (const auto& c : ch)
    {
        const auto a = ejdsp::truePeakExact (c, cl, marginDb); const double adb = ejdsp::dB (a.peakLin);
        if (adb > p.truePeakDb) { p.truePeakDb = adb; p.worstSec = (double) a.peakIndex / sr; }
        p.overs += a.overs; p.edgeOvers += a.edgeOvers; p.edgePeakDb = std::max (p.edgePeakDb, ejdsp::dB (a.edgePeakLin));
        const auto t = ejdsp::truePeak (c, cl, 0.05); p.meterPeakDb = std::max (p.meterPeakDb, ejdsp::dB (t.peakLin)); p.meterOvers += t.overs;
        p.samplePeakDb = std::max (p.samplePeakDb, ejdsp::dB (t.samplePeakLin));
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// 4. GAIN-REDUCTION ENVELOPE: per block, 10log10(sum out^2 / (G^2 sum in^2)), channels summed (or one channel).
//    NaN where the input block is below -60 dBFS: a ratio of two near-zeros is not a gain.
// ---------------------------------------------------------------------------------------------------------------
inline std::vector<double> grSeries (const Pair& p, size_t block, int channel = -1)
{
    std::vector<double> gr; const size_t N = p.frames(); const double g2 = p.G * p.G;
    for (size_t b = 0; b + block <= N; b += block)
    {
        double ei = 0, eo = 0;
        for (int c = 0; c < p.channels(); ++c) { if (channel >= 0 && c != channel) continue; const auto& i = p.in[(size_t) c]; const auto& o = p.out[(size_t) c]; for (size_t n = b; n < b + block; ++n) { ei += i[n] * i[n]; eo += o[n] * o[n]; } }
        const double rmsIn = std::sqrt (ei / (double) block);
        gr.push_back (rmsIn > 1e-3 ? ejdsp::dBp (eo / (g2 * ei)) : NaN);
    }
    return gr;
}

// ---------------------------------------------------------------------------------------------------------------
// 5. HITS: onsets detected on the gained input (a +8 dB jump over the preceding 60 ms, above -20 dBFS, 150 ms
//    refractory). Detected on the SOURCE only, so every render of a case gets the same hit list and hits compare
//    index for index. Per hit: the attack peak retained, the GR dip, how early the dip starts (the lookahead), and the
//    recovery times (release limbs). On the probe fixtures the count is cross-checked against the layout.
// ---------------------------------------------------------------------------------------------------------------
struct Hit
{
    double tSec = 0; double peakInDb = NaN, peakOutDb = NaN, retentionDb = NaN, overDb = NaN;
    double grMinDb = NaN, baselineDb = NaN, preDipMs = NaN, holdMs = NaN;
    double t10 = NaN, t50 = NaN, t63 = NaN, t90 = NaN;   // ms from the END OF THE HOLD (GR first 0.2 dB above its minimum) to that fraction recovered
    double dipLDb = NaN, dipRDb = NaN;                   // per-channel dips (stereo linking)
    std::vector<double> trace;                           // GR in 1 ms steps from -25 ms to +60 ms around the onset
};

inline std::vector<size_t> detectOnsets (const Pair& p)
{
    const size_t B = (size_t) std::llround (p.sr / 1000.0), N = p.frames();
    std::vector<double> pk; for (size_t b = 0; b + B <= N; b += B) { double m = 0; for (int c = 0; c < p.channels(); ++c) for (size_t n = b; n < b + B; ++n) m = std::max (m, std::abs (p.in[(size_t) c][n])); pk.push_back (m * p.G); }
    std::vector<size_t> on; long long last = -1000000;
    for (size_t k = 3; k < pk.size(); ++k)
    {
        double base = 0; for (size_t j = (k >= 63 ? k - 63 : 0); j + 3 <= k; ++j) base = std::max (base, pk[j]);
        if (pk[k] > 0.1 && pk[k] > 2.512 * base && (long long) k - last >= 150) { on.push_back (k * B); last = (long long) k; }
    }
    return on;
}

inline std::vector<Hit> analyseHits (const Pair& p, const std::vector<size_t>& onsets)
{
    std::vector<Hit> hits; const size_t N = p.frames();
    const size_t Bf = std::max<size_t> (8, (size_t) std::llround (p.sr / 3000.0));   // ~0.33 ms blocks for the envelope
    const auto gr = grSeries (p, Bf); std::vector<std::vector<double>> grc; for (int c = 0; c < p.channels(); ++c) grc.push_back (grSeries (p, Bf, c));
    const double msPerBlock = 1000.0 * (double) Bf / p.sr;
    for (size_t h = 0; h < onsets.size(); ++h)
    {
        Hit H; const size_t on = onsets[h]; H.tSec = (double) on / p.sr;
        const size_t win = (size_t) std::llround (0.015 * p.sr);
        double pi = 0, po = 0; for (int c = 0; c < p.channels(); ++c) for (size_t n = on; n < std::min (N, on + win); ++n) { pi = std::max (pi, std::abs (p.in[(size_t) c][n]) * p.G); po = std::max (po, std::abs (p.out[(size_t) c][n])); }
        H.peakInDb = ejdsp::dB (pi); H.peakOutDb = ejdsp::dB (po); H.retentionDb = H.peakOutDb - H.peakInDb; H.overDb = H.peakInDb - p.ceilingDb;
        const size_t kOn = on / Bf;
        const size_t kBase0 = (size_t) std::max<long long> (0, (long long) kOn - (long long) std::llround (120.0 / msPerBlock)), kBase1 = (size_t) std::max<long long> (0, (long long) kOn - (long long) std::llround (40.0 / msPerBlock));
        { std::vector<double> b; for (size_t k = kBase0; k < kBase1 && k < gr.size(); ++k) b.push_back (gr[k]); H.baselineDb = median (b); if (isnan (H.baselineDb)) H.baselineDb = 0.0; }
        const size_t kMin0 = (size_t) std::max<long long> (0, (long long) kOn - (long long) std::llround (25.0 / msPerBlock)), kMin1 = std::min (gr.size(), kOn + (size_t) std::llround (30.0 / msPerBlock));
        size_t kMin = kOn; double vMin = NaN; for (size_t k = kMin0; k < kMin1; ++k) if (! isnan (gr[k]) && (isnan (vMin) || gr[k] < vMin)) { vMin = gr[k]; kMin = k; }
        if (! isnan (vMin)) for (size_t k = kMin0; k < kMin1; ++k) if (! isnan (gr[k]) && gr[k] <= vMin + 0.05) { kMin = k; break; }   // the FIRST block at the minimum: a held minimum is flat, and ties must not be broken by rounding noise
        H.grMinDb = vMin;
        if (p.channels() >= 2) { double a = NaN, b = NaN; for (size_t k = kMin0; k < kMin1; ++k) { if (! isnan (grc[0][k]) && (isnan (a) || grc[0][k] < a)) a = grc[0][k]; if (! isnan (grc[1][k]) && (isnan (b) || grc[1][k] < b)) b = grc[1][k]; } H.dipLDb = a; H.dipRDb = b; }
        // pre-dip: walk back from the onset block to the last block still within 0.5 dB of the baseline
        if (! isnan (vMin) && vMin < H.baselineDb - 1.0) { size_t k = kOn; while (k > kBase1 && ! isnan (gr[k - 1]) && gr[k - 1] < H.baselineDb - 0.5) --k; H.preDipMs = (double) (kOn - k) * msPerBlock; if (k <= kBase1) H.preDipMs = NaN; }
        // the HOLD: from the minimum until GR first rises 0.2 dB above it (a long burst keeps the limiter at its
        // minimum for the burst's length); the release limbs are timed from there, until the next onset. The 0.2 dB
        // adds ~3% of the span to the hold and takes it from the limbs - the same for every render, so it cancels
        // in a comparison.
        const size_t kEnd = (h + 1 < onsets.size()) ? std::min (gr.size(), onsets[h + 1] / Bf) : gr.size();
        if (! isnan (vMin) && vMin < H.baselineDb - 1.0)
        {
            size_t kRel = kMin; while (kRel + 1 < kEnd && ! isnan (gr[kRel + 1]) && gr[kRel + 1] <= vMin + 0.2) ++kRel;
            H.holdMs = (double) (kRel - kMin) * msPerBlock;
            const double span = H.baselineDb - vMin;
            auto timeTo = [&] (double frac) { const double target = vMin + frac * span; for (size_t k = kRel; k < kEnd; ++k) if (! isnan (gr[k]) && gr[k] >= target) return (double) (k - kRel) * msPerBlock; return NaN; };
            H.t10 = timeTo (0.10); H.t50 = timeTo (0.50); H.t63 = timeTo (0.632); H.t90 = timeTo (0.90);
        }
        const size_t B1 = (size_t) std::llround (p.sr / 1000.0); const auto g1 = grSeries (p, B1);
        for (int ms = -25; ms <= 60; ++ms) { const long long k = (long long) (on / B1) + ms; H.trace.push_back (k >= 0 && (size_t) k < g1.size() ? g1[(size_t) k] : NaN); }
        hits.push_back (H);
    }
    return hits;
}

// ---------------------------------------------------------------------------------------------------------------
// 6. TONE SEGMENTS: steady-state level and GR, THD / THD+N or IMD per segment, and the step response at each
//    boundary (time to 10/50/90 % of the GR change), from the shared layout.
// ---------------------------------------------------------------------------------------------------------------
struct ToneSeg { double levelDb = 0, outRmsDb = NaN, grSteadyDb = NaN, fundDb = NaN, thdDb = NaN, thdnDb = NaN; std::vector<double> harmDb; /* h2..h8 rel. fundamental */ double imd1k = NaN, imd2k = NaN, imd18k = NaN, imd21k = NaN; };
struct ToneStep { double tSec = 0, fromDb = NaN, toDb = NaN, t10 = NaN, t50 = NaN, t90 = NaN; };
struct ToneResult { std::vector<ToneSeg> segs; std::vector<ToneStep> steps; };

inline ToneResult analyseTone (const Pair& p, const ejfix::Layout& L)
{
    ToneResult R; const size_t N = p.frames(); const size_t nfft = 1 << 16;
    const size_t B = (size_t) std::llround (p.sr / 1000.0); const auto gr = grSeries (p, B);
    const double binHz = p.sr / (double) nfft;
    auto linePower = [&] (const std::vector<double>& ps, double hz) { const long k0 = std::lround (hz / binHz); double s = 0; for (long k = k0 - 5; k <= k0 + 5; ++k) if (k >= 0 && (size_t) k < ps.size()) s += ps[(size_t) k]; return s; };
    for (const auto& s : L.segments)
    {
        ToneSeg T; T.levelDb = s.levelDb;
        const size_t n0 = (size_t) std::llround ((s.t0 + 1.5) * p.sr); if (n0 + nfft > N) { R.segs.push_back (T); continue; }
        { double e = 0; for (size_t n = n0; n < n0 + nfft; ++n) e += p.out[0][n] * p.out[0][n]; T.outRmsDb = ejdsp::dB (std::sqrt (e / (double) nfft)); }
        { std::vector<double> g; for (size_t k = n0 / B; k < (n0 + nfft) / B && k < gr.size(); ++k) g.push_back (gr[k]); T.grSteadyDb = mean (g); }
        const auto ps = ejdsp::powerSpectrum (p.out[0], n0, nfft);
        double total = 0; for (size_t k = 1; k < ps.size(); ++k) total += ps[k];
        if (L.toneHz2 > 0)
        {
            const double pair = linePower (ps, L.toneHz) + linePower (ps, L.toneHz2);
            T.fundDb = ejdsp::dBp (pair);
            T.imd1k = ejdsp::dBp (linePower (ps, L.toneHz2 - L.toneHz) / pair); T.imd2k = ejdsp::dBp (linePower (ps, 2 * (L.toneHz2 - L.toneHz)) / pair);
            T.imd18k = ejdsp::dBp (linePower (ps, 2 * L.toneHz - L.toneHz2) / pair); T.imd21k = (2 * L.toneHz2 - L.toneHz < p.sr / 2) ? ejdsp::dBp (linePower (ps, 2 * L.toneHz2 - L.toneHz) / pair) : NaN;
            T.thdnDb = ejdsp::dBp (std::max (0.0, total - pair) / pair);
        }
        else
        {
            const double fund = linePower (ps, L.toneHz); T.fundDb = ejdsp::dBp (fund);
            double harm = 0; for (int h = 2; h <= 10 && h * L.toneHz < p.sr / 2; ++h) { const double hp = linePower (ps, h * L.toneHz); harm += hp; if (h <= 8) T.harmDb.push_back (ejdsp::dBp (hp / fund)); }
            T.thdDb = ejdsp::dBp (harm / fund); T.thdnDb = ejdsp::dBp (std::max (0.0, total - fund) / fund);
        }
        R.segs.push_back (T);
    }
    for (size_t i = 1; i < L.segments.size(); ++i)
    {
        ToneStep S; S.tSec = L.segments[i].t0;
        const size_t kb = (size_t) std::llround (S.tSec * 1000.0);
        auto meanOver = [&] (double a, double b) { std::vector<double> v; for (size_t k = (size_t) std::llround ((S.tSec + a) * 1000.0); k < (size_t) std::llround ((S.tSec + b) * 1000.0) && k < gr.size(); ++k) v.push_back (gr[k]); return mean (v); };
        S.fromDb = meanOver (-1.0, -0.05); S.toDb = meanOver (2.0, 3.9);
        if (! isnan (S.fromDb) && ! isnan (S.toDb) && std::abs (S.toDb - S.fromDb) > 0.5)
        {
            const bool down = S.toDb < S.fromDb;
            auto timeTo = [&] (double frac) { const double target = S.fromDb + frac * (S.toDb - S.fromDb); for (size_t k = kb >= 30 ? kb - 30 : 0; k < kb + 3000 && k < gr.size(); ++k) if (! isnan (gr[k]) && (down ? gr[k] <= target : gr[k] >= target)) return (double) ((long long) k - (long long) kb); return NaN; };
            S.t10 = timeTo (0.1); S.t50 = timeTo (0.5); S.t90 = timeTo (0.9);
        }
        R.steps.push_back (S);
    }
    return R;
}

// ---------------------------------------------------------------------------------------------------------------
// 7. PUMPING and CREST: momentary (400 ms / 100 ms) GR series over the loud region - its std and peak-to-peak
//    are the level modulation the limiter ADDED; crest factor (peak - RMS per 400 ms) in and out.
// ---------------------------------------------------------------------------------------------------------------
struct Dynamics { double grMeanDb = NaN, grStdDb = NaN, grP2pDb = NaN, grMaxDb = NaN, crestInDb = NaN, crestOutDb = NaN, grLRstdDb = NaN; };
inline Dynamics dynamics (const Pair& p)
{
    Dynamics D; const size_t N = p.frames(), blk = (size_t) std::llround (0.4 * p.sr), hop = (size_t) std::llround (0.1 * p.sr);
    std::vector<double> gr, ci, co; const double g2 = p.G * p.G;
    for (size_t b = 0; b + blk <= N; b += hop)
    {
        double ei = 0, eo = 0, pi = 0, po = 0;
        for (int c = 0; c < p.channels(); ++c) for (size_t n = b; n < b + blk; ++n) { const double i = p.in[(size_t) c][n], o = p.out[(size_t) c][n]; ei += i * i; eo += o * o; pi = std::max (pi, std::abs (i)); po = std::max (po, std::abs (o)); }
        const double rmsIn = std::sqrt (ei / (double) (blk * (size_t) p.channels())) * p.G, rmsOut = std::sqrt (eo / (double) (blk * (size_t) p.channels()));
        if (ejdsp::dB (rmsIn) < -40.0) continue;
        gr.push_back (ejdsp::dBp (eo / (g2 * ei))); ci.push_back (ejdsp::dB (pi * p.G) - ejdsp::dB (rmsIn)); co.push_back (ejdsp::dB (po) - ejdsp::dB (rmsOut));
    }
    D.grMeanDb = mean (gr); D.grStdDb = stddev (gr); D.crestInDb = mean (ci); D.crestOutDb = mean (co);
    if (! gr.empty()) { const auto mm = std::minmax_element (gr.begin(), gr.end()); D.grP2pDb = *mm.second - *mm.first; D.grMaxDb = *mm.first; }
    if (p.channels() >= 2) { const size_t B = (size_t) std::llround (p.sr / 1000.0); const auto l = grSeries (p, B, 0), r = grSeries (p, B, 1); std::vector<double> d; for (size_t k = 0; k < l.size(); ++k) if (! isnan (l[k]) && ! isnan (r[k])) d.push_back (l[k] - r[k]); D.grLRstdDb = stddev (d); }
    return D;
}

// ---------------------------------------------------------------------------------------------------------------
// Everything for one render, in one place: the report prints it and the comparison reads it.
// ---------------------------------------------------------------------------------------------------------------
struct Report
{
    std::string caseName, tag; bool aligned = false; AlignResult align; double gainDb = 0, ceilingDb = 0, sr = 0; size_t frames = 0;
    Loudness inLoud, outLoud; Peaks pk; Dynamics dyn; std::vector<Hit> hits; size_t expectedHits = 0; ToneResult tone; bool isTone = false;
};

inline Report analyse (const std::string& caseName, const std::string& tag, const ejwav::Audio& src, const ejwav::Audio& proc, double gainDb, double ceilingDb)
{
    Report R; R.caseName = caseName; R.tag = tag; R.gainDb = gainDb; R.ceilingDb = ceilingDb; R.sr = src.sampleRate;
    const auto L = ejfix::layout (caseName);
    std::vector<std::pair<size_t, size_t>> anchors; for (const auto& m : L.markers) anchors.push_back ({ (size_t) std::llround (m.first * src.sampleRate), (size_t) std::llround ((m.second - m.first) * src.sampleRate) });
    R.align = align (src, proc, anchors); R.aligned = R.align.ok; if (! R.aligned) return R;
    const Pair p = makePair (src, proc, R.align.offset, gainDb, ceilingDb); R.frames = p.frames();
    R.inLoud = loudness (p.in, p.sr, gainDb); R.outLoud = loudness (p.out, p.sr); R.pk = peaks (p.out, p.sr, ceilingDb); R.dyn = dynamics (p);
    R.isTone = L.isTone();
    if (L.isTone()) R.tone = analyseTone (p, L);
    else { R.hits = analyseHits (p, detectOnsets (p)); R.expectedHits = L.events.size(); }
    return R;
}

} // namespace ejm
