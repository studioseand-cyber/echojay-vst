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

    // Viterbi emission scale and the sensitivity→switch-penalty map.
    //
    // The penalty must sit at KEY timescale, not harmonic-rhythm timescale:
    // a V chord holds a ~0.4 correlation advantage over the tonic key for a
    // bar or two (≈ 4-5 half-second segments ≈ 8-10 emission units), and if
    // the penalty is below that the path obediently "modulates" to the
    // dominant for every V bar — chord tracking wearing a key tracker's
    // badge. At the default penalty of 12 a bounded chord excursion can
    // never pay for a switch, while a genuine modulation — the SUSTAINED
    // advantage of the rest of the passage — pays for it within a few
    // seconds. Sensitivity keeps its meaning: high = follows changes
    // sooner, low = stickier.
    constexpr float kEmissionScale = 5.0f;
    inline float switchPenaltyFor (float sensPct)
    { return 4.0f + (1.0f - sensPct / 100.0f) * 16.0f; }

    // Continuous-mode hysteresis on the aggregate correlation, same idea.
    inline float hysteresisFor (float sensPct)
    { return 0.01f + (1.0f - sensPct / 100.0f) * 0.08f; }

    // ---- confidence calibration --------------------------------------------
    // Calibrated against the rule that CONSUMES it (spec §4: below ~0.5 the
    // key is unknown): a correct, unambiguous reading must land comfortably
    // above 0.5, an untonal or genuinely ambiguous one comfortably below.
    // A raw winner-vs-runner-up margin fails that on purpose-built material:
    // the runner-up is usually the winner's RELATIVE major/minor, which
    // shares all seven scale degrees, so the margin stays small precisely
    // when the answer is right. Four factors instead, each answering a
    // different question:
    //
    //   S — is the winning profile a real fit at all? (absolute correlation,
    //       ramped: Pearson against a Temperley profile tops out well short
    //       of 1.0 on genuine multi-voice chroma, so raw s1 would punish
    //       every correct answer.)
    //   A — is the PITCH SET clearly this one? (margin to the best candidate
    //       that is NOT the winner's relative.)
    //   R — is the TONIC settled within the agreed pitch set? (margin to the
    //       relative itself, floored at kRelFloor: when the top two are
    //       relatives the notes are agreed and only the tonic is in
    //       question, so that gap is capped at costing 1-kRelFloor, and the
    //       relative is named in the alternates instead of dragging the
    //       whole reading under the threshold.)
    //   T — was there tonal material to read? (chroma flatness gate, below.)
    //
    // conf = S * A * R * T. Measured on the calibration battery in
    // test/key_engine_test.cpp: correct unambiguous majors/minors 0.75+,
    // drums and white noise ~0.00 — both sides pinned by tests.
    constexpr float kMarginKnee   = 0.03f;
    constexpr float kStrengthLo   = 0.15f;   // s1 at or below this: no faith
    constexpr float kStrengthSpan = 0.35f;   // s1 of 0.5+ counts as full fit
    constexpr float kRelFloor     = 0.85f;   // relative-tonic doubt costs <= 15%

    // The relative major/minor partner of key k (root + (minor ? 12 : 0)):
    // A minor <-> C major, i.e. minor -> root+3 major, major -> root+9 minor.
    inline int relativeKeyOf (int k)
    {
        const int  root  = k % 12;
        const bool minor = k >= 12;
        return minor ? (root + 3) % 12 : ((root + 9) % 12) + 12;
    }

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
        const double t = (1.0 - flat) * 2.0;
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

void KeyEngine::resetCounters()
{
    // Consumer thread only. The whole family moves together — see the header.
    histCount_        = 0;
    armedAt_          = 0;
    lastContinuousAt_ = 0;
    lastLiveAt_       = 0;
    signalSeen_       = false;
}

void KeyEngine::reset()
{
    ring_.fill (0.0f);
    ringWrite_.store (0, std::memory_order_release);
    ringRead_  = 0;
    std::fill (history_.begin(), history_.end(), 0.0f);
    resetCounters();
    aa1_.resetState();
    aa2_.resetState();
    decimPhase_ = 0;

    collecting_.store (false);
    armRequest_.store (false);
    clearRequest_.store (false);
    waitingForSignal_.store (false);
    lastPassEmpty_.store (false);
    passesCompleted_.store (0);

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
    return copyEnding (dest, histCount_, n);
}

int KeyEngine::copyEnding (std::vector<float>& dest, uint64_t endAbs, int n) const
{
    // n samples in chronological order ending at absolute count endAbs. The
    // committed pass uses this to analyse EXACTLY the promised window
    // [armedAt, armedAt + window] — analysing "the newest window at whatever
    // moment the worker happened to run" slides the passage by the drain
    // cadence, which is enough to flip a marginal call.
    if (endAbs > histCount_) endAbs = histCount_;
    if (n <= 0 || endAbs == 0) return 0;
    if ((uint64_t) n > endAbs)              n = (int) endAbs;
    if (n > kRingSize)                      n = kRingSize;

    // The ring only still holds the newest kRingSize samples.
    if (histCount_ - (endAbs - (uint64_t) n) > (uint64_t) kRingSize)
        return 0;

    dest.resize ((std::size_t) n);
    const uint64_t start = endAbs - (uint64_t) n;
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
KeyEngine::PassResult KeyEngine::analyseWindow (int n, bool hpssOn, uint64_t endAbs)
{
    PassResult r;

    const int got = copyEnding (work_, endAbs, n);
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

    // Backtrack; the answer is the key that carried the most EVIDENCE along
    // the smoothed path — the sum of each state's own correlations over the
    // segments the path assigned to it. NOT the modal (most-occupied) state:
    // on a window whose first half is percussion-tilted garbage, the optimal
    // path can idle in a wrong key for 8 weak segments before switching to
    // the right one for 7 strong ones, and a segment head-count then crowns
    // the wrong key 8-to-7 while the right one holds twice the correlation.
    // Weighting occupancy by the correlations makes garbage segments count
    // for what they are worth.
    std::array<float, 24> pathScore {};
    {
        int k = 0;
        float best = dp[(std::size_t) (nSeg - 1)][0];
        for (int j = 1; j < 24; ++j)
            if (dp[(std::size_t) (nSeg - 1)][(std::size_t) j] > best)
            { best = dp[(std::size_t) (nSeg - 1)][(std::size_t) j]; k = j; }

        for (int s = nSeg - 1; s >= 0; --s)
        {
            pathScore[(std::size_t) k] += std::max (0.0f, segCorr[(std::size_t) s][(std::size_t) k]);
            k = bp[(std::size_t) s][(std::size_t) k];
        }
    }
    int winner = 0;
    for (int k = 1; k < 24; ++k)
        if (pathScore[(std::size_t) k] > pathScore[(std::size_t) winner]) winner = k;


    // ---- confidence: S * A * R * T (see the calibration note up top) -------
    const float s1  = (float) agg[(std::size_t) winner];
    const int   rel = relativeKeyOf (winner);

    float bestNonRel = -2.0f, bestRel = (float) agg[(std::size_t) rel];
    for (int k = 0; k < 24; ++k)
        if (k != winner && k != rel && (float) agg[(std::size_t) k] > bestNonRel)
            bestNonRel = (float) agg[(std::size_t) k];

    const float mNonRel = s1 - bestNonRel;
    const float mRel    = s1 - bestRel;

    const float S = clamp01 ((s1 - kStrengthLo) / kStrengthSpan);
    const float A = mNonRel > 0.0f ? mNonRel / (mNonRel + kMarginKnee) : 0.0f;
    const float R = kRelFloor + (1.0f - kRelFloor)
                      * (mRel > 0.0f ? mRel / (mRel + kMarginKnee) : 0.0f);

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
    r.conf   = S * A * R * tonalityOf (chromaAvg);
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
// The triggers deliberately touch NO consumer-side counter: they used to call
// drainRing()/histCount_ surgery straight from the message thread while the
// worker could be mid-update() — a data race — and clearAccumulation() zeroed
// histCount_ while leaving the high-water marks (armedAt_, lastLiveAt_,
// lastContinuousAt_) at their old values, which froze the live chroma until
// the counter had regrown past them: the device read as DEAD for as long as
// the previous source had played, then "recovered on its own". Requests are
// now applied atomically at the top of update(), on the one thread that owns
// the counters, and the counters only ever reset as a family.
void KeyEngine::startAnalysis()
{
    armRingPos_.store (ringWrite_.load (std::memory_order_acquire));
    armRequest_.store (true);
    collecting_.store (true);          // visible NOW — the UI says "listening"
    waitingForSignal_.store (true);    // honest until real audio is heard
    lastPassEmpty_.store (false);
}

void KeyEngine::cancelAnalysis()
{
    armRequest_.store (false);
    collecting_.store (false);
    waitingForSignal_.store (false);
}

float KeyEngine::collectionProgress() const
{
    if (! collecting_.load()) return 0.0f;
    if (armRequest_.load())   return 0.0f;   // armed, not yet applied: nothing gathered
    const double winN = (double) windowS_.load() * fsA_;
    if (winN <= 0.0) return 0.0f;
    return clamp01 ((float) ((double) (histCount_ - armedAt_) / winN));
}

void KeyEngine::clearAccumulation()
{
    // The reading clears IMMEDIATELY — a key from the previous source shown
    // as current is worse than no key. The counter surgery is deferred to
    // update() (consumer thread).
    armRequest_.store (false);
    collecting_.store (false);
    waitingForSignal_.store (false);
    lastPassEmpty_.store (false);
    clearRequest_.store (true);

    std::lock_guard<std::mutex> lock (readingMutex_);
    reading_ = KeyReading {};
    liveChroma_.fill (0.0f);
    haveLiveChroma_ = false;
}

KeyActivity KeyEngine::getActivity() const
{
    KeyActivity a;
    a.collecting       = collecting_.load();
    a.waitingForSignal = a.collecting && waitingForSignal_.load();
    a.windowSeconds    = windowS_.load();
    a.progress         = collectionProgress();
    a.gatheredSeconds  = a.progress * a.windowSeconds;
    a.lastPassEmpty    = lastPassEmpty_.load();
    a.passesCompleted  = passesCompleted_.load();
    return a;
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
    // ---- apply pending triggers first (this thread owns the counters) ------
    if (clearRequest_.exchange (false))
    {
        ringRead_ = ringWrite_.load (std::memory_order_acquire);
        resetCounters();
    }

    drainRing();

    if (armRequest_.exchange (false))
    {
        // Map the captured tap position into history coordinates (they share
        // the decimated-sample clock; a clear offsets them), so the window
        // starts at the sample the trigger arrived on.
        const uint64_t off = ringRead_ - histCount_;
        const uint64_t pos = armRingPos_.load();
        armedAt_    = pos > off ? std::min (pos - off, histCount_) : 0;
        signalSeen_ = false;
    }

    bool changed = false;
    const double winN = (double) windowS_.load() * fsA_;

    if (collecting_.load())
    {
        // The window counts PLAYBACK, not wall clock: until real signal has
        // been heard, the start keeps sliding forward so a stopped transport
        // or a silent channel reads as "waiting for signal" instead of a
        // pass that quietly fills its window with nothing and reports
        // nothing. Once signal starts, gaps inside the passage count — they
        // are part of the music.
        if (! signalSeen_)
        {
            const uint64_t gathered = histCount_ - armedAt_;
            if (gathered > 0)
            {
                const int n = (int) std::min (gathered, (uint64_t) (0.5 * fsA_));
                double sumSq = 0.0;
                if (copyRecent (work_, n) == n)
                    for (float v : work_) sumSq += (double) v * (double) v;
                if (n > 0 && std::sqrt (sumSq / (double) n) >= kSilenceRms)
                    signalSeen_ = true;
                else
                    armedAt_ = histCount_;          // silence: window not started
            }
            waitingForSignal_.store (! signalSeen_);
        }

        if (signalSeen_ && (double) (histCount_ - armedAt_) >= winN)
        {
            // EXACTLY the promised window: [armedAt, armedAt + window_s] —
            // not "the newest window_s at whatever moment this thread ran".
            const int n = (int) winN;
            const auto r = analyseWindow (n, hpss_.load(), armedAt_ + (uint64_t) n);
            collecting_.store (false);
            waitingForSignal_.store (false);
            passesCompleted_.fetch_add (1);
            lastPassEmpty_.store (! r.ok);

            // The committed result is the fresh baseline: continuous mode
            // (if on) waits for genuinely NEW audio before refreshing,
            // instead of instantly re-running on the same passage and
            // relabelling the reading "continuous".
            lastContinuousAt_ = histCount_;

            // An empty pass (nothing tonal in the window) KEEPS the previous
            // reading and raises the flag instead — wiping a good reading
            // with silence would punish the user for a mis-timed ANALYSE.
            if (r.ok && ! hold_.load())
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
            const auto r = analyseWindow (n, hpss_.load(), histCount_);

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
    if (liveChromaOn_.load())
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

// ---------------------------------------------------------------------------
// offline (captures) — see the header. The whole committed machinery is
// reused per window: arm, push, update. Nothing here analyses; it only
// schedules the same pass the ANALYSE button runs.
// ---------------------------------------------------------------------------
KeyReading KeyEngine::analyseBufferOffline (const float* l, const float* r, int n)
{
    KeyReading best;
    if (l == nullptr || n <= 0 || fs_ <= 0.0) return best;

    const double totalS = (double) n / fs_;
    const float  winS   = clampf ((float) totalS - 0.5f, kMinWindowS, kMaxWindowS);
    const int    winN   = (int) ((double) winS * fs_);
    if (winN <= 0) return best;

    // A window START must leave more than winN raw samples of material after
    // it: decimation rounding shaves a few samples off the pushed count, and
    // a window fed EXACTLY its own length can end one decimated sample short
    // and never complete. The margin also absorbs a quiet first beat.
    const int seg = std::min (n, winN + (int) (0.4 * fs_));

    // Up to three disjoint-start windows: start, middle, end. Shorter
    // buffers collapse to fewer (under two windows long: start + end; one
    // window long: a single pass).
    int starts[3];
    int nWin = 0;
    if (n <= seg + (int) (0.1 * fs_))
        starts[nWin++] = n - seg;
    else if (n <= 2 * seg)
    {
        starts[nWin++] = 0;
        starts[nWin++] = n - seg;
    }
    else
    {
        starts[nWin++] = 0;
        starts[nWin++] = (n - seg) / 2;
        starts[nWin++] = n - seg;
    }

    const float prevWindow     = windowS_.load();
    const bool  prevContinuous = continuous_.load();
    const bool  prevLive       = liveChromaOn_.load();
    setWindowSeconds (winS);
    setContinuous (false);
    setLiveChromaEnabled (false);      // nobody is watching an offline pass

    for (int w = 0; w < nWin; ++w)
    {
        reset();                       // fresh tap + counters per window
        startAnalysis();
        // Feed in modest chunks so one call never outruns the ring; the
        // consumer side drains inside update(). Feeding continues to the END
        // of the buffer, not just one window: leading silence slides the
        // committed window's start (playback semantics), so the material
        // that fills it may sit later than starts[w].
        const int chunk = 1 << 14;
        int off = starts[w];
        while (off < n && isCollecting())
        {
            const int m = std::min (chunk, n - off);
            pushBlock (l + off, r != nullptr ? r + off : nullptr, m);
            update();
            off += m;
        }
        update();                      // completes the pass at the window edge

        const KeyReading kr = getReading();
        if (kr.valid && (! best.valid || kr.confidence > best.confidence))
            best = kr;
    }

    setWindowSeconds (prevWindow);
    setContinuous (prevContinuous);
    setLiveChromaEnabled (prevLive);
    return best;
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
