/*
    EqEngine.cpp  —  see EqEngine.h for the design rationale.
*/

#include "EqEngine.h"

#include <algorithm>

namespace echojay
{

static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// coefficient construction (Cytomic TPT-SVF "cheat sheet" mix forms)
// ---------------------------------------------------------------------------
EqEngine::Coeffs EqEngine::computeCoeffs (BandType type, double fs, float freqHz,
                                          float gainDb, float q) noexcept
{
    Coeffs c;
    c.passthrough = false;

    // clamp to a sane, stable range
    const double nyq   = fs * 0.5;
    const double f     = std::min (std::max ((double) freqHz, 5.0), nyq * 0.999);
    const double qq    = std::min (std::max ((double) q, 0.025), 100.0);
    const double gtan  = std::tan (kPi * f / fs);
    const double A     = std::pow (10.0, (double) gainDb / 40.0);

    switch (type)
    {
        case BandType::Bell:
        {
            c.g = (float) gtan;
            c.k = (float) (1.0 / (qq * A));
            c.m0 = 1.0f;
            c.m1 = (float) (c.k * (A * A - 1.0));
            c.m2 = 0.0f;
            break;
        }
        case BandType::LowShelf:
        {
            c.g = (float) (gtan / std::sqrt (A));
            c.k = (float) (1.0 / qq);
            c.m0 = 1.0f;
            c.m1 = (float) (c.k * (A - 1.0));
            c.m2 = (float) (A * A - 1.0);
            break;
        }
        case BandType::HighShelf:
        {
            c.g = (float) (gtan * std::sqrt (A));
            c.k = (float) (1.0 / qq);
            c.m0 = (float) (A * A);
            c.m1 = (float) (c.k * (1.0 - A) * A);
            c.m2 = (float) (1.0 - A * A);
            break;
        }
        case BandType::Notch:
        {
            c.g = (float) gtan;
            c.k = (float) (1.0 / qq);
            c.m0 = 1.0f;
            c.m1 = -c.k;
            c.m2 = 0.0f;
            break;
        }
        case BandType::HighPass:
        {
            c.g = (float) gtan;
            c.k = (float) (1.0 / qq);
            c.m0 = 1.0f;
            c.m1 = -c.k;
            c.m2 = -1.0f;
            break;
        }
        case BandType::LowPass:
        {
            c.g = (float) gtan;
            c.k = (float) (1.0 / qq);
            c.m0 = 0.0f;
            c.m1 = 0.0f;
            c.m2 = 1.0f;
            break;
        }
        case BandType::NumTypes:
        default: c.passthrough = true; break;
    }

    const double a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
    c.a1 = (float) a1;
    c.a2 = (float) (c.g * a1);
    c.a3 = (float) (c.g * c.a2);
    return c;
}

// Fast bell rebuild when only the gain changes (g == tan(pi f/fs) precomputed).
// Used per-sample by dynamic bands so we avoid a tan() in the audio inner loop.
EqEngine::Coeffs EqEngine::bellCoeffsFromG (float g, float gainDb, float q) noexcept
{
    Coeffs c;
    c.passthrough = false;
    const double A  = std::pow (10.0, (double) gainDb / 40.0);
    const double qq = std::min (std::max ((double) q, 0.025), 100.0);
    c.g  = g;
    c.k  = (float) (1.0 / (qq * A));
    c.m0 = 1.0f;
    c.m1 = (float) (c.k * (A * A - 1.0));
    c.m2 = 0.0f;
    const double a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
    c.a1 = (float) a1;
    c.a2 = (float) (c.g * a1);
    c.a3 = (float) (c.g * c.a2);
    return c;
}

// Pure bandpass (constant-Q, unity peak) for the dynamic-band level detector.
EqEngine::Coeffs EqEngine::bandpassCoeffs (double fs, float freqHz, float q) noexcept
{
    Coeffs c;
    c.passthrough = false;
    const double nyq = fs * 0.5;
    const double f   = std::min (std::max ((double) freqHz, 5.0), nyq * 0.999);
    const double qq  = std::min (std::max ((double) q, 0.1), 100.0);
    c.g  = (float) std::tan (kPi * f / fs);
    c.k  = (float) (1.0 / qq);
    c.m0 = 0.0f; c.m1 = 1.0f; c.m2 = 0.0f;             // bandpass = v1 output
    const double a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
    c.a1 = (float) a1;
    c.a2 = (float) (c.g * a1);
    c.a3 = (float) (c.g * c.a2);
    return c;
}

int EqEngine::stagesForSlope (int slopeDbPerOct) noexcept
{
    int n = (slopeDbPerOct + 6) / 12;      // 12->1, 24->2, ... ; 6 rounds to 1
    return std::min (std::max (n, 1), 8);
}

// -- one description of a band's cascade, used everywhere ------------------
// The audio path, the UI curve and the auto-gain integral all have to agree on
// how many sections a band expands to and what Q each one runs at. Having
// three copies of that rule is how a curve silently stops matching the audio.
int EqEngine::stagesForBand (const BandSpec& s) noexcept
{
    const bool isPass = (s.type == BandType::HighPass || s.type == BandType::LowPass);
    return isPass ? stagesForSlope (s.slopeDbPerOct) : 1;
}

float EqEngine::qForStage (const BandSpec& s, int stage, int numStages) noexcept
{
    const bool isPass = (s.type == BandType::HighPass || s.type == BandType::LowPass);
    if (! isPass) return s.q;

    // Butterworth Q-staggering across cascaded 2nd-order sections
    const int order = 2 * numStages;
    return (float) (1.0 / (2.0 * std::cos (kPi * (2.0 * stage + 1.0) / (2.0 * order))));
}

std::complex<double> EqEngine::bandResponse (const BandSpec& s, double fs,
                                             double omega) noexcept
{
    std::complex<double> h { 1.0, 0.0 };
    const int stages = stagesForBand (s);
    for (int stg = 0; stg < stages; ++stg)
    {
        const Coeffs c = computeCoeffs (s.type, fs, s.freqHz, s.gainDb,
                                        qForStage (s, stg, stages));
        h *= stageResponse (c, omega);
    }
    return h;
}

float EqEngine::processStage (Coeffs& c, StageState& s, float x) noexcept
{
    const float v3 = x - s.ic2;
    const float v1 = c.a1 * s.ic1 + c.a2 * v3;
    const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    return c.m0 * x + c.m1 * v1 + c.m2 * v2;
}

// exact discrete response of one SVF stage at digital angular frequency omega
std::complex<double> EqEngine::stageResponse (const Coeffs& c, double omega) noexcept
{
    if (c.passthrough)
        return { 1.0, 0.0 };

    const std::complex<double> z = std::polar (1.0, omega);   // e^{jω}
    const double a1 = c.a1, a2 = c.a2, a3 = c.a3;

    // state-space: A=[[2a1-1,-2a2],[2a2,1-2a3]], B=[2a2,2a3],
    // C=[m1*a1+m2*a2, -m1*a2+m2*(1-a3)], D=m0+m1*a2+m2*a3, H=C(zI-A)^-1 B + D
    const std::complex<double> m11 = z - (2.0 * a1 - 1.0);
    const std::complex<double> m12 = 2.0 * a2;
    const std::complex<double> m21 = -2.0 * a2;
    const std::complex<double> m22 = z - (1.0 - 2.0 * a3);
    const std::complex<double> det = m11 * m22 - m12 * m21;

    const double B0 = 2.0 * a2, B1 = 2.0 * a3;
    const std::complex<double> x0 = ( m22 * B0 - m12 * B1) / det;
    const std::complex<double> x1 = (-m21 * B0 + m11 * B1) / det;

    const double C0 = c.m1 * a1 + c.m2 * a2;
    const double C1 = -c.m1 * a2 + c.m2 * (1.0 - a3);
    const double D  = c.m0 + c.m1 * a2 + c.m2 * a3;

    return C0 * x0 + C1 * x1 + D;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
void EqEngine::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    maxBlock_   = std::max (1, maxBlockSize);
    numCh_      = std::min (std::max (1, numChannels), kMaxChannels);

    // per-block smoothing time constant ~15 ms, computed against the max block
    const double blockSec = (double) maxBlock_ / sampleRate_;
    const double tau      = 0.015;
    smoothingCoeff_       = (float) (1.0 - std::exp (-blockSec / tau));

    reset();

    // snap runtime to current targets so we don't sweep from stale state
    BandSpec snap[kMaxBands];
    snapshotForAnalysis (snap);
    for (int i = 0; i < kMaxBands; ++i)
    {
        bands_[i].curFreq    = snap[i].freqHz;
        bands_[i].curGain    = snap[i].gainDb;
        bands_[i].curQ       = snap[i].q;
        bands_[i].curEnabled = snap[i].enabled;
        bands_[i].curType    = snap[i].type;
        bands_[i].curSlope   = snap[i].slopeDbPerOct;
    }
    lastSeenPulse_ = 0xffffffff; // force a pull on first process

    // The makeup depends on the sample rate (the response is evaluated at
    // digital frequencies), so a rate change has to re-integrate the curve.
    recomputeAutoGain();
    gainPrimed_ = false;         // snap the output stage rather than ramping into it
}

void EqEngine::reset()
{
    for (auto& b : bands_)
    {
        for (auto& st : b.state)
            for (auto& ch : st)
                ch = StageState {};
        for (auto& ch : b.detState)
            ch = StageState {};
        b.env = 0.0f;
        b.dynGainDb = 0.0f;
    }
    gainPrimed_ = false;         // no ramp out of a cleared state
}

// ---------------------------------------------------------------------------
// parameter publish (message thread) — seqlock writer
// ---------------------------------------------------------------------------
void EqEngine::setBands (const BandSpec* specs, int count)
{
    count = std::min (count, kMaxBands);
    const uint32_t s = seq_.load (std::memory_order_relaxed);
    seq_.store (s + 1, std::memory_order_release);      // enter write (odd)
    for (int i = 0; i < count; ++i)
        targets_[i] = specs[i];
    seq_.store (s + 2, std::memory_order_release);      // leave write (even)
    dirtyPulse_.fetch_add (1, std::memory_order_release);
    recomputeAutoGain();
}

void EqEngine::setBand (int index, const BandSpec& spec)
{
    if (index < 0 || index >= kMaxBands) return;
    const uint32_t s = seq_.load (std::memory_order_relaxed);
    seq_.store (s + 1, std::memory_order_release);
    targets_[index] = spec;
    seq_.store (s + 2, std::memory_order_release);
    dirtyPulse_.fetch_add (1, std::memory_order_release);
    recomputeAutoGain();
}

void EqEngine::setOutputDb (float db) noexcept
{
    if (! (db == db)) db = 0.0f;                        // NaN in, unity out
    outputDb_.store (std::min (std::max (db, -24.0f), 24.0f), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// auto-gain: the pink-weighted loudness delta of the enabled static bands
// ---------------------------------------------------------------------------
// Pink noise carries equal energy per octave, so on a LOG-spaced frequency
// grid every point weighs the same and the mean of the linear power response
// over that grid *is* the RMS change pink noise sees through the EQ. Its
// inverse is the makeup. That makes this a real loudness estimate rather than
// the "sum the band gains" hand-wave, and it costs one curve integral per
// parameter publish — on the message thread, never on the audio thread.
//
// Dynamic bands are deliberately excluded: their contribution is program
// dependent and momentary, so folding it into a static makeup would make the
// output level breathe with the detector.
void EqEngine::recomputeAutoGain()
{
    BandSpec snap[kMaxBands];
    snapshotForAnalysis (snap);

    constexpr int kGridPoints = 121;                    // 1/12 octave, 20 Hz..20 kHz
    const double fLo = 20.0;
    const double fHi = std::min (20000.0, sampleRate_ * 0.45);
    if (fHi <= fLo)
    {
        autoGainTargetDb_.store (0.0f, std::memory_order_relaxed);
        return;
    }

    bool any = false;
    for (const auto& s : snap)
        if (s.enabled && ! (s.dynamic && s.type == BandType::Bell)) { any = true; break; }

    if (! any)
    {
        autoGainTargetDb_.store (0.0f, std::memory_order_relaxed);
        return;
    }

    const double ratio = fHi / fLo;
    double sumPow = 0.0;

    for (int i = 0; i < kGridPoints; ++i)
    {
        const double t = (double) i / (double) (kGridPoints - 1);
        const double f = fLo * std::pow (ratio, t);
        const double omega = 2.0 * kPi * f / sampleRate_;

        std::complex<double> h { 1.0, 0.0 };
        for (const auto& s : snap)
        {
            if (! s.enabled) continue;
            if (s.dynamic && s.type == BandType::Bell) continue;
            h *= bandResponse (s, sampleRate_, omega);
        }
        sumPow += std::norm (h);                        // |H|^2
    }

    const double meanPow = sumPow / (double) kGridPoints;
    const double deltaDb = 10.0 * std::log10 (std::max (meanPow, 1.0e-12));

    // Clamp the makeup to the same ±24 the output trim uses: a brutal
    // high-pass removes most of the pink spectrum, and "compensating" that
    // with +30 dB of boost is a blown speaker, not a level match.
    autoGainTargetDb_.store ((float) std::min (std::max (-deltaDb, -24.0), 24.0),
                             std::memory_order_relaxed);
}

BandSpec EqEngine::getBand (int index) const
{
    BandSpec out;
    if (index < 0 || index >= kMaxBands) return out;
    BandSpec snap[kMaxBands];
    snapshotForAnalysis (snap);
    return snap[index];
}

// seqlock reader (bounded) used by both audio + analysis
void EqEngine::snapshotForAnalysis (BandSpec out[kMaxBands]) const
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const uint32_t s1 = seq_.load (std::memory_order_acquire);
        if (s1 & 1u) continue;                          // writer mid-update
        std::memcpy (out, targets_, sizeof (BandSpec) * kMaxBands);
        const uint32_t s2 = seq_.load (std::memory_order_acquire);
        if (s1 == s2) return;                           // consistent read
    }
    // extremely unlikely: fall back to a direct copy (writer will publish again)
    std::memcpy (out, targets_, sizeof (BandSpec) * kMaxBands);
}

void EqEngine::pullTargetsIfChanged() noexcept
{
    const uint32_t pulse = dirtyPulse_.load (std::memory_order_acquire);
    if (pulse == lastSeenPulse_) return;                // nothing new
    snapshotForAnalysis (activeTargets_);
    lastSeenPulse_ = pulse;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EqEngine::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    if (bypassed_.load (std::memory_order_relaxed))
        return;

    pullTargetsIfChanged();

    const int solo   = soloBand_.load (std::memory_order_relaxed);
    const int nCh    = std::min (numChannels, kMaxChannels);

    // recompute per-block smoothing alpha against the actual block length
    const double blockSec = (double) numSamples / sampleRate_;
    const float  alpha    = (float) (1.0 - std::exp (-blockSec / 0.015));

    // --- update each band's runtime coefficients (block rate) --------------
    for (int i = 0; i < kMaxBands; ++i)
    {
        BandRuntime&    b = bands_[i];
        const BandSpec& t = activeTargets_[i];

        const bool active = (solo >= 0) ? (i == solo) : t.enabled;

        // a type / slope / enable transition can't be smoothed → snap + clear
        const bool structuralChange = (t.type != b.curType)
                                    || (t.slopeDbPerOct != b.curSlope)
                                    || (active && ! b.curEnabled);
        if (structuralChange)
        {
            b.curFreq = t.freqHz; b.curGain = t.gainDb; b.curQ = t.q;
            b.curType = t.type;   b.curSlope = t.slopeDbPerOct;
            for (auto& st : b.state) for (auto& ch : st) ch = StageState {};
        }
        else
        {
            // smooth freq in log domain, gain in dB, q linear
            const float lf = std::log (std::max (b.curFreq, 1.0f));
            const float lt = std::log (std::max (t.freqHz, 1.0f));
            b.curFreq = std::exp (lf + (lt - lf) * alpha);
            b.curGain += (t.gainDb - b.curGain) * alpha;
            b.curQ    += (t.q      - b.curQ)    * alpha;
        }
        b.curEnabled = active;

        if (! active)
        {
            b.numStages = 0;
            b.isDynamic = false;
            continue;
        }

        // Dynamic action is supported for Bell bands only (the surgical
        // de-ess / resonance-taming case). Other types ignore the flag.
        b.isDynamic = (t.dynamic && t.type == BandType::Bell);

        if (b.isDynamic)
        {
            b.numStages   = 1;
            b.gBell       = (float) std::tan (kPi
                              * std::min ((double) b.curFreq, sampleRate_ * 0.4999)
                              / sampleRate_);
            b.detCoeffs   = bandpassCoeffs (sampleRate_, b.curFreq, std::max (b.curQ, 0.5f));
            b.thresholdDb = t.thresholdDb;
            b.rangeDb     = t.rangeDb;
            const double atkSec = std::max ((double) t.attackMs,  0.05) * 0.001;
            const double relSec = std::max ((double) t.releaseMs, 0.05) * 0.001;
            b.atkCoeff = (float) (1.0 - std::exp (-1.0 / (atkSec * sampleRate_)));
            b.relCoeff = (float) (1.0 - std::exp (-1.0 / (relSec * sampleRate_)));
            continue;   // per-sample coeffs are built in the cascade
        }

        b.numStages = stagesForBand (t);

        // Q staggering is a property of the cascade, not of the smoothed value,
        // so it is asked for by shape (from t) and applied to the smoothed
        // params (curFreq/curGain/curQ) that this block actually realises.
        BandSpec shape = t;
        shape.q = b.curQ;
        for (int stg = 0; stg < b.numStages; ++stg)
            b.coeffs[stg] = computeCoeffs (b.curType, sampleRate_, b.curFreq, b.curGain,
                                           qForStage (shape, stg, b.numStages));
    }

    // --- run the cascade, band by band on the running multichannel buffer ---
    // Static bands filter each channel a block at a time. Dynamic bands run
    // per-sample with a stereo-linked detector so both channels share one gain.
    for (int i = 0; i < kMaxBands; ++i)
    {
        BandRuntime& b = bands_[i];
        if (b.numStages == 0) continue;

        if (! b.isDynamic)
        {
            for (int stg = 0; stg < b.numStages; ++stg)
            {
                Coeffs& c = b.coeffs[stg];
                for (int ch = 0; ch < nCh; ++ch)
                {
                    StageState& s = b.state[stg][ch];
                    float* x = channels[ch];
                    for (int n = 0; n < numSamples; ++n)
                        x[n] = processStage (c, s, x[n]);
                }
            }
            continue;
        }

        // -- dynamic Bell: detect → envelope → gain → per-sample rebuild -----
        const float thr   = b.thresholdDb;
        const float rng    = b.rangeDb;
        const float rngAbs = std::fabs (rng);
        const float rngSgn = (rng < 0.0f) ? -1.0f : 1.0f;

        for (int n = 0; n < numSamples; ++n)
        {
            // stereo-linked detector: peak of the band-passed signal across ch
            float lvl = 0.0f;
            for (int ch = 0; ch < nCh; ++ch)
            {
                const float d = processStage (b.detCoeffs, b.detState[ch], channels[ch][n]);
                lvl = std::max (lvl, std::fabs (d));
            }
            // envelope follower (attack when rising, release when falling)
            const float coeff = (lvl > b.env) ? b.atkCoeff : b.relCoeff;
            b.env += (lvl - b.env) * coeff;

            const float envDb     = 20.0f * std::log10 (b.env + 1.0e-9f);
            const float overshoot = envDb - thr;
            b.dynGainDb = (overshoot > 0.0f)
                        ? rngSgn * std::min (overshoot, rngAbs)
                        : 0.0f;

            const Coeffs c = bellCoeffsFromG (b.gBell, b.curGain + b.dynGainDb, b.curQ);
            for (int ch = 0; ch < nCh; ++ch)
            {
                StageState& s = b.state[0][ch];
                channels[ch][n] = processStage (const_cast<Coeffs&> (c), s, channels[ch][n]);
            }
        }
    }

    // --- final gain stage: auto-gain makeup, then the output trim ----------
    // Both are smoothed in dB at block rate (same 15 ms constant as the band
    // params) and then ramped LINEARLY across the block, so even a hard jump
    // from an AI apply arrives as a ramp rather than a step.
    const float agTarget  = autoGain_.load (std::memory_order_relaxed)
                          ? autoGainTargetDb_.load (std::memory_order_relaxed) : 0.0f;
    const float outTarget = outputDb_.load (std::memory_order_relaxed);

    if (! gainPrimed_)
    {
        curAutoGainDb_ = agTarget;
        curOutputDb_   = outTarget;
        lastGainLin_   = (float) std::pow (10.0, (curAutoGainDb_ + curOutputDb_) / 20.0);
        gainPrimed_    = true;
    }
    else
    {
        curAutoGainDb_ += (agTarget  - curAutoGainDb_) * alpha;
        curOutputDb_   += (outTarget - curOutputDb_)   * alpha;
    }

    const float targetLin = (float) std::pow (10.0, (curAutoGainDb_ + curOutputDb_) / 20.0);

    // Unity in and unity out: skip the whole pass rather than multiplying every
    // sample by 1.0f, which is the overwhelmingly common case. Compared with a
    // tolerance rather than ==, so this stays honest under -Wfloat-equal and
    // does not care whether pow() returned 1.0f to the last bit.
    constexpr float kUnityEps = 1.0e-7f;
    if (std::fabs (targetLin - 1.0f) > kUnityEps || std::fabs (lastGainLin_ - 1.0f) > kUnityEps)
    {
        const float step = (targetLin - lastGainLin_) / (float) std::max (numSamples, 1);
        for (int ch = 0; ch < nCh; ++ch)
        {
            float* x = channels[ch];
            float  g = lastGainLin_;
            for (int n = 0; n < numSamples; ++n) { g += step; x[n] *= g; }
        }
    }
    lastGainLin_ = targetLin;
}

// ---------------------------------------------------------------------------
// analysis
// ---------------------------------------------------------------------------
void EqEngine::getMagnitudeResponse (const float* freqsHz, float* magsDb, int n) const
{
    snapshotForAnalysis (analysisScratch_);

    for (int i = 0; i < n; ++i)
    {
        const double omega = 2.0 * kPi * (double) freqsHz[i] / sampleRate_;
        std::complex<double> h { 1.0, 0.0 };

        for (int bi = 0; bi < kMaxBands; ++bi)
        {
            const BandSpec& t = analysisScratch_[bi];
            if (! t.enabled) continue;
            h *= bandResponse (t, sampleRate_, omega);
        }

        const double mag = std::abs (h);
        magsDb[i] = (float) (20.0 * std::log10 (std::max (mag, 1.0e-9)));
    }
}

void EqEngine::getBandMagnitudeResponse (int index, const float* freqsHz,
                                         float* magsDb, int n) const
{
    BandSpec snap[kMaxBands];
    snapshotForAnalysis (snap);

    for (int i = 0; i < n; ++i)
    {
        if (index < 0 || index >= kMaxBands || ! snap[index].enabled)
        {
            magsDb[i] = 0.0f;
            continue;
        }
        const BandSpec& t = snap[index];
        const double omega = 2.0 * kPi * (double) freqsHz[i] / sampleRate_;
        const std::complex<double> h = bandResponse (t, sampleRate_, omega);
        magsDb[i] = (float) (20.0 * std::log10 (std::max (std::abs (h), 1.0e-9)));
    }
}

} // namespace echojay
