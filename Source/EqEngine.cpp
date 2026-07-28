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
        default: c.passthrough = true; break;
    }

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
}

void EqEngine::reset()
{
    for (auto& b : bands_)
        for (auto& st : b.state)
            for (auto& ch : st)
                ch = StageState {};
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
}

void EqEngine::setBand (int index, const BandSpec& spec)
{
    if (index < 0 || index >= kMaxBands) return;
    const uint32_t s = seq_.load (std::memory_order_relaxed);
    seq_.store (s + 1, std::memory_order_release);
    targets_[index] = spec;
    seq_.store (s + 2, std::memory_order_release);
    dirtyPulse_.fetch_add (1, std::memory_order_release);
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
            continue;
        }

        const bool isPassFilter = (t.type == BandType::HighPass || t.type == BandType::LowPass);
        b.numStages = isPassFilter ? stagesForSlope (t.slopeDbPerOct) : 1;

        for (int stg = 0; stg < b.numStages; ++stg)
        {
            float qForStage = b.curQ;
            if (isPassFilter)
            {
                // Butterworth Q-staggering across cascaded 2nd-order sections
                const int order = 2 * b.numStages;
                qForStage = (float) (1.0 / (2.0 * std::cos (kPi * (2.0 * stg + 1.0)
                                                            / (2.0 * order))));
            }
            b.coeffs[stg] = computeCoeffs (b.curType, sampleRate_, b.curFreq,
                                           b.curGain, qForStage);
        }
    }

    // --- run the cascade, channel by channel -------------------------------
    for (int ch = 0; ch < nCh; ++ch)
    {
        float* x = channels[ch];
        for (int i = 0; i < kMaxBands; ++i)
        {
            BandRuntime& b = bands_[i];
            if (b.numStages == 0) continue;
            for (int stg = 0; stg < b.numStages; ++stg)
            {
                Coeffs&     c = b.coeffs[stg];
                StageState& s = b.state[stg][ch];
                for (int n = 0; n < numSamples; ++n)
                    x[n] = processStage (c, s, x[n]);
            }
        }
    }
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

            const bool isPass = (t.type == BandType::HighPass || t.type == BandType::LowPass);
            const int stages  = isPass ? stagesForSlope (t.slopeDbPerOct) : 1;
            for (int stg = 0; stg < stages; ++stg)
            {
                float qForStage = t.q;
                if (isPass)
                {
                    const int order = 2 * stages;
                    qForStage = (float) (1.0 / (2.0 * std::cos (kPi * (2.0 * stg + 1.0)
                                                                / (2.0 * order))));
                }
                const Coeffs c = computeCoeffs (t.type, sampleRate_, t.freqHz,
                                                t.gainDb, qForStage);
                h *= stageResponse (c, omega);
            }
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
        std::complex<double> h { 1.0, 0.0 };

        const bool isPass = (t.type == BandType::HighPass || t.type == BandType::LowPass);
        const int stages  = isPass ? stagesForSlope (t.slopeDbPerOct) : 1;
        for (int stg = 0; stg < stages; ++stg)
        {
            float qForStage = t.q;
            if (isPass)
            {
                const int order = 2 * stages;
                qForStage = (float) (1.0 / (2.0 * std::cos (kPi * (2.0 * stg + 1.0)
                                                            / (2.0 * order))));
            }
            const Coeffs c = computeCoeffs (t.type, sampleRate_, t.freqHz,
                                            t.gainDb, qForStage);
            h *= stageResponse (c, omega);
        }
        magsDb[i] = (float) (20.0 * std::log10 (std::max (std::abs (h), 1.0e-9)));
    }
}

} // namespace echojay
