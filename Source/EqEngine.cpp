/*
    EqEngine.cpp  —  see EqEngine.h for the design rationale.
*/

#include "EqEngine.h"
#include "EqFft.h"

#include <algorithm>

namespace echojay
{

static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// per-band routing (P2)
// ---------------------------------------------------------------------------
// Which buffer lanes a band filters, and whether those lanes live in the M/S
// domain. The process loop converts the running buffer between L/R and M/S
// only when adjacent bands disagree about the domain, so an all-Stereo band
// set (the default) never converts at all and stays bit-identical to the
// pre-P2 path.
//
// Mono input: lane 0 IS the signal (its own mid), so Stereo / Left / Mid all
// process it; Side and Right have no lane to land on and are clean no-ops.
static int lanesForBand (BandChannel bc, int nCh, int lanes[2], bool& wantsMs) noexcept
{
    wantsMs = false;
    if (nCh < 2)
    {
        if (bc == BandChannel::Side || bc == BandChannel::Right) return 0;
        lanes[0] = 0;
        return 1;
    }
    switch (bc)
    {
        case BandChannel::Stereo: lanes[0] = 0; lanes[1] = 1; return 2;
        case BandChannel::Left:   lanes[0] = 0; return 1;
        case BandChannel::Right:  lanes[0] = 1; return 1;
        case BandChannel::Mid:    wantsMs = true; lanes[0] = 0; return 1;
        case BandChannel::Side:   wantsMs = true; lanes[0] = 1; return 1;
        case BandChannel::NumChannelModes:
        default:                  lanes[0] = 0; lanes[1] = 1; return 2;
    }
}

static void lrToMs (float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float m = 0.5f * (l[i] + r[i]);
        const float s = 0.5f * (l[i] - r[i]);
        l[i] = m; r[i] = s;
    }
}

static void msToLr (float* m, float* s, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float l = m[i] + s[i];
        const float r = m[i] - s[i];
        m[i] = l; s[i] = r;
    }
}

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
        bands_[i].curChannel = snap[i].channel;
    }
    lastSeenPulse_ = 0xffffffff; // force a pull on first process

    // The makeup depends on the sample rate (the response is evaluated at
    // digital frequencies), so a rate change has to re-integrate the curve.
    recomputeAutoGain();

    // Linear mode is rate-dependent twice over: the IR is designed on this
    // rate's bin grid, and the convolver stream must restart cleanly.
    if (getPhaseMode() == PhaseMode::Linear)
    {
        ensureLinearBuffers();
        recomputeLinearPhase();
    }

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

    if (conv_.allocated.load (std::memory_order_acquire))
    {
        for (int l = 0; l < 2; ++l)
        {
            std::fill (conv_.inFifo[l].begin(),   conv_.inFifo[l].end(),   0.0f);
            std::fill (conv_.outReady[l].begin(), conv_.outReady[l].end(), 0.0f);
            std::fill (conv_.tail[l].begin(),     conv_.tail[l].end(),     0.0f);
            std::fill (conv_.xRe[l].begin(),      conv_.xRe[l].end(),      0.0f);
            std::fill (conv_.xIm[l].begin(),      conv_.xIm[l].end(),      0.0f);
        }
        conv_.fill = 0;
        conv_.fdlPos = 0;
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
    recomputeLinearPhase();
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
    recomputeLinearPhase();
}

void EqEngine::setSoloBand (int index)
{
    soloBand_.store (index, std::memory_order_relaxed);
    // In Linear mode the audible static curve IS the FIR, so soloing has to
    // re-design it to match what the SVF path would have done.
    recomputeLinearPhase();
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
        double routedPow = 1.0;
        for (const auto& s : snap)
        {
            if (! s.enabled) continue;
            if (s.dynamic && s.type == BandType::Bell) continue;

            if (s.channel == BandChannel::Stereo)
            {
                h *= bandResponse (s, sampleRate_, omega);
            }
            else
            {
                // A single-lane band only touches ~half the signal energy
                // (exactly half for uncorrelated L/R), so its power response is
                // averaged with unity rather than applied to everything —
                // otherwise a side-only shelf would claim a makeup no listener
                // would hear. Stereo bands keep the exact complex product, so
                // an all-Stereo set computes bit-identically to before.
                routedPow *= 0.5 * (std::norm (bandResponse (s, sampleRate_, omega)) + 1.0);
            }
        }
        sumPow += std::norm (h) * routedPow;            // |H|^2
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

    // Linear only when the message thread has finished allocating the
    // convolver — until then the SVF path carries on, so a mode flip can never
    // race the audio thread into unallocated buffers.
    const bool linear = (phaseMode_.load (std::memory_order_acquire)
                            == (int) PhaseMode::Linear)
                      && conv_.allocated.load (std::memory_order_acquire);

    // A mode transition swaps which engine owns the static curve; both sides'
    // runtime state is stale audio from the other mode, so it is cleared
    // rather than rung out.
    if (linear != wasLinear_)
    {
        for (auto& b : bands_)
        {
            for (auto& st : b.state) for (auto& ch : st) ch = StageState {};
            for (auto& ch : b.detState) ch = StageState {};
            b.env = 0.0f; b.dynGainDb = 0.0f;
        }
        if (conv_.allocated.load (std::memory_order_acquire))
        {
            for (int l = 0; l < 2; ++l)
            {
                std::fill (conv_.inFifo[l].begin(),  conv_.inFifo[l].end(),  0.0f);
                std::fill (conv_.outReady[l].begin(),conv_.outReady[l].end(),0.0f);
                std::fill (conv_.tail[l].begin(),    conv_.tail[l].end(),    0.0f);
                std::fill (conv_.xRe[l].begin(),     conv_.xRe[l].end(),     0.0f);
                std::fill (conv_.xIm[l].begin(),     conv_.xIm[l].end(),     0.0f);
            }
            conv_.fill = 0;
            conv_.fdlPos = 0;
        }
        wasLinear_ = linear;
    }

    // recompute per-block smoothing alpha against the actual block length
    const double blockSec = (double) numSamples / sampleRate_;
    const float  alpha    = (float) (1.0 - std::exp (-blockSec / 0.015));

    // --- update each band's runtime coefficients (block rate) --------------
    for (int i = 0; i < kMaxBands; ++i)
    {
        BandRuntime&    b = bands_[i];
        const BandSpec& t = activeTargets_[i];

        const bool active = (solo >= 0) ? (i == solo) : t.enabled;

        // a type / slope / enable / routing transition can't be smoothed →
        // snap + clear (the filter states belong to the old lanes)
        const bool structuralChange = (t.type != b.curType)
                                    || (t.slopeDbPerOct != b.curSlope)
                                    || (t.channel != b.curChannel)
                                    || (active && ! b.curEnabled);
        if (structuralChange)
        {
            b.curFreq = t.freqHz; b.curGain = t.gainDb; b.curQ = t.q;
            b.curType = t.type;   b.curSlope = t.slopeDbPerOct;
            b.curChannel = t.channel;
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

        // Linear mode: the FIR owns every static band; only dynamic bells stay
        // on the per-sample SVF path.
        if (linear) { b.numStages = 0; continue; }

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

    // --- linear mode: the static curve, as one zero-phase FIR ---------------
    // Runs BEFORE the dynamic bells (below), which then act on the linear-
    // phase-corrected signal — the hybrid every dynamic linear EQ ships.
    if (linear)
    {
        if (conv_.lanesIn != nCh)
        {
            // channel-count change: the FDL and tails describe the old stream
            for (int l = 0; l < 2; ++l)
            {
                std::fill (conv_.inFifo[l].begin(),  conv_.inFifo[l].end(),  0.0f);
                std::fill (conv_.outReady[l].begin(),conv_.outReady[l].end(),0.0f);
                std::fill (conv_.tail[l].begin(),    conv_.tail[l].end(),    0.0f);
                std::fill (conv_.xRe[l].begin(),     conv_.xRe[l].end(),     0.0f);
                std::fill (conv_.xIm[l].begin(),     conv_.xIm[l].end(),     0.0f);
            }
            conv_.fill = 0;
            conv_.fdlPos = 0;
            conv_.lanesIn = nCh;
        }

        for (int n = 0; n < numSamples; ++n)
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                const float in = channels[ch][n];
                channels[ch][n] = conv_.outReady[ch][(size_t) conv_.fill];
                conv_.inFifo[ch][(size_t) conv_.fill] = in;
            }
            if (++conv_.fill == kPartSize)
            {
                pullIrIfChanged();
                processLinearHop();
                conv_.fill = 0;
            }
        }
    }

    // --- run the cascade, band by band on the running multichannel buffer ---
    // Static bands filter each of their routed lanes a block at a time.
    // Dynamic bands run per-sample with a lane-linked detector so all their
    // lanes share one gain. The buffer flips between the L/R and M/S domains
    // only when consecutive bands disagree about it, so an all-Stereo set (the
    // default) never converts and the arithmetic is bit-identical to before
    // routing existed.
    bool msDomain = false;
    for (int i = 0; i < kMaxBands; ++i)
    {
        BandRuntime& b = bands_[i];
        if (b.numStages == 0) continue;

        int  lanes[2];
        bool wantsMs = false;
        const int nLanes = lanesForBand (b.curChannel, nCh, lanes, wantsMs);
        if (nLanes == 0) { b.dynGainDb = 0.0f; continue; }   // Side/Right on mono: no-op

        if (wantsMs != msDomain && nCh == 2)
        {
            if (wantsMs) lrToMs (channels[0], channels[1], numSamples);
            else         msToLr (channels[0], channels[1], numSamples);
            msDomain = wantsMs;
        }

        if (! b.isDynamic)
        {
            for (int stg = 0; stg < b.numStages; ++stg)
            {
                Coeffs& c = b.coeffs[stg];
                for (int li = 0; li < nLanes; ++li)
                {
                    const int ch = lanes[li];
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
            // lane-linked detector: peak of the band-passed signal across lanes
            float lvl = 0.0f;
            for (int li = 0; li < nLanes; ++li)
            {
                const int ch = lanes[li];
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
            for (int li = 0; li < nLanes; ++li)
            {
                const int ch = lanes[li];
                StageState& s = b.state[0][ch];
                channels[ch][n] = processStage (const_cast<Coeffs&> (c), s, channels[ch][n]);
            }
        }
    }

    if (msDomain && nCh == 2)
        msToLr (channels[0], channels[1], numSamples);

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
// linear phase (P4)
// ---------------------------------------------------------------------------
void EqEngine::setPhaseMode (PhaseMode m)
{
    if ((int) m == phaseMode_.load (std::memory_order_relaxed))
        return;

    if (m == PhaseMode::Linear)
    {
        // Allocate, THEN flip the mode, THEN design: the audio thread gates on
        // allocated, and until the first design lands it convolves against the
        // identity IR — delayed but transparent, never garbage.
        ensureLinearBuffers();
        phaseMode_.store ((int) m, std::memory_order_release);
        recomputeLinearPhase();
    }
    else
    {
        phaseMode_.store ((int) m, std::memory_order_release);
    }
}

void EqEngine::ensureLinearBuffers()
{
    if (conv_.allocated.load (std::memory_order_relaxed))
        return;

    const size_t fdlLen = (size_t) (kNumParts * kPartBins);
    for (int l = 0; l < 2; ++l)
    {
        conv_.xRe[l].assign (fdlLen, 0.0f);
        conv_.xIm[l].assign (fdlLen, 0.0f);
        conv_.inFifo[l].assign   ((size_t) kPartSize, 0.0f);
        conv_.outReady[l].assign ((size_t) kPartSize, 0.0f);
        conv_.tail[l].assign     ((size_t) kPartSize, 0.0f);
        conv_.accRe[l].assign    ((size_t) kPartBins, 0.0f);
        conv_.accIm[l].assign    ((size_t) kPartBins, 0.0f);
    }
    conv_.fftRe.assign ((size_t) kPartFft, 0.0f);
    conv_.fftIm.assign ((size_t) kPartFft, 0.0f);

    irStaging_.ll.resizeAll(); irStaging_.lr.resizeAll();
    irStaging_.rl.resizeAll(); irStaging_.rr.resizeAll();
    irActive_.ll.resizeAll();  irActive_.lr.resizeAll();
    irActive_.rl.resizeAll();  irActive_.rr.resizeAll();

    // Identity IR (a delta at the centre) so the stream is transparent until
    // the first real design is picked up. A delta at offset 0 of its partition
    // has an all-ones spectrum with no phase term, which is what the assert
    // guarantees.
    static_assert (kIrCentre % kPartSize == 0,
                   "identity IR assumes the centre sits on a partition boundary");
    const int dp = kIrCentre / kPartSize;
    for (int k = 0; k < kPartBins; ++k)
    {
        irActive_.ll.re[(size_t) (dp * kPartBins + k)] = 1.0f;
        irActive_.rr.re[(size_t) (dp * kPartBins + k)] = 1.0f;
    }
    irActive_.cross = false;

    conv_.fill = 0;
    conv_.fdlPos = 0;
    conv_.lanesIn = 0;
    conv_.allocated.store (true, std::memory_order_release);
}

// Message thread. Builds the 2x2 zero-phase magnitude matrix of every active
// STATIC band on the design grid, turns each lane into a windowed symmetric
// FIR, partitions it, and publishes under the IR seqlock.
void EqEngine::recomputeLinearPhase()
{
    if (getPhaseMode() != PhaseMode::Linear) return;
    if (! conv_.allocated.load (std::memory_order_relaxed)) return;

    BandSpec snap[kMaxBands];
    snapshotForAnalysis (snap);
    const int solo = soloBand_.load (std::memory_order_relaxed);

    constexpr int K = kIrFft / 2;
    std::vector<float> mLL ((size_t) K + 1, 1.0f), mLR ((size_t) K + 1, 0.0f),
                       mRL ((size_t) K + 1, 0.0f), mRR ((size_t) K + 1, 1.0f);

    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        const BandSpec& s = snap[bi];
        // Same activity rule as the audio path, including "solo activates".
        const bool active = (solo >= 0) ? (bi == solo) : s.enabled;
        if (! active) continue;
        if (s.dynamic && s.type == BandType::Bell) continue;   // dynamics stay SVF

        for (int k = 0; k <= K; ++k)
        {
            const double omega = kPi * (double) k / (double) K;
            const float  h     = (float) std::abs (bandResponse (s, sampleRate_, omega));
            float& ll = mLL[(size_t) k]; float& lr = mLR[(size_t) k];
            float& rl = mRL[(size_t) k]; float& rr = mRR[(size_t) k];

            switch (s.channel)
            {
                case BandChannel::Stereo: ll *= h; lr *= h; rl *= h; rr *= h; break;
                case BandChannel::Left:   ll *= h; lr *= h; break;
                case BandChannel::Right:  rl *= h; rr *= h; break;
                case BandChannel::Mid:
                case BandChannel::Side:
                {
                    // T⁻¹·diag(hMid,hSide)·T expanded: a on the diagonal,
                    // b coupling the channels.
                    const float a = (s.channel == BandChannel::Mid) ? 0.5f * (h + 1.0f)
                                                                    : 0.5f * (1.0f + h);
                    const float b = (s.channel == BandChannel::Mid) ? 0.5f * (h - 1.0f)
                                                                    : 0.5f * (1.0f - h);
                    const float nLL = a * ll + b * rl, nLR = a * lr + b * rr;
                    const float nRL = b * ll + a * rl, nRR = b * lr + a * rr;
                    ll = nLL; lr = nLR; rl = nRL; rr = nRR;
                    break;
                }
                case BandChannel::NumChannelModes:
                default: break;
            }
        }
    }

    bool cross = false;
    for (int k = 0; k <= K && ! cross; ++k)
        cross = std::fabs (mLR[(size_t) k]) > 1.0e-9f
             || std::fabs (mRL[(size_t) k]) > 1.0e-9f;

    std::vector<float> sRe, sIm;

    const uint32_t s0 = irSeq_.load (std::memory_order_relaxed);
    irSeq_.store (s0 + 1, std::memory_order_release);       // enter write (odd)

    designLane (mLL, irStaging_.ll, sRe, sIm);
    designLane (mRR, irStaging_.rr, sRe, sIm);
    if (cross)
    {
        designLane (mLR, irStaging_.lr, sRe, sIm);
        designLane (mRL, irStaging_.rl, sRe, sIm);
    }
    else
    {
        std::fill (irStaging_.lr.re.begin(), irStaging_.lr.re.end(), 0.0f);
        std::fill (irStaging_.lr.im.begin(), irStaging_.lr.im.end(), 0.0f);
        std::fill (irStaging_.rl.re.begin(), irStaging_.rl.re.end(), 0.0f);
        std::fill (irStaging_.rl.im.begin(), irStaging_.rl.im.end(), 0.0f);
    }
    irStaging_.cross = cross;

    irSeq_.store (s0 + 2, std::memory_order_release);       // leave write (even)
    irPulse_.fetch_add (1, std::memory_order_release);
    irReady_.store (true, std::memory_order_release);
}

// One lane: zero-phase magnitude → symmetric windowed FIR → freq-domain
// partitions. The inverse transform of a real, even spectrum is real and even
// about sample 0; extracting kIrLen samples centred there (mod kIrFft) and
// Hann-windowing yields the linear-phase FIR whose centre is kIrCentre.
void EqEngine::designLane (const std::vector<float>& mag, LaneSpectra& out,
                           std::vector<float>& sRe, std::vector<float>& sIm)
{
    constexpr int K = kIrFft / 2;

    sRe.assign ((size_t) kIrFft, 0.0f);
    sIm.assign ((size_t) kIrFft, 0.0f);
    sRe[0]          = mag[0];
    sRe[(size_t) K] = mag[(size_t) K];
    for (int k = 1; k < K; ++k)
    {
        sRe[(size_t) k]             = mag[(size_t) k];
        sRe[(size_t) (kIrFft - k)]  = mag[(size_t) k];
    }
    eqFft (sRe.data(), sIm.data(), kIrFft, true);

    for (int p = 0; p < kNumParts; ++p)
    {
        float pre[kPartFft] = {}, pim[kPartFft] = {};
        for (int j = 0; j < kPartSize; ++j)
        {
            const int idx = p * kPartSize + j;
            if (idx >= kIrLen) break;
            const int src = (idx - kIrCentre + kIrFft) & (kIrFft - 1);
            const float w = 0.5f * (1.0f - std::cos (2.0f * (float) kPi * (float) idx
                                                     / (float) (kIrLen - 1)));
            pre[j] = sRe[(size_t) src] * w;
        }
        eqFft (pre, pim, kPartFft, false);
        for (int k = 0; k < kPartBins; ++k)
        {
            out.re[(size_t) (p * kPartBins + k)] = pre[k];
            out.im[(size_t) (p * kPartBins + k)] = pim[k];
        }
    }
}

// Audio thread, hop boundary. Copies the staged IR when a new one was
// published; bails without harm if the writer is mid-design.
bool EqEngine::pullIrIfChanged() noexcept
{
    const uint32_t pulse = irPulse_.load (std::memory_order_acquire);
    if (pulse == irLastPulse_ || ! irReady_.load (std::memory_order_acquire))
        return false;

    const size_t nb = (size_t) (kNumParts * kPartBins) * sizeof (float);
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s1 = irSeq_.load (std::memory_order_acquire);
        if (s1 & 1u) return false;                          // writer mid-design

        std::memcpy (irActive_.ll.re.data(), irStaging_.ll.re.data(), nb);
        std::memcpy (irActive_.ll.im.data(), irStaging_.ll.im.data(), nb);
        std::memcpy (irActive_.rr.re.data(), irStaging_.rr.re.data(), nb);
        std::memcpy (irActive_.rr.im.data(), irStaging_.rr.im.data(), nb);
        std::memcpy (irActive_.lr.re.data(), irStaging_.lr.re.data(), nb);
        std::memcpy (irActive_.lr.im.data(), irStaging_.lr.im.data(), nb);
        std::memcpy (irActive_.rl.re.data(), irStaging_.rl.re.data(), nb);
        std::memcpy (irActive_.rl.im.data(), irStaging_.rl.im.data(), nb);
        irActive_.cross = irStaging_.cross;

        const uint32_t s2 = irSeq_.load (std::memory_order_acquire);
        if (s1 == s2) { irLastPulse_ = pulse; return true; }
    }
    return false;                                           // keep old; next hop
}

// One kPartSize hop: FFT the gathered input into the frequency-delay line,
// multiply-accumulate against the IR partitions, inverse-FFT, overlap-add.
void EqEngine::processLinearHop() noexcept
{
    const int  nIn      = std::max (1, conv_.lanesIn);
    const bool stereoIn = (nIn == 2);
    const bool cross    = irActive_.cross;
    const int  nOut     = stereoIn ? 2 : 1;

    conv_.fdlPos = (conv_.fdlPos + 1) % kNumParts;

    float* fre = conv_.fftRe.data();
    float* fim = conv_.fftIm.data();

    for (int l = 0; l < nIn; ++l)
    {
        for (int j = 0; j < kPartSize; ++j) fre[j] = conv_.inFifo[l][(size_t) j];
        std::memset (fre + kPartSize, 0, (size_t) kPartSize * sizeof (float));
        std::memset (fim, 0, (size_t) kPartFft * sizeof (float));
        eqFft (fre, fim, kPartFft, false);

        float* dRe = &conv_.xRe[l][(size_t) (conv_.fdlPos * kPartBins)];
        float* dIm = &conv_.xIm[l][(size_t) (conv_.fdlPos * kPartBins)];
        std::memcpy (dRe, fre, (size_t) kPartBins * sizeof (float));
        std::memcpy (dIm, fim, (size_t) kPartBins * sizeof (float));
    }

    for (int l = 0; l < nOut; ++l)
    {
        std::memset (conv_.accRe[l].data(), 0, (size_t) kPartBins * sizeof (float));
        std::memset (conv_.accIm[l].data(), 0, (size_t) kPartBins * sizeof (float));
    }

    for (int p = 0; p < kNumParts; ++p)
    {
        const int slot = (conv_.fdlPos - p + kNumParts) % kNumParts;
        const size_t xo = (size_t) (slot * kPartBins);
        const size_t ho = (size_t) (p * kPartBins);

        const float* xLre = &conv_.xRe[0][xo];
        const float* xLim = &conv_.xIm[0][xo];
        // Mono: the one lane is conceptually L == R, so the cross lane (when
        // present) folds onto the same spectrum and y = (ll + lr) · x.
        const float* xRre = stereoIn ? &conv_.xRe[1][xo] : xLre;
        const float* xRim = stereoIn ? &conv_.xIm[1][xo] : xLim;

        float* aLre = conv_.accRe[0].data();
        float* aLim = conv_.accIm[0].data();

        {
            const float* hre = &irActive_.ll.re[ho];
            const float* him = &irActive_.ll.im[ho];
            for (int k = 0; k < kPartBins; ++k)
            {
                aLre[k] += hre[k] * xLre[k] - him[k] * xLim[k];
                aLim[k] += hre[k] * xLim[k] + him[k] * xLre[k];
            }
        }
        if (cross)
        {
            const float* hre = &irActive_.lr.re[ho];
            const float* him = &irActive_.lr.im[ho];
            for (int k = 0; k < kPartBins; ++k)
            {
                aLre[k] += hre[k] * xRre[k] - him[k] * xRim[k];
                aLim[k] += hre[k] * xRim[k] + him[k] * xRre[k];
            }
        }

        if (nOut == 2)
        {
            float* aRre = conv_.accRe[1].data();
            float* aRim = conv_.accIm[1].data();
            {
                const float* hre = &irActive_.rr.re[ho];
                const float* him = &irActive_.rr.im[ho];
                for (int k = 0; k < kPartBins; ++k)
                {
                    aRre[k] += hre[k] * xRre[k] - him[k] * xRim[k];
                    aRim[k] += hre[k] * xRim[k] + him[k] * xRre[k];
                }
            }
            if (cross)
            {
                const float* hre = &irActive_.rl.re[ho];
                const float* him = &irActive_.rl.im[ho];
                for (int k = 0; k < kPartBins; ++k)
                {
                    aRre[k] += hre[k] * xLre[k] - him[k] * xLim[k];
                    aRim[k] += hre[k] * xLim[k] + him[k] * xLre[k];
                }
            }
        }
    }

    for (int l = 0; l < nOut; ++l)
    {
        const float* aRe = conv_.accRe[l].data();
        const float* aIm = conv_.accIm[l].data();

        // rebuild the full hermitian spectrum, inverse-transform, overlap-add
        fre[0] = aRe[0]; fim[0] = aIm[0];
        fre[kPartSize] = aRe[kPartSize]; fim[kPartSize] = aIm[kPartSize];
        for (int k = 1; k < kPartSize; ++k)
        {
            fre[k] = aRe[k];            fim[k] = aIm[k];
            fre[kPartFft - k] = aRe[k]; fim[kPartFft - k] = -aIm[k];
        }
        eqFft (fre, fim, kPartFft, true);

        float* outR = conv_.outReady[l].data();
        float* tail = conv_.tail[l].data();
        for (int j = 0; j < kPartSize; ++j)
        {
            outR[j] = fre[j] + tail[j];
            tail[j] = fre[kPartSize + j];
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
