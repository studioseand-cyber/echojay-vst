/*
    EedKeyEngine.cpp  —  see EedKeyEngine.h for the pipeline overview.

    Numbers that shape the analysis live at the top of the anonymous namespace;
    everything below is the mechanics.
*/

#include "EedKeyEngine.h"
#include "EqFft.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace echojay
{

namespace
{
    // ---- the analysis geometry --------------------------------------------
    // Everything runs at a decimated rate near this. Pitch information above
    // ~7 kHz contributes nothing a harmonic sum cannot get from lower
    // partials, and every stage's cost scales with this rate.
    constexpr double kTargetRateHz = 16000.0;

    // 3 sub-bins per semitone. The sub-bin resolution is what the tuning
    // estimator reads: energy sitting between semitone centres shows up as a
    // consistent sub-bin phase, and 33 cents per sub-bin resolves a mistuning
    // to a few cents once the circular mean is taken over the whole range.
    constexpr int    kBinsPerSemitone = 3;
    constexpr int    kBinsPerOctave   = 36;
    constexpr double kCentsPerSubbin  = 100.0 / kBinsPerSemitone;

    // Constant Q for 36 bins/octave: fk / (2^(1/36) - 1).
    const double kQ = 1.0 / (std::pow (2.0, 1.0 / kBinsPerOctave) - 1.0);

    // A constant-Q window in the bass would want ~0.9 s at A1; longer than a
    // second stops being "this segment" and starts being "the whole intro".
    constexpr double kMaxCqtWindowS = 1.0;

    // Segment hop. ~0.5 s per chroma vector is the spec's segment scale:
    // short enough that a modulation registers within a couple of segments,
    // long enough that one strum does not read as a key.
    constexpr double kSegmentHopS = 0.5;

    // ---- HPSS (median-filtering, Fitzgerald 2010) -------------------------
    constexpr int kHpssFft  = 1024;          // 64 ms at 16 kHz
    constexpr int kHpssHop  = kHpssFft / 4;  // Hann at 75% overlap → COLA
    constexpr int kMedTime  = 17;            // ~0.27 s of frames per median
    constexpr int kMedFreq  = 17;            // ~265 Hz of bins per median

    // ---- whitening / harmonic summation -----------------------------------
    constexpr int kWhitenHalfSpan = 18;      // ± half an octave of sub-bins

    // h = 1..4 folded onto the fundamental. 1/h-ish weights: enough that a
    // bass note's overtones reinforce its class, not so much that the fifth
    // (h = 3) starts outvoting the root.
    constexpr int   kHarmOffsets[4] = { 0, 36, 57, 72 };   // 36·log2(h), rounded
    constexpr float kHarmWeights[4] = { 1.0f, 0.6f, 0.4f, 0.3f };

    // ---- decision stage ----------------------------------------------------
    // Temperley's revision of the Krumhansl-Schmuckler profiles
    // (D. Temperley, "What's Key for Key? The Krumhansl-Schmuckler Key-Finding
    // Algorithm Reconsidered", Music Perception 17(1), 1999) — validated
    // against popular material, where the raw K-S weights over-favour the
    // relative major.
    constexpr float kMajorProfile[12] = { 5.0f, 2.0f, 3.5f, 2.0f, 4.5f, 4.0f,
                                          2.0f, 4.5f, 2.0f, 3.5f, 1.5f, 4.0f };
    constexpr float kMinorProfile[12] = { 5.0f, 2.0f, 3.5f, 4.5f, 2.0f, 4.0f,
                                          2.0f, 4.5f, 3.5f, 2.0f, 1.5f, 4.0f };

    // Viterbi emission scale and the sensitivity→switch-penalty map. The
    // penalty is in emission units: at sensitivity 0 a key change must be
    // worth ~8 correlation-scaled units to register, at 100 only ~1.
    constexpr float kEmissionScale = 5.0f;
    inline float switchPenaltyFor (float sensPct)
    { return 1.0f + (1.0f - sensPct / 100.0f) * 7.0f; }

    // Continuous-mode hysteresis on the aggregate correlation, same idea.
    inline float hysteresisFor (float sensPct)
    { return 0.01f + (1.0f - sensPct / 100.0f) * 0.08f; }

    // Confidence: winner correlation, scaled by how decisively it beats the
    // runner-up. margin/(margin+knee) ≈ 0.33 at a 2-point margin and ≈ 0.9 at
    // a 36-point one, so "C major 0.90 / A minor 0.88" honestly reads LOW.
    constexpr float kMarginKnee = 0.04f;

    // Segment chroma is peak-normalised and mildly sharpened before the
    // profile correlations. The diatonic SET is shared between a key and its
    // relative/dominant neighbours; what distinguishes them is the tonic
    // emphasis, and sharpening amplifies exactly that part of the evidence.
    constexpr float kChromaSharpen = 1.5f;

    // ...and by how tonal the material was at all. Whitened noise and drums
    // fold to a near-flat chroma; spectral flatness of the aggregate chroma is
    // the honest "was there even a key to find" gate. Measured on the SQUARED
    // chroma: a full diatonic texture legitimately lights seven classes, which
    // already reads fairly flat — squaring separates "seven peaks of different
    // heights" (music) from "twelve of the same height" (noise) instead of
    // punishing both.
    inline float tonalityOf (const std::array<float, 12>& chroma)
    {
        double logSum = 0.0, sum = 0.0;
        for (float c : chroma)
        {
            const double v = (double) c * (double) c + 1.0e-6;
            logSum += std::log (v);
            sum    += v;
        }
        const double flat = std::exp (logSum / 12.0) / (sum / 12.0);
        const double t = (1.0 - flat) * 1.8;
        return (float) (t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
    }

    constexpr float kSilenceRms = 1.0e-5f;   // < ~-100 dBFS: nothing to analyse

    // Live-chroma refresh geometry (display only).
    constexpr double kLiveSpanS = 1.0;
    constexpr double kLiveMinNewS = 0.25;

    inline float clamp01 (float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    inline float pearson (const std::array<float, 12>& a, const float* b, int rot)
    {
        // b is read rotated so index 0 of the profile lands on pitch class
        // `rot` of the chroma — i.e. correlation against the profile in the
        // key whose tonic is `rot`.
        double ma = 0.0, mb = 0.0;
        for (int i = 0; i < 12; ++i) { ma += a[(std::size_t) i]; mb += b[i]; }
        ma /= 12.0; mb /= 12.0;

        double num = 0.0, da = 0.0, db = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const double xa = a[(std::size_t) ((i + rot) % 12)] - ma;
            const double xb = b[i] - mb;
            num += xa * xb; da += xa * xa; db += xb * xb;
        }
        const double den = std::sqrt (da * db);
        return den > 1.0e-12 ? (float) (num / den) : 0.0f;
    }

    inline float medianOfWindow (const float* src, int stride, int lo, int hi, float* scratch)
    {
        int n = 0;
        for (int i = lo; i <= hi; ++i) scratch[n++] = src[i * stride];
        std::nth_element (scratch, scratch + n / 2, scratch + n);
        return scratch[n / 2];
    }
}

// ---------------------------------------------------------------------------
KeyEngine::KeyEngine()
{
    history_.assign ((std::size_t) kRingSize, 0.0f);
    prepare (48000.0, 512);
}

void KeyEngine::Biquad::makeLowpass (double fs, double fc, double q)
{
    const double w0 = 2.0 * 3.14159265358979323846 * fc / fs;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = ((1.0 - cw) * 0.5) / a0;
    b1 =  (1.0 - cw)        / a0;
    b2 = ((1.0 - cw) * 0.5) / a0;
    a1 = (-2.0 * cw)        / a0;
    a2 = (1.0 - alpha)      / a0;
    resetState();
}

void KeyEngine::prepare (double sampleRate, int /*maxBlockSize*/)
{
    fs_ = sampleRate > 8000.0 ? sampleRate : 48000.0;

    decimFactor_ = (int) std::lround (fs_ / kTargetRateHz);
    if (decimFactor_ < 1) decimFactor_ = 1;
    fsA_ = fs_ / (double) decimFactor_;

    // 4th-order Butterworth (Q = 0.5412 / 1.3066) at 0.45 of the decimated
    // Nyquist-defining rate: what aliases past this is content the analysis
    // band never looks at anyway.
    const double fc = 0.45 * fsA_;
    aa1_.makeLowpass (fs_, fc, 0.54119610);
    aa2_.makeLowpass (fs_, fc, 1.30656296);
    decimPhase_ = 0;

    reset();
}

void KeyEngine::reset()
{
    ring_.fill (0.0f);
    ringWrite_.store (0, std::memory_order_release);
    ringRead_  = 0;
    histCount_ = 0;
    std::fill (history_.begin(), history_.end(), 0.0f);
    aa1_.resetState();
    aa2_.resetState();
    decimPhase_ = 0;

    collecting_.store (false);
    armedAt_ = 0;
    lastContinuousAt_ = 0;

    std::lock_guard<std::mutex> lock (readingMutex_);
    reading_ = KeyReading {};
    liveChroma_.fill (0.0f);
    haveLiveChroma_ = false;
}

// ---------------------------------------------------------------------------
// audio thread
// ---------------------------------------------------------------------------
void KeyEngine::pushBlock (const float* l, const float* r, int n) noexcept
{
    if (l == nullptr || n <= 0) return;

    uint64_t w = ringWrite_.load (std::memory_order_relaxed);

    for (int i = 0; i < n; ++i)
    {
        const float mono = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
        const float f = aa2_.process (aa1_.process (mono));

        if (++decimPhase_ >= decimFactor_)
        {
            decimPhase_ = 0;
            ring_[(std::size_t) (w & (uint64_t) kRingMask)] = f;
            ++w;
        }
    }

    // Release pairs with the acquire in drainRing: the samples are visible
    // before the count that claims they exist.
    ringWrite_.store (w, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// consumer side
// ---------------------------------------------------------------------------
void KeyEngine::drainRing()
{
    const uint64_t w = ringWrite_.load (std::memory_order_acquire);
    if (w == ringRead_) return;

    // Fell more than a ring behind (host stalled the consumer): skip forward,
    // keeping the newest ring's worth. Analysing stale audio as if it were
    // current would be worse than the gap.
    if (w - ringRead_ > (uint64_t) kRingSize)
        ringRead_ = w - (uint64_t) kRingSize;

    while (ringRead_ < w)
    {
        history_[(std::size_t) (histCount_ & (uint64_t) kRingMask)] =
            ring_[(std::size_t) (ringRead_ & (uint64_t) kRingMask)];
        ++ringRead_;
        ++histCount_;
    }
}

int KeyEngine::copyRecent (std::vector<float>& dest, int n) const
{
    if (n <= 0 || histCount_ == 0) return 0;
    if ((uint64_t) n > histCount_)          n = (int) histCount_;
    if (n > kRingSize)                      n = kRingSize;

    dest.resize ((std::size_t) n);
    const uint64_t start = histCount_ - (uint64_t) n;
    for (int i = 0; i < n; ++i)
        dest[(std::size_t) i] =
            history_[(std::size_t) ((start + (uint64_t) i) & (uint64_t) kRingMask)];
    return n;
}

// ---------------------------------------------------------------------------
// HPSS — median-filtering harmonic/percussive separation, in place.
// ---------------------------------------------------------------------------
void KeyEngine::hpssSeparate (std::vector<float>& x) const
{
    const int n = (int) x.size();
    if (n < kHpssFft * 2) return;                    // too short to separate

    const int bins   = kHpssFft / 2 + 1;
    const int frames = (n - kHpssFft) / kHpssHop + 1;
    if (frames < kMedTime) return;

    // Analysis window (Hann), shared by synthesis.
    std::vector<float> win ((std::size_t) kHpssFft);
    for (int i = 0; i < kHpssFft; ++i)
        win[(std::size_t) i] =
            0.5f - 0.5f * std::cos (6.28318530717958647692 * (double) i / (double) kHpssFft);

    std::vector<float> re ((std::size_t) frames * (std::size_t) bins);
    std::vector<float> im ((std::size_t) frames * (std::size_t) bins);
    std::vector<float> mag ((std::size_t) frames * (std::size_t) bins);

    {
        std::vector<float> fr ((std::size_t) kHpssFft), fi ((std::size_t) kHpssFft);
        for (int t = 0; t < frames; ++t)
        {
            const int off = t * kHpssHop;
            for (int i = 0; i < kHpssFft; ++i)
            {
                fr[(std::size_t) i] = x[(std::size_t) (off + i)] * win[(std::size_t) i];
                fi[(std::size_t) i] = 0.0f;
            }
            eqFft (fr.data(), fi.data(), kHpssFft, false);

            float* rRow = &re[(std::size_t) t * (std::size_t) bins];
            float* iRow = &im[(std::size_t) t * (std::size_t) bins];
            float* mRow = &mag[(std::size_t) t * (std::size_t) bins];
            for (int b = 0; b < bins; ++b)
            {
                rRow[b] = fr[(std::size_t) b];
                iRow[b] = fi[(std::size_t) b];
                mRow[b] = std::sqrt (rRow[b] * rRow[b] + iRow[b] * iRow[b]);
            }
        }
    }

    // Median across TIME (per bin) → harmonic-enhanced; median across
    // FREQUENCY (per frame) → percussive-enhanced. Windows shrink at the
    // edges rather than padding: a padded zero would darken the medians at
    // both ends of the pass for no physical reason.
    std::vector<float> scratch ((std::size_t) std::max (kMedTime, kMedFreq));
    std::vector<float> harm ((std::size_t) frames * (std::size_t) bins);
    std::vector<float> perc ((std::size_t) frames * (std::size_t) bins);

    const int htT = kMedTime / 2, htF = kMedFreq / 2;
    for (int t = 0; t < frames; ++t)
    {
        const int tLo = std::max (0, t - htT), tHi = std::min (frames - 1, t + htT);
        for (int b = 0; b < bins; ++b)
        {
            harm[(std::size_t) t * (std::size_t) bins + (std::size_t) b] =
                medianOfWindow (&mag[(std::size_t) b], bins, tLo, tHi, scratch.data());
        }

        const float* mRow = &mag[(std::size_t) t * (std::size_t) bins];
        float* pRow = &perc[(std::size_t) t * (std::size_t) bins];
        for (int b = 0; b < bins; ++b)
        {
            const int bLo = std::max (0, b - htF), bHi = std::min (bins - 1, b + htF);
            pRow[b] = medianOfWindow (mRow, 1, bLo, bHi, scratch.data());
        }
    }

    // Wiener soft mask (power 2), applied to the complex frames; ISTFT with
    // the same Hann and window-squared normalisation.
    std::vector<float> out ((std::size_t) n, 0.0f);
    std::vector<float> wsum ((std::size_t) n, 0.0f);
    {
        std::vector<float> fr ((std::size_t) kHpssFft), fi ((std::size_t) kHpssFft);
        for (int t = 0; t < frames; ++t)
        {
            const float* rRow = &re[(std::size_t) t * (std::size_t) bins];
            const float* iRow = &im[(std::size_t) t * (std::size_t) bins];
            const float* hRow = &harm[(std::size_t) t * (std::size_t) bins];
            const float* pRow = &perc[(std::size_t) t * (std::size_t) bins];

            for (int b = 0; b < bins; ++b)
            {
                const float h2 = hRow[b] * hRow[b];
                const float p2 = pRow[b] * pRow[b];
                const float m  = h2 / (h2 + p2 + 1.0e-12f);
                fr[(std::size_t) b] = rRow[b] * m;
                fi[(std::size_t) b] = iRow[b] * m;
            }
            // Hermitian mirror for the real inverse.
            for (int b = 1; b < kHpssFft / 2; ++b)
            {
                fr[(std::size_t) (kHpssFft - b)] =  fr[(std::size_t) b];
                fi[(std::size_t) (kHpssFft - b)] = -fi[(std::size_t) b];
            }
            eqFft (fr.data(), fi.data(), kHpssFft, true);

            const int off = t * kHpssHop;
            for (int i = 0; i < kHpssFft; ++i)
            {
                const float w = win[(std::size_t) i];
                out[(std::size_t) (off + i)]  += fr[(std::size_t) i] * w;
                wsum[(std::size_t) (off + i)] += w * w;
            }
        }
    }

    for (int i = 0; i < n; ++i)
        x[(std::size_t) i] = wsum[(std::size_t) i] > 1.0e-6f
                               ? out[(std::size_t) i] / wsum[(std::size_t) i]
                               : x[(std::size_t) i];
}

// ---------------------------------------------------------------------------
// Constant-Q salience per segment: direct per-bin evaluation (log-spaced bins
// need no FFT compromise), whitened, harmonics folded onto fundamentals.
// ---------------------------------------------------------------------------
void KeyEngine::computeSegmentSalience (const std::vector<float>& x,
                                        std::vector<std::array<float, 36>>& wrappedPerSeg,
                                        std::vector<float>& aggSalience,
                                        int& bLoOut, int& bHiOut) const
{
    wrappedPerSeg.clear();
    aggSalience.clear();

    const int n = (int) x.size();
    const double ref = (double) tuningHint_.load();      // the grid's reference

    // Sub-bin b sits at midi m = b/3 on a grid whose A4 (midi 69) is `ref`.
    auto freqOf = [ref] (int b)
    {
        return ref * std::pow (2.0, ((double) b / (double) kBinsPerSemitone - 69.0) / 12.0);
    };

    const double loHz = (double) lowHz_.load();
    const double hiHz = std::min ((double) highHz_.load(), 0.45 * fsA_);

    int bLo = 0;
    while (freqOf (bLo) < loHz) ++bLo;
    int bHi = bLo;
    while (freqOf (bHi + 1) <= hiHz) ++bHi;
    bLoOut = bLo; bHiOut = bHi;
    if (bHi - bLo < kBinsPerOctave) return;              // less than an octave: nothing usable

    const int hopN   = std::max (1, (int) std::lround (kSegmentHopS * fsA_));
    const int maxWin = (int) std::lround (kMaxCqtWindowS * fsA_);
    const int nBins  = bHi - bLo + 1;

    aggSalience.assign ((std::size_t) nBins, 0.0f);

    std::vector<float> mags ((std::size_t) nBins);
    std::vector<float> whitened ((std::size_t) nBins);
    std::vector<float> logMag ((std::size_t) nBins);

    // Segment ends stepping back from the newest sample; earliest first so
    // wrappedPerSeg is chronological for the Viterbi pass.
    std::vector<int> ends;
    for (int e = n; e >= std::min (n, maxWin); e -= hopN)
    {
        ends.push_back (e);
        if ((int) ends.size() > 4096) break;             // paranoia bound
    }
    std::reverse (ends.begin(), ends.end());
    if (ends.empty()) return;

    constexpr double kTwoPi = 6.28318530717958647692;

    for (int end : ends)
    {
        // ---- constant-Q magnitudes ------------------------------------
        for (int b = bLo; b <= bHi; ++b)
        {
            const double fk = freqOf (b);
            int Nk = (int) std::lround (kQ * fsA_ / fk);
            if (Nk > maxWin) Nk = maxWin;
            if (Nk > end)    Nk = end;
            if (Nk < 8) { mags[(std::size_t) (b - bLo)] = 0.0f; continue; }

            const int start = end - Nk;

            // Two recurrence oscillators: the analysis tone and the Hann
            // window. Double phase recurrences keep an 16k-sample rotation
            // well under float noise.
            const double w  = kTwoPi * fk / fsA_;
            const double cw = std::cos (w),  sw = std::sin (w);
            const double hw = kTwoPi / (double) Nk;
            const double ch = std::cos (hw), sh = std::sin (hw);

            double cr = 1.0, ci = 0.0;                   // e^{-j w t} rotated forward
            double hr = 1.0, hi2 = 0.0;                  // cos(hann phase)

            double accR = 0.0, accI = 0.0, wSum = 0.0;
            for (int i = 0; i < Nk; ++i)
            {
                const double wnd = 0.5 - 0.5 * hr;
                const double v   = (double) x[(std::size_t) (start + i)] * wnd;
                accR += v * cr;
                accI -= v * ci;
                wSum += wnd;

                double t  = cr * cw - ci * sw; ci = cr * sw + ci * cw; cr = t;
                t = hr * ch - hi2 * sh; hi2 = hr * sh + hi2 * ch; hr = t;
            }

            mags[(std::size_t) (b - bLo)] =
                wSum > 1.0 ? (float) (2.0 * std::sqrt (accR * accR + accI * accI) / wSum)
                           : 0.0f;
        }

        // ---- spectral whitening (log-magnitude minus local envelope) ----
        for (int i = 0; i < nBins; ++i)
            logMag[(std::size_t) i] = std::log (mags[(std::size_t) i] + 1.0e-9f);

        for (int i = 0; i < nBins; ++i)
        {
            const int lo = std::max (0, i - kWhitenHalfSpan);
            const int hi = std::min (nBins - 1, i + kWhitenHalfSpan);
            double env = 0.0;
            for (int j = lo; j <= hi; ++j) env += logMag[(std::size_t) j];
            env /= (double) (hi - lo + 1);
            const double v = (double) logMag[(std::size_t) i] - env;
            whitened[(std::size_t) i] = v > 0.0 ? (float) v : 0.0f;
        }

        // ---- harmonic summation onto fundamentals, then wrap ------------
        std::array<float, 36> wrapped {};
        for (int i = 0; i < nBins; ++i)
        {
            float sal = 0.0f;
            for (int h = 0; h < 4; ++h)
            {
                const int j = i + kHarmOffsets[h];
                if (j >= nBins) break;
                sal += kHarmWeights[h] * whitened[(std::size_t) j];
            }
            aggSalience[(std::size_t) i] += sal;
            wrapped[(std::size_t) (((bLo + i) % kBinsPerOctave + kBinsPerOctave)
                                   % kBinsPerOctave)] += sal;
        }
        wrappedPerSeg.push_back (wrapped);
    }
}

// ---------------------------------------------------------------------------
// Tuning: circular mean of sub-bin position. Sub-bin b ≡ 0 (mod 3) sits ON a
// semitone centre; ≡1 is +33 c, ≡2 is −33 c (of the next semitone up). Energy
// consistently off-centre resolves to one phase; in-tune material resolves to
// zero. Returns the offset IN SUB-BINS (±1.5 = ±50 cents).
// ---------------------------------------------------------------------------
float KeyEngine::estimateTuningSubbins (const std::vector<std::array<float, 36>>& wrapped)
{
    double sr = 0.0, si = 0.0;
    constexpr double kThird = 2.0943951023931953;        // 2π/3

    for (const auto& w36 : wrapped)
        for (int b = 0; b < 36; ++b)
        {
            const int    sub = b % kBinsPerSemitone;      // 0, +1, −1 pattern
            const double pos = sub == 2 ? -1.0 : (double) sub;
            const double th  = pos * kThird;
            sr += (double) w36[(std::size_t) b] * std::cos (th);
            si += (double) w36[(std::size_t) b] * std::sin (th);
        }

    if (sr * sr + si * si < 1.0e-12) return 0.0f;
    return (float) (std::atan2 (si, sr) / kThird);
}

// Fold the wrapped 36-bin salience to 12 pitch classes, reading each class at
// its TUNED position (semitone centre + the estimated offset), linearly
// interpolated between sub-bins.
void KeyEngine::foldChroma (const std::array<float, 36>& wrapped, float offSub,
                            std::array<float, 12>& chromaOut)
{
    for (int c = 0; c < 12; ++c)
    {
        const double p  = (double) (c * kBinsPerSemitone) + (double) offSub;
        const double pw = p - 36.0 * std::floor (p / 36.0);
        const int    i0 = (int) pw % 36;
        const int    i1 = (i0 + 1) % 36;
        const double t  = pw - std::floor (pw);
        chromaOut[(std::size_t) c] =
            (float) ((1.0 - t) * (double) wrapped[(std::size_t) i0]
                   + t         * (double) wrapped[(std::size_t) i1]);
    }
}

// ---------------------------------------------------------------------------
// The full pass.
// ---------------------------------------------------------------------------
KeyEngine::PassResult KeyEngine::analyseWindow (int n, bool hpssOn)
{
    PassResult r;

    const int got = copyRecent (work_, n);
    if (got < (int) (0.5 * fsA_)) return r;

    // Silence gate — a key detected in silence would be a key detected in the
    // anti-alias filter's noise floor.
    {
        double sumSq = 0.0;
        for (float v : work_) sumSq += (double) v * (double) v;
        if (std::sqrt (sumSq / (double) got) < kSilenceRms) return r;
    }

    if (hpssOn) hpssSeparate (work_);

    std::vector<std::array<float, 36>> wrapped;
    std::vector<float> aggSal;
    int bLo = 0, bHi = 0;
    computeSegmentSalience (work_, wrapped, aggSal, bLo, bHi);
    if (wrapped.empty()) return r;

    // ---- tuning first ------------------------------------------------------
    const bool  autoTune = autoTuning_.load();
    const float offSub   = autoTune ? estimateTuningSubbins (wrapped) : 0.0f;
    r.cents = (float) ((double) offSub * kCentsPerSubbin);

    // ---- per-segment tuned chroma + profile correlations -------------------
    const int nSeg = (int) wrapped.size();
    const int lock = modeLock_.load();

    std::vector<std::array<float, 24>> segCorr ((std::size_t) nSeg);
    std::array<double, 24> agg {};
    std::array<double, 12> chromaSum {};

    for (int s = 0; s < nSeg; ++s)
    {
        std::array<float, 12> chroma {};
        foldChroma (wrapped[(std::size_t) s], offSub, chroma);
        for (int c = 0; c < 12; ++c) chromaSum[(std::size_t) c] += chroma[(std::size_t) c];

        // Peak-normalise + sharpen for the correlations (the reported chroma
        // stays linear — sharpening is decision fuel, not display truth).
        {
            float mx = 0.0f;
            for (float v : chroma) mx = std::max (mx, v);
            if (mx > 1.0e-9f)
                for (int c = 0; c < 12; ++c)
                    chroma[(std::size_t) c] =
                        std::pow (chroma[(std::size_t) c] / mx, kChromaSharpen);
        }

        for (int root = 0; root < 12; ++root)
        {
            const float cMaj = pearson (chroma, kMajorProfile, root);
            const float cMin = pearson (chroma, kMinorProfile, root);
            segCorr[(std::size_t) s][(std::size_t) root]        = lock == kLockMinor ? -1.0f : cMaj;
            segCorr[(std::size_t) s][(std::size_t) (root + 12)] = lock == kLockMajor ? -1.0f : cMin;
        }
        for (int k = 0; k < 24; ++k) agg[(std::size_t) k] += segCorr[(std::size_t) s][(std::size_t) k];
    }
    for (int k = 0; k < 24; ++k)
    {
        agg[(std::size_t) k] /= (double) nSeg;
        r.score[(std::size_t) k] = (float) agg[(std::size_t) k];
    }

    // ---- Viterbi over the segment sequence ---------------------------------
    // Self-transitions are free; a switch costs `penalty` in emission units.
    // On stationary material this is a formality; on real songs it is what
    // keeps two ambiguous middle-eight segments from flipping the answer.
    const float penalty = switchPenaltyFor (sensitivity_.load());

    std::vector<std::array<float, 24>> dp ((std::size_t) nSeg);
    std::vector<std::array<int, 24>>   bp ((std::size_t) nSeg);

    for (int k = 0; k < 24; ++k)
    {
        dp[0][(std::size_t) k] = kEmissionScale * segCorr[0][(std::size_t) k];
        bp[0][(std::size_t) k] = k;
    }
    for (int s = 1; s < nSeg; ++s)
    {
        // The best predecessor is either yourself (free) or the globally best
        // previous state minus the switch cost — O(24) not O(24²).
        int   bestPrev = 0;
        float bestVal  = dp[(std::size_t) (s - 1)][0];
        for (int k = 1; k < 24; ++k)
            if (dp[(std::size_t) (s - 1)][(std::size_t) k] > bestVal)
            { bestVal = dp[(std::size_t) (s - 1)][(std::size_t) k]; bestPrev = k; }

        for (int k = 0; k < 24; ++k)
        {
            const float stay   = dp[(std::size_t) (s - 1)][(std::size_t) k];
            const float sw     = bestVal - penalty;
            const bool  switching = sw > stay && bestPrev != k;
            dp[(std::size_t) s][(std::size_t) k] =
                (switching ? sw : stay) + kEmissionScale * segCorr[(std::size_t) s][(std::size_t) k];
            bp[(std::size_t) s][(std::size_t) k] = switching ? bestPrev : k;
        }
    }

    // Backtrack; the answer is the MODAL state of the path — the key the
    // window actually lived in, robust to a modulation at either edge.
    std::array<int, 24> occupancy {};
    {
        int k = 0;
        float best = dp[(std::size_t) (nSeg - 1)][0];
        for (int j = 1; j < 24; ++j)
            if (dp[(std::size_t) (nSeg - 1)][(std::size_t) j] > best)
            { best = dp[(std::size_t) (nSeg - 1)][(std::size_t) j]; k = j; }

        for (int s = nSeg - 1; s >= 0; --s)
        {
            ++occupancy[(std::size_t) k];
            k = bp[(std::size_t) s][(std::size_t) k];
        }
    }
    int winner = 0;
    for (int k = 1; k < 24; ++k)
        if (occupancy[(std::size_t) k] > occupancy[(std::size_t) winner]) winner = k;

    // ---- confidence: winner against runner-up, gated by tonality -----------
    const float s1 = (float) agg[(std::size_t) winner];
    float s2 = -2.0f;
    for (int k = 0; k < 24; ++k)
        if (k != winner && (float) agg[(std::size_t) k] > s2) s2 = (float) agg[(std::size_t) k];

    const float margin = s1 - s2;

    // Aggregate chroma, normalised for display and for the tonality gate.
    std::array<float, 12> chromaAvg {};
    {
        double mx = 0.0;
        for (int c = 0; c < 12; ++c) mx = std::max (mx, chromaSum[(std::size_t) c]);
        for (int c = 0; c < 12; ++c)
            chromaAvg[(std::size_t) c] = mx > 1.0e-12
                ? (float) (chromaSum[(std::size_t) c] / mx) : 0.0f;
    }

    r.ok     = true;
    r.key    = winner;
    r.conf   = clamp01 (s1) * (margin > 0.0f ? margin / (margin + kMarginKnee) : 0.0f)
                            * tonalityOf (chromaAvg);
    r.chroma = chromaAvg;

    // ---- the root's fundamental, in the octave the signal actually used ----
    {
        const int rc = winner % 12;
        const double refTuned = (double) tuningHint_.load()
                              * std::pow (2.0, (double) r.cents / 1200.0);

        int   bestMidi = 36 + rc;                        // C2-octave fallback
        float bestSal  = -1.0f;
        for (int m = 24 + rc; m <= 69; m += 12)          // C1 .. A4
        {
            const int b = m * kBinsPerSemitone;
            if (b < bLo || b > bHi) continue;
            const float sal = aggSal[(std::size_t) (b - bLo)];
            if (sal > bestSal) { bestSal = sal; bestMidi = m; }
        }
        r.rootMidi = bestMidi;
        r.rootHz   = (float) (refTuned * std::pow (2.0, (double) (bestMidi - 69) / 12.0));
    }

    return r;
}

// ---------------------------------------------------------------------------
// state machine
// ---------------------------------------------------------------------------
void KeyEngine::startAnalysis()
{
    drainRing();
    armedAt_ = histCount_;
    collecting_.store (true);
}

void KeyEngine::cancelAnalysis()
{
    collecting_.store (false);
}

float KeyEngine::collectionProgress() const
{
    if (! collecting_.load()) return 0.0f;
    const double winN = (double) windowS_.load() * fsA_;
    if (winN <= 0.0) return 0.0f;
    return clamp01 ((float) ((double) (histCount_ - armedAt_) / winN));
}

void KeyEngine::clearAccumulation()
{
    drainRing();
    ringRead_  = ringWrite_.load (std::memory_order_acquire);
    histCount_ = 0;
    collecting_.store (false);

    std::lock_guard<std::mutex> lock (readingMutex_);
    reading_ = KeyReading {};
    liveChroma_.fill (0.0f);
    haveLiveChroma_ = false;
}

void KeyEngine::publish (const PassResult& r, bool committed, float seconds)
{
    const float hint  = tuningHint_.load();
    const float tuned = hint * std::pow (2.0f, r.cents / 1200.0f);

    std::lock_guard<std::mutex> lock (readingMutex_);
    reading_.valid       = r.ok;
    reading_.committed   = committed;
    reading_.root        = r.key % 12;
    reading_.minor       = r.key >= 12;
    reading_.confidence  = r.conf;
    reading_.tuningHz    = tuned;
    reading_.tuningCents = 1200.0f * std::log2 (tuned / 440.0f);
    reading_.rootHz      = r.rootHz;
    reading_.rootMidi    = r.rootMidi;
    reading_.analysedSeconds = seconds;
    reading_.chroma      = r.chroma;
    reading_.score       = r.score;

    reading_.numAlternates = 0;
    std::array<bool, 24> used {};
    used[(std::size_t) r.key] = true;
    for (int slot = 0; slot < 3; ++slot)
    {
        int   best = -1;
        float bestScore = -2.0f;
        for (int k = 0; k < 24; ++k)
            if (! used[(std::size_t) k] && r.score[(std::size_t) k] > bestScore)
            { bestScore = r.score[(std::size_t) k]; best = k; }
        if (best < 0 || bestScore <= -1.0f) break;       // mode-locked keys sit at -1
        used[(std::size_t) best] = true;
        reading_.alternates[(std::size_t) slot] = { best % 12, best >= 12, bestScore };
        ++reading_.numAlternates;
    }
}

bool KeyEngine::update()
{
    drainRing();

    bool changed = false;
    const double winN = (double) windowS_.load() * fsA_;

    if (collecting_.load())
    {
        if ((double) (histCount_ - armedAt_) >= winN)
        {
            const int n = (int) winN;
            const auto r = analyseWindow (n, hpss_.load());
            collecting_.store (false);
            if (! hold_.load())
            {
                publish (r, true, (float) ((double) n / fsA_));
                changed = true;
            }
        }
    }
    else if (continuous_.load() && ! hold_.load())
    {
        // A fresh pass every ~2 s of new audio, over up to the window length.
        const uint64_t newSamples = histCount_ - lastContinuousAt_;
        if ((double) newSamples >= 2.0 * fsA_ && (double) histCount_ >= 3.0 * fsA_)
        {
            lastContinuousAt_ = histCount_;
            const int n = (int) std::min ((double) histCount_, winN);
            const auto r = analyseWindow (n, hpss_.load());

            if (r.ok)
            {
                KeyReading cur = getReading();
                const int newKey = r.key;
                const int curKey = cur.root + (cur.minor ? 12 : 0);

                // Hysteresis: the incumbent keeps the seat unless the
                // challenger wins by a real margin in THIS pass's numbers.
                if (! cur.valid || newKey == curKey
                    || r.score[(std::size_t) newKey]
                         > r.score[(std::size_t) curKey] + hysteresisFor (sensitivity_.load()))
                {
                    publish (r, false, (float) ((double) n / fsA_));
                    changed = true;
                }
            }
        }
    }

    // ---- display-only live chroma ------------------------------------------
    {
        static_assert (kLiveSpanS <= 2.0, "live span must stay cheap");
        const int liveN = (int) (kLiveSpanS * fsA_);
        const uint64_t since = histCount_ - std::min (histCount_, lastLiveAt_);
        if ((double) since >= kLiveMinNewS * fsA_ && (int) histCount_ >= liveN)
        {
            lastLiveAt_ = histCount_;
            if (copyRecent (work_, liveN) == liveN)
            {
                double sumSq = 0.0;
                for (float v : work_) sumSq += (double) v * (double) v;
                if (std::sqrt (sumSq / (double) liveN) >= kSilenceRms)
                {
                    std::vector<std::array<float, 36>> wrapped;
                    std::vector<float> aggSal;
                    int bLo = 0, bHi = 0;
                    computeSegmentSalience (work_, wrapped, aggSal, bLo, bHi);
                    if (! wrapped.empty())
                    {
                        const float off = autoTuning_.load()
                                            ? estimateTuningSubbins (wrapped) : 0.0f;
                        std::array<float, 36> sum {};
                        for (const auto& w36 : wrapped)
                            for (int b = 0; b < 36; ++b)
                                sum[(std::size_t) b] += w36[(std::size_t) b];

                        std::array<float, 12> chroma {};
                        foldChroma (sum, off, chroma);
                        float mx = 0.0f;
                        for (float c : chroma) mx = std::max (mx, c);

                        std::lock_guard<std::mutex> lock (readingMutex_);
                        for (int c = 0; c < 12; ++c)
                            liveChroma_[(std::size_t) c] =
                                mx > 1.0e-9f ? chroma[(std::size_t) c] / mx : 0.0f;
                        haveLiveChroma_ = true;
                    }
                }
            }
        }
    }

    return changed;
}

KeyReading KeyEngine::getReading() const
{
    std::lock_guard<std::mutex> lock (readingMutex_);
    return reading_;
}

void KeyEngine::getLiveChroma (float out[12]) const
{
    std::lock_guard<std::mutex> lock (readingMutex_);
    if (haveLiveChroma_)
        for (int c = 0; c < 12; ++c) out[c] = liveChroma_[(std::size_t) c];
    else
        for (int c = 0; c < 12; ++c) out[c] = reading_.chroma[(std::size_t) c];
}

const char* KeyEngine::pitchClassName (int pc)
{
    static const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B" };
    return kNames[((pc % 12) + 12) % 12];
}

void KeyEngine::keyName (int root, bool minor, char* buf, int bufLen)
{
    if (buf == nullptr || bufLen < 16) return;
    std::snprintf (buf, (std::size_t) bufLen, "%s %s",
                   pitchClassName (root), minor ? "minor" : "major");
}

} // namespace echojay
