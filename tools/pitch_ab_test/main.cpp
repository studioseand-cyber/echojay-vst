// pitch_ab_test — the ANTARES-ANCHORED quality gate for EchoJay Pitch.
//
// WHY THIS EXISTS. An independent A/B against Antares Auto-Tune Pro on the
// same take with matched settings (PITCH_P0_VALIDATION.md §16) found EchoJay
// losing in three separate, measurable ways: under-correction (median 16.9
// cents from the nearest semitone against Antares's 8.5), lost harmonicity
// (-1.55 dB HNR against -0.19), and frame-to-frame roughness (+24.6%
// spectral flux above dry against +3.8%). Every existing test was green
// throughout. This gate reproduces that analysis and asserts against the
// ANTARES numbers - measured fresh from the reference bounce each run, never
// transcribed - because a test that only compares us to yesterday's us will
// happily ratchet in the wrong direction.
//
// MATERIAL. Three bounces of the same take, in one folder, named:
//     dry.wav   echojay.wav   antares.wav
// (the echojay.wav bounce is read for reporting only - the gated column is
// RENDERED HERE from dry.wav through the current engine, so the gate always
// measures the code being built, not a stale bounce). The folder path lives
// in the git-ignored sibling file:
//     tools/pitch_ab_test/material.local
// When absent, the tool explains itself and exits 0 - weaker, never silent.
//
// SETTINGS are the HARD-MATCH: low_male, chromatic, retune 0, flex 0,
// humanize 0, natural_vibrato 0, targeting_ignores_vibrato ON - the
// corrected correction_mode=hard configuration (the spec's §4 table setting
// it off for tuned/hard was a spec error, fixed 2026-08-14: unsmoothed
// target selection flips between adjacent degrees whenever wide vibrato
// sits on a semitone boundary, at any retune speed). NOT the
// schema defaults: natural_vibrato defaults to 100 and deliberately re-adds
// the singer's wobble on top of the snapped note, which is a character
// choice, not a defect - the original A/B's "matched settings" left it at
// 40 (via correction_mode tuned) and measured the wobble as under-correction.
// Antares retune 0 flattens vibrato, so the honest comparison sets 0.
//
// Build (JUCE-free, same pattern as test/psola_engine_test.cpp):
//   g++ -std=c++17 -O2 -ISource tools/pitch_ab_test/main.cpp -o pitch_ab && ./pitch_ab <folder>

#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"
#include "EedPsolaEngine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

// ---- the Antares-anchored margins ----------------------------------------
// The goal is zero on all three. These are the measured remaining gaps after
// the §16 rebuild (splice-resampler preserve), each with a little room for
// material noise - NOT quality targets. Tighten them as the gaps close;
// never widen them to make a regression pass.
static constexpr double kCentsGapC  = 1.5;   // measured +0.9c vs Antares
static constexpr double kHnrGapDb   = 0.25;  // measured -0.04 dB (we WIN this one)
static constexpr double kFluxGapPp  = 2.0;   // measured +1.2 points vs Antares

// HF-excess event ceiling (events/s where our >6 kHz peak envelope exceeds
// the Antares bounce's LOCAL MAX by >15 dB). The +/-3 ms reference window is
// load-bearing: both processors are resamplers whose output timing wobbles a
// few ms against each other, and an instantaneous compare reads that skew as
// level at every shared source transient - measured, it manufactures ~4
// events/s that vanish at any tolerance >= 2 ms and sit at identical times
// across all four voice types (i.e. they are the take's own consonants).
// Current truth: the HOST bounce measures 0; the offline render measures one
// marginal event at a note-boundary chatter with no waveform step. Goal 0.
static constexpr double kHfxThreshDb = 15.0;
static constexpr double kHfxTolMs    = 3.0;
static constexpr double kHfxMaxPerSec = 0.30;   // = the two marginal events on the reference take; a third fails

// Pitch-excursion ceiling: events where the output's own pitch track departs
// its local median by more than kExcCents and RETURNS within kExcMaxMs. The
// audible defect this measures is a momentary wrong NOTE - a grain-accurate
// shift to the wrong pitch is perfectly smooth in the waveform and invisible
// to every discontinuity gate (the independent A/B measured 3.5% of our
// voiced frames >600c from the dry against Antares's 0.44%, mostly downward,
// the sub-harmonic direction). Antares is the target; the margin allows the
// same count, not one more.
static constexpr double kExcCents  = 400.0;
static constexpr double kExcMaxMs  = 150.0;
// Initial calibration, not a widening: ours measures Antares+1 on the
// reference take, and the +1 is dissected in §16.8 - the take's own creak
// onset at 1.89 s, where our hard-tuned onset (creak at 87 Hz into wet at
// 185) reads sub-octave to the tracker for ~15 ms across the seam. It is
// NOT a mid-phrase octave spike (those were driven to zero by F0JumpGate,
// which held 345 hops on the acapella); any spike this metric was built
// for adds an event and fails. Tighten to 0 when the onset seam closes.
static constexpr int    kExcMargin = 1;         // ours <= antares + this

// ---------------------------------------------------------------------------
// WAV I/O (PCM16/24, float32; channels averaged)
// ---------------------------------------------------------------------------
static bool readWavMono (const char* path, std::vector<float>& out, double& fs)
{
    FILE* f = std::fopen (path, "rb");
    if (! f) return false;
    auto rd32 = [&] { uint8_t b[4]; if (std::fread (b, 1, 4, f) != 4) return 0u;
                      return (uint32_t) (b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24)); };
    auto rd16 = [&] { uint8_t b[2]; if (std::fread (b, 1, 2, f) != 2) return 0u;
                      return (uint32_t) (b[0] | (b[1] << 8)); };
    char tag[5] = {};
    if (std::fread (tag, 1, 4, f) != 4 || std::strncmp (tag, "RIFF", 4) != 0) { std::fclose (f); return false; }
    rd32();
    if (std::fread (tag, 1, 4, f) != 4 || std::strncmp (tag, "WAVE", 4) != 0) { std::fclose (f); return false; }
    uint16_t fmt = 0, chans = 0, bits = 0; uint32_t rate = 0;
    while (std::fread (tag, 1, 4, f) == 4)
    {
        const uint32_t sz = rd32();
        if (std::strncmp (tag, "fmt ", 4) == 0)
        {
            fmt = (uint16_t) rd16(); chans = (uint16_t) rd16();
            rate = rd32(); rd32(); rd16(); bits = (uint16_t) rd16();
            if (sz > 16) std::fseek (f, (long) sz - 16, SEEK_CUR);
        }
        else if (std::strncmp (tag, "data", 4) == 0)
        {
            if (chans == 0) { std::fclose (f); return false; }
            const uint32_t bytesPer = bits / 8u;
            const uint32_t frames = sz / (bytesPer * chans);
            out.resize (frames);
            std::vector<uint8_t> fr (bytesPer * chans);
            for (uint32_t i = 0; i < frames; ++i)
            {
                if (std::fread (fr.data(), 1, fr.size(), f) != fr.size()) break;
                double acc = 0.0;
                for (uint16_t c = 0; c < chans; ++c)
                {
                    const uint8_t* b = fr.data() + c * bytesPer;
                    if (fmt == 3 && bits == 32) { float v; std::memcpy (&v, b, 4); acc += v; }
                    else if (fmt == 1 && bits == 16) { acc += (double) (int16_t) (b[0] | (b[1] << 8)) / 32768.0; }
                    else if (fmt == 1 && bits == 24)
                    { acc += (double) ((int32_t) ((b[0] << 8) | (b[1] << 16) | ((uint32_t) b[2] << 24)) >> 8) / 8388608.0; }
                    else { std::fclose (f); return false; }
                }
                out[i] = (float) (acc / chans);
            }
            fs = (double) rate; std::fclose (f); return true;
        }
        else std::fseek (f, (long) (sz + (sz & 1)), SEEK_CUR);
    }
    std::fclose (f); return false;
}

// ---------------------------------------------------------------------------
// The render: dry through the CURRENT engine at the hard-match settings,
// mirroring EedPitchProcessor::processBlock's hop slicing and target hold.
// ---------------------------------------------------------------------------
static std::vector<float> renderEchoJay (const std::vector<float>& in, double fs,
                                         int fmodeArg = -1, float fshiftArg = 0.0f,
                                         bool disableSplice = false)
{
    constexpr int vt = PitchEngine::kLowMale;

    PitchEngine det;
    det.prepare (fs, 256);
    det.setVoiceType (vt);
    det.setTracking (PitchEngine::kNormal);
    const float hopMs = det.hopMs();

    PitchCorrect corr;
    corr.prepare (fs, det.inputHopLength (vt));
    corr.initDegrees();
    for (int s = 0; s < 12; ++s) corr.setDegree (s, true, 0.0f);   // chromatic
    corr.setRetuneMs (0.0f);
    corr.setFlex (0.0f);
    corr.setHumanize (0.0f);
    corr.setIgnoreVibrato (true);
    corr.setNaturalVibrato (0.0f);
    // Acceptance-render overrides (3 Sep 2026, the D-minor rotation):
    // AB_ROOT=<0..11 C-frame>, AB_SCALE=major|minor, AB_PRESET=natural|
    // balanced|tuned|hard. With AB_DUMP this renders an acceptance wav
    // through the exact shipped pipeline; the hard-match gates are skipped
    // when any override is set (their expectations assume chromatic).
    if (const char* sc = getenv ("AB_SCALE"))
        if (*sc != 0 && std::string (sc) != "chromatic")
        {
            static const int kMajor[7] = { 0,2,4,5,7,9,11 };
            static const int kMinor[7] = { 0,2,3,5,7,8,10 };
            const int* set = std::string (sc) == "minor" ? kMinor : kMajor;
            for (int s = 0; s < 12; ++s) corr.setDegree (s, false, 0.0f);
            for (int k = 0; k < 7; ++k) corr.setDegree (set[k], true, 0.0f);
        }
    if (const char* r = getenv ("AB_ROOT")) corr.setKeyRoot (std::atoi (r));
    if (const char* p = getenv ("AB_PRESET"))
    {
        const std::string ps (p);
        struct P { float retune, flex, hum, nv; };
        const P v = ps == "natural"  ? P { 120.0f, 55.0f, 60.0f, 100.0f }
                  : ps == "balanced" ? P {  40.0f, 25.0f, 30.0f, 100.0f }
                  : ps == "tuned"    ? P {   8.0f,  0.0f,  0.0f,  40.0f }
                  :                    P {   0.0f,  0.0f,  0.0f,   0.0f };
        corr.setRetuneMs (v.retune); corr.setFlex (v.flex);
        corr.setHumanize (v.hum);    corr.setNaturalVibrato (v.nv);
    }
    if (const char* iv = getenv ("AB_IGNVIB"))
        corr.setIgnoreVibrato (std::atoi (iv) != 0);
    corr.reset();

    F0JumpGate f0Gate;    // mirrors EedPitchProcessor::processBlock

    PitchEngine det2;
    det2.prepare (fs, 256);
    det2.setVoiceType (vt);
    det2.setTracking (PitchEngine::kNormal);

    float worst = PitchEngine::voiceRange (0).fMinHz;
    for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
        worst = std::min (worst, PitchEngine::voiceRange (t).fMinHz);

    PsolaEngine sh;
    sh.prepare (fs, 256, PitchEngine::voiceRange (vt).fMinHz, worst);
    // DIAGNOSTIC overrides, not the gate: AB_FORMANT_MODE=off|preserve|shift
    // and AB_FORMANT_SHIFT=<st> run the same six metrics on the other
    // formant modes so they can be compared against the preserve column.
    // The shipped gate run sets neither and asserts at preserve.
    int fmode = fmodeArg >= 0 ? fmodeArg : PsolaEngine::kFormantPreserve;
    if (fmodeArg < 0)
    {
        if (const char* m = getenv ("AB_FORMANT_MODE"))
        {
            if (! std::strcmp (m, "off"))   fmode = PsolaEngine::kFormantOff;
            if (! std::strcmp (m, "shift")) fmode = PsolaEngine::kFormantShift;
        }
    }
    sh.setFormantMode (fmode);
    float fshift = fshiftArg;
    if (fmodeArg < 0)
        if (const char* s = getenv ("AB_FORMANT_SHIFT")) fshift = (float) std::atof (s);
    sh.setFormantShift (fshift);
    sh.debugDisableSplice (disableSplice);
    sh.setPitchLagSamples (det2.pitchLagFor (vt));
    sh.setDriftBleed (true);   // mirrors the processor (5 Sep 2026 ruling)
    const int latency = sh.latencySamples();

    // THE BLOCK IS SLICED AT HOP BOUNDARIES, exactly as
    // EedPitchProcessor::processBlock does it. Applying a hop's values at
    // block granularity instead was measured at +28.6% spectral flux against
    // +3.9% for exact slicing - the splice-resampler's ratio reads the f0
    // ring precisely, and a target that leads or lags it by up to a block
    // turns every note change into a transient wrong-ratio spike.
    std::vector<float> out (in.size(), 0.0f);
    PitchEngine::HopEvent ev[64];
    float target = 0.0f, sliceF0 = 0.0f;
    float shift  = echojay::PsolaEngine::kNoShift;   // mirrors the processor
    bool  sliceVoiced = false;
    for (size_t p = 0; p + 256 <= in.size(); p += 256)
    {
        det.process (in.data() + p, nullptr, 256);
        const int nHops = det.drainHops (ev, 64);
        const uint64_t blockStart = det.inputPosition() - 256;

        int cursor = 0;
        for (int h = 0; h <= nHops; ++h)
        {
            int sliceEnd = 256;
            if (h < nHops)
            {
                const int64_t rel = (int64_t) ev[h].inputPos - (int64_t) blockStart;
                sliceEnd = (int) std::clamp (rel, (int64_t) cursor, (int64_t) 256);
            }
            if (sliceEnd > cursor)
            {
                sh.process (in.data() + p + (size_t) cursor,
                            out.data() + p + (size_t) cursor,
                            sliceEnd - cursor, sliceF0, sliceVoiced, target, shift);
                cursor = sliceEnd;
            }
            if (h < nHops)
            {
                float rOldT = -1.0f, rNewT = -1.0f;
                const bool seeding = ev[h].voiced && ev[h].f0Hz > 0.0f
                                  && f0Gate.lastGood() <= 0.0f;
                if (f0Gate.isBigJump (ev[h].f0Hz, ev[h].voiced) || seeding)
                {
                    const double refHz = seeding ? 2.0 * (double) ev[h].f0Hz
                                                 : (double) f0Gate.lastGood();
                    const int tOld = (int) std::lround (fs / refHz);
                    const int tNew = (int) std::lround (fs / (double) ev[h].f0Hz);
                    rOldT = sh.inputPeriodicity (ev[h].inputPos, tOld);
                    rNewT = sh.inputPeriodicity (ev[h].inputPos, tNew);
                }
                const float gatedF0 = f0Gate.filter (ev[h].f0Hz, ev[h].voiced, hopMs,
                                                     rOldT, rNewT);
                sliceF0 = gatedF0; sliceVoiced = ev[h].voiced;
                const float t = corr.process (gatedF0, ev[h].voiced, hopMs);
                if (t > 0.0f) { target = t;            // hold through gaps
                                shift  = corr.shiftPreferred()
                                             ? corr.lastShiftCents()
                                             : echojay::PsolaEngine::kNoShift; }
            }
        }
        if (cursor < 256)
            sh.process (in.data() + p + (size_t) cursor,
                        out.data() + p + (size_t) cursor,
                        256 - cursor, sliceF0, sliceVoiced, target, shift);
    }

    std::vector<float> aligned (in.size(), 0.0f);
    for (size_t i = (size_t) latency; i < out.size(); ++i)
        aligned[i - (size_t) latency] = out[i];
    return aligned;
}

// ---------------------------------------------------------------------------
// Measurement (definitions frozen; the positive control in §16 reproduced the
// independent analysis with these: dry 29.5/16.9-era-echojay 15.7/antares 11.2
// cents against their 28.7/16.9/8.5 on a different tracker)
// ---------------------------------------------------------------------------
struct Track { std::vector<float> f0, conf; };

static Track trackFile (const std::vector<float>& x, double fs)
{
    PitchEngine e;
    e.prepare (fs, 8192);
    e.setVoiceType (PitchEngine::kLowMale);
    e.setTracking (PitchEngine::kNormal);
    const int hop = e.inputHopLength (PitchEngine::kLowMale);
    Track t;
    for (size_t p = 0; p + (size_t) hop <= x.size(); p += (size_t) hop)
    {
        e.process (x.data() + p, nullptr, hop);
        const PitchReading r = e.getReading();
        t.f0.push_back (r.voiced ? r.f0Hz : 0.0f);
        t.conf.push_back (r.confidence);
    }
    // 5-frame median smoothing over voiced runs, matching the §16 analyzer.
    std::vector<float> f = t.f0;
    for (size_t i = 2; i + 2 < f.size(); ++i)
    {
        float v[5]; int n = 0;
        for (int k = -2; k <= 2; ++k) if (f[i + (size_t) k] > 0.0f) v[n++] = f[i + (size_t) k];
        if (n >= 3 && f[i] > 0.0f) { std::sort (v, v + n); t.f0[i] = v[n / 2]; }
    }
    return t;
}

static double centsFromET (double f0)
{
    const double midi = 69.0 + 12.0 * std::log2 (f0 / 440.0);
    return std::fabs (midi - std::round (midi)) * 100.0;
}

static double hnrAt (const std::vector<float>& x, double fs, size_t centre, double f0)
{
    const int W = (int) (0.040 * fs);
    if (centre < (size_t) W / 2 || centre + (size_t) W / 2 + 1 >= x.size() || f0 <= 0.0) return -999.0;
    const size_t from = centre - (size_t) (W / 2);
    std::vector<double> w ((size_t) W);
    double mean = 0.0;
    for (int i = 0; i < W; ++i) mean += x[from + (size_t) i];
    mean /= W;
    for (int i = 0; i < W; ++i)
    {
        const double h = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (W - 1));
        w[(size_t) i] = h * ((double) x[from + (size_t) i] - mean);
    }
    const int T = (int) std::lround (fs / f0);
    const int lo = std::max (8, (int) (T * 0.8)), hi = std::min (W - 2, (int) (T * 1.2));
    double best = -1.0;
    for (int k = lo; k <= hi; ++k)
    {
        double rk = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i + k < W; ++i)
        {
            rk += w[(size_t) i] * w[(size_t) (i + k)];
            e1 += w[(size_t) i] * w[(size_t) i];
            e2 += w[(size_t) (i + k)] * w[(size_t) (i + k)];
        }
        best = std::max (best, rk / std::sqrt (std::max (1e-18, e1 * e2)));
    }
    best = std::min (best, 0.9999);
    if (best <= 0.0) return -999.0;
    return 10.0 * std::log10 (best / (1.0 - best));
}

static void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * M_PI / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

static double fluxTotal (const std::vector<float>& x)
{
    constexpr int N = 1024, H = 256;
    std::vector<double> prev ((size_t) N / 2, 0.0);
    bool have = false;
    double total = 0.0;
    std::vector<std::complex<double>> buf ((size_t) N);
    for (size_t p = 0; p + N < x.size(); p += H)
    {
        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (N - 1));
            buf[(size_t) i] = std::complex<double> (w * (double) x[p + (size_t) i], 0.0);
        }
        fft (buf);
        double d = 0.0;
        for (int k = 0; k < N / 2; ++k)
        {
            const double m = std::abs (buf[(size_t) k]);
            if (have) { const double e = m - prev[(size_t) k]; d += e * e; }
            prev[(size_t) k] = m;
        }
        if (have) total += std::sqrt (d);
        have = true;
    }
    return total;
}

static double median (std::vector<double> v)
{ if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v[v.size() / 2]; }

// ---- HF excess (>6 kHz, timing-robust) -----------------------------------
static std::vector<float> highpass6k (const std::vector<float>& x, double fs)
{
    std::vector<float> y = x;
    const double qv[2] = { 0.54119610, 1.30656296 };   // 4th-order Butterworth
    for (int s = 0; s < 2; ++s)
    {
        const double w0 = 2.0 * M_PI * 6000.0 / fs;
        const double alpha = std::sin (w0) / (2.0 * qv[s]);
        const double cosw = std::cos (w0);
        const double b0 = (1 + cosw) / 2, b1 = -(1 + cosw), b2 = (1 + cosw) / 2;
        const double a0 = 1 + alpha, a1 = -2 * cosw, a2 = 1 - alpha;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : y)
        {
            const double xn = v;
            const double yn = (b0 * xn + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1; x1 = xn; y2 = y1; y1 = yn;
            v = (float) yn;
        }
    }
    return y;
}

// 2 ms peak envelope in dB, 0.5 ms hop.
static std::vector<double> hfEnvDb (const std::vector<float>& x, double fs, int& hopOut)
{
    const auto hf = highpass6k (x, fs);
    const int W = (int) (0.002 * fs), H = (int) (0.0005 * fs);
    hopOut = H;
    std::vector<double> e;
    for (size_t p = 0; p + (size_t) W < hf.size(); p += (size_t) H)
    {
        double s = 0.0;
        for (int i = 0; i < W; ++i) s = std::max (s, (double) std::fabs (hf[p + (size_t) i]));
        e.push_back (20.0 * std::log10 (std::max (1e-7, s)));
    }
    return e;
}

// ---- clicks (the pitch_click_test detector, Antares-anchored) ------------
// Prediction error > 20x local median where the dry has no transient -
// with the drift-aware patch-match exclusion (a candidate whose waveform
// matches the dry at >= 0.97 correlation within the splice-drift window,
// whose matched dry twin is itself near threshold, is the source's own
// edge). Same rules as tools/pitch_click_test after instrument failure #11.
static int clickEvents (const std::vector<float>& x, const std::vector<float>& dry, double fs)
{
    // Coarse alignment (the bounces are host-compensated to within a few ms).
    int lag = 0;
    {
        const size_t from = (size_t) (fs * 1.0), len = (size_t) std::min ((double) dry.size() - from - 4801, fs * 3.0);
        double best = -1e30;
        for (int L = -2400; L <= 2400; L += 4)
        {
            double s = 0.0;
            for (size_t i = from; i < from + len; i += 8)
            {
                const long j = (long) i + L;
                if (j >= 0 && j < (long) x.size()) s += (double) dry[i] * x[(size_t) j];
            }
            if (s > best) { best = s; lag = L; }
        }
    }

    auto pe = [] (const std::vector<float>& v, long i)
    { return std::fabs ((double) v[(size_t) i] - (2.0 * v[(size_t) (i - 1)] - v[(size_t) (i - 2)])); };
    auto scaleAt = [&] (const std::vector<float>& v, long i)
    {
        std::vector<double> w;
        for (long j = std::max (2L, i - 512); j < std::min ((long) v.size() - 1, i + 512); j += 2)
            w.push_back (pe (v, j));
        std::nth_element (w.begin(), w.begin() + (long) (w.size() / 2), w.end());
        return w[w.size() / 2];
    };

    int events = 0;
    double lastT = -1.0;
    for (long i = 600; i + 600 < (long) x.size(); ++i)
    {
        const double e = pe (x, i);
        if (e < 0.02 && (i % 8) != 0) continue;
        const double sc = scaleAt (x, i);
        if (sc <= 0.0 || e < 20.0 * sc) continue;

        const long di = i - lag;      // position in dry
        if (di - 600 < 0 || di + 600 >= (long) dry.size()) continue;
        bool excl = false;
        for (long j = di - 64; j <= di + 64 && ! excl; ++j)
            if (pe (dry, j) > 20.0 * scaleAt (dry, j)) excl = true;
        if (! excl)
        {
            double bestC = -2.0; long bestJ = di;
            for (int off = -480; off <= 480; off += 2)
            {
                double ab = 0, aa = 0, bb = 0;
                for (int k = -64; k <= 64; k += 2)
                {
                    const double a = x[(size_t) (i + k)], b = dry[(size_t) (di + off + k)];
                    ab += a * b; aa += a * a; bb += b * b;
                }
                if (aa < 1e-12 || bb < 1e-12) continue;
                const double c = ab / std::sqrt (aa * bb);
                if (c > bestC) { bestC = c; bestJ = di + off; }
            }
            if (bestC >= 0.97)
            {
                double dMax = 0.0;
                for (int k = -8; k <= 8; ++k) dMax = std::max (dMax, pe (dry, bestJ + k));
                if (dMax > 0.6 * 20.0 * scaleAt (dry, bestJ)) excl = true;
            }
        }
        if (excl) continue;
        const double t = (double) i / fs;
        if (t - lastT > 0.004) ++events;
        lastT = t;
    }
    return events;
}

// ---- pitch excursions (momentary wrong notes) ----------------------------
// Events where a file's own pitch track departs its +/-150 ms local median
// by more than kExcCents and comes back within kExcMaxMs. The local median
// is robust to the excursion itself (<= 50% of the window); a genuine note
// change moves the median with it and never counts.
static int excursionEvents (const Track& t, double hopSec)
{
    const int W = (int) (0.150 / hopSec);              // +/- window for the median
    const int maxRun = (int) (kExcMaxMs * 0.001 / hopSec);
    const size_t n = t.f0.size();

    int events = 0;
    int run = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (t.f0[i] <= 0.0f) { run = 0; continue; }
        std::vector<float> win;
        for (long j = (long) i - W; j <= (long) i + W; ++j)
            if (j >= 0 && (size_t) j < n && t.f0[(size_t) j] > 0.0f) win.push_back (t.f0[(size_t) j]);
        if (win.size() < 8) { run = 0; continue; }
        std::nth_element (win.begin(), win.begin() + (long) (win.size() / 2), win.end());
        const float med = win[win.size() / 2];
        const double dev = std::fabs (1200.0 * std::log2 ((double) t.f0[i] / (double) med));
        if (dev > kExcCents) ++run;
        else
        {
            // The run just ended on a RETURNED voiced frame - the shape the
            // metric asks for. Runs cut short by silence do not count (a
            // wrong note into a gap is a different defect).
            if (run > 0 && run <= maxRun) ++events;
            run = 0;
        }
    }
    return events;
}

// Count events where ours exceeds the reference's +/-tol local max by thr dB.
static int hfExcessEvents (const std::vector<double>& eO, const std::vector<double>& eR,
                           int hop, double fs)
{
    std::vector<double> sorted = eR;
    std::sort (sorted.begin(), sorted.end());
    const double floorDb = sorted[sorted.size() / 2] - 6.0;
    const int tol = (int) (kHfxTolMs * 0.001 * fs) / hop;

    int events = 0;
    bool in = false;
    double lastT = -1.0;
    for (size_t i = 0; i < eO.size(); ++i)
    {
        double r = floorDb;
        for (long j = (long) i - tol; j <= (long) i + tol; ++j)
            if (j >= 0 && (size_t) j < eR.size()) r = std::max (r, eR[(size_t) j]);
        const bool hot = eO[i] - r > kHfxThreshDb;
        const double t = (double) (i * (size_t) hop) / fs;
        if (hot && ! in && t - lastT > 0.005) { ++events; lastT = t; }
        in = hot;
    }
    return events;
}

struct Metrics { double medCents = 0, within5 = 0, medHnr = 0, fluxPct = 0; int unvoiced = 0; };

static Metrics measure (const std::vector<float>& x, const Track& t, double fs,
                        const std::vector<size_t>& sel, int hop, double dryFlux)
{
    Metrics m;
    std::vector<double> cents, hnrs;
    for (size_t i : sel)
    {
        const float f0 = i < t.f0.size() ? t.f0[i] : 0.0f;
        if (f0 <= 0.0f) { ++m.unvoiced; continue; }
        cents.push_back (centsFromET (f0));
        const double h = hnrAt (x, fs, i * (size_t) hop + (size_t) hop / 2, f0);
        if (h > -500.0) hnrs.push_back (h);
    }
    int w5 = 0;
    for (double c : cents) if (c < 5.0) ++w5;
    m.medCents = median (cents);
    m.within5  = cents.empty() ? 0.0 : 100.0 * w5 / (double) cents.size();
    m.medHnr   = median (hnrs);
    m.fluxPct  = 100.0 * (fluxTotal (x) / dryFlux - 1.0);
    return m;
}

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf ("usage: pitch_ab_test <folder with dry.wav / echojay.wav / antares.wav>\n");
        return 1;
    }
    const std::string dir (argv[1]);

    std::vector<float> dry, bounce, antares;
    double fs = 0.0, fs2 = 0.0, fs3 = 0.0;
    const char* srcOverride = getenv ("AB_SRC");   // acceptance renders
    if (! readWavMono ((srcOverride != nullptr ? std::string (srcOverride)
                                               : dir + "/dry.wav").c_str(), dry, fs)
        || ! readWavMono ((dir + "/antares.wav").c_str(), antares, fs3))
    { std::printf ("cannot read dry.wav/antares.wav in %s\n", dir.c_str()); return 1; }
    const bool haveBounce = readWavMono ((dir + "/echojay.wav").c_str(), bounce, fs2);

    std::printf ("material: %s (%.0f Hz, %.1f s)\n", dir.c_str(), fs, (double) dry.size() / fs);

    // The gated column: rendered HERE from the current engine.
    const std::vector<float> ours = renderEchoJay (dry, fs);
    const bool overridden = getenv ("AB_SCALE") != nullptr
                         || getenv ("AB_ROOT") != nullptr
                         || getenv ("AB_PRESET") != nullptr;

    // Forensics hook: AB_DUMP=<path> writes the gated render as float32 WAV,
    // so external instruments inspect EXACTLY what the assertions measured.
    if (const char* dump = getenv ("AB_DUMP"))
    {
        FILE* f = std::fopen (dump, "wb");
        if (f)
        {
            auto w32 = [&] (uint32_t v) { uint8_t b[4] = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) }; std::fwrite (b, 1, 4, f); };
            auto w16 = [&] (uint16_t v) { uint8_t b[2] = { (uint8_t) v, (uint8_t) (v >> 8) }; std::fwrite (b, 1, 2, f); };
            const uint32_t dataBytes = (uint32_t) (ours.size() * 4);
            std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataBytes); std::fwrite ("WAVE", 1, 4, f);
            std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (3); w16 (1); w32 ((uint32_t) fs);
            w32 ((uint32_t) fs * 4); w16 (4); w16 (32);
            std::fwrite ("data", 1, 4, f); w32 (dataBytes);
            std::fwrite (ours.data(), 4, ours.size(), f);
            std::fclose (f);
            std::printf ("render dumped to %s\n", dump);
        }
    }
    if (overridden)
    {
        std::printf ("overrides active (AB_SCALE/AB_ROOT/AB_PRESET): render "
                     "dumped, hard-match gates SKIPPED.\n");
        return 0;
    }

    const Track td = trackFile (dry, fs);
    const Track to = trackFile (ours, fs);
    const Track ta = trackFile (antares, fs);

    PitchEngine probe; probe.prepare (fs, 8192);
    const int hop = probe.inputHopLength (PitchEngine::kLowMale);

    std::vector<size_t> sel;
    for (size_t i = 0; i < td.f0.size(); ++i)
        if (td.f0[i] > 0.0f && td.conf[i] >= 0.70f) sel.push_back (i);

    const double dryFlux = fluxTotal (dry);
    Metrics md = measure (dry, td, fs, sel, hop, dryFlux);
    Metrics mo = measure (ours, to, fs, sel, hop, dryFlux);
    Metrics ma = measure (antares, ta, fs, sel, hop, dryFlux);

    std::printf ("\n%zu frames where dry is confidently voiced\n", sel.size());
    std::printf ("%-10s %9s %9s %9s %9s %8s\n", "file", "medCents", "within5", "medHNR", "flux", "untrkd");
    auto row = [&] (const char* n, const Metrics& m)
    { std::printf ("%-10s %8.1fc %8.1f%% %7.2fdB %+7.1f%% %8d\n",
                   n, m.medCents, m.within5, m.medHnr, m.fluxPct, m.unvoiced); };
    row ("dry", md);
    row ("echojay", mo);
    row ("antares", ma);
    if (haveBounce)
    {
        const Track tb = trackFile (bounce, fs2);
        Metrics mb = measure (bounce, tb, fs, sel, hop, dryFlux);
        row ("(bounce)", mb);   // reporting only - possibly a stale build/settings
    }

    std::printf ("\n== the three findings, gated against the measured Antares column ==\n");
    check (sel.size() > 500, "enough confidently-voiced dry frames to mean anything");

    char msg[240];
    std::snprintf (msg, sizeof (msg),
                   "under-correction: median %.1fc vs Antares %.1fc (margin %.1fc)",
                   mo.medCents, ma.medCents, kCentsGapC);
    check (mo.medCents <= ma.medCents + kCentsGapC, msg);

    const double dHnrOurs = mo.medHnr - md.medHnr;
    const double dHnrAnt  = ma.medHnr - md.medHnr;
    std::snprintf (msg, sizeof (msg),
                   "harmonicity: HNR delta vs dry %+.2f dB vs Antares %+.2f dB (margin %.2f)",
                   dHnrOurs, dHnrAnt, kHnrGapDb);
    check (dHnrOurs >= dHnrAnt - kHnrGapDb, msg);

    std::snprintf (msg, sizeof (msg),
                   "roughness: spectral flux %+.1f%% vs Antares %+.1f%% (margin %.1f points)",
                   mo.fluxPct, ma.fluxPct, kFluxGapPp);
    check (mo.fluxPct <= ma.fluxPct + kFluxGapPp, msg);

    // ---- HF-excess events, with the instrument's own positive control ----
    {
        int hop = 0;
        const auto eA = hfEnvDb (antares, fs, hop);
        const auto eO = hfEnvDb (ours, fs, hop);
        const int clean = hfExcessEvents (eO, eA, hop, fs);
        const double dur = (double) ours.size() / fs;

        // Control: 25 one-sample-onset steps at 3x local RMS injected into a
        // COPY of our render must be found, or the zero above means nothing.
        std::vector<float> doctored = ours;
        int injected = 0;
        for (int k = 0; k < 25; ++k)
        {
            const size_t p = (size_t) ((1.5 + 5.5 * k / 24.0) * fs);
            if (p < 1200 || p + 4800 >= doctored.size()) continue;
            double s = 0.0;
            for (int i = -1200; i < 1200; i += 4)
                s += (double) doctored[p + (size_t) i] * doctored[p + (size_t) i];
            const float amp = (float) (3.0 * std::sqrt (s / 600.0));
            for (int i = 0; i < 24; ++i)
                doctored[p + (size_t) i] += amp * (1.0f - (float) i / 24.0f);
            ++injected;
        }
        const auto eD = hfEnvDb (doctored, fs, hop);
        const int found = hfExcessEvents (eD, eA, hop, fs) - clean;

        std::snprintf (msg, sizeof (msg),
                       "HF-excess control: %d of %d injected steps found", found, injected);
        check (found >= injected / 2, msg);

        std::snprintf (msg, sizeof (msg),
                       "HF excess (>6 kHz, >%.0f dB over Antares +/-%.0f ms): %d events, %.2f/s (ceiling %.2f/s)",
                       kHfxThreshDb, kHfxTolMs, clean, clean / dur, kHfxMaxPerSec);
        check (clean / dur <= kHfxMaxPerSec, msg);
    }

    // ---- pitch excursions, with the instrument's own positive control ----
    {
        PitchEngine hp; hp.prepare (fs, 8192);
        const double hopSec = (double) hp.inputHopLength (PitchEngine::kLowMale) / fs;

        const int excOurs = excursionEvents (to, hopSec);
        const int excAnt  = excursionEvents (ta, hopSec);
        const int excDry  = excursionEvents (td, hopSec);

        // Control: 10 x 60 ms octave-up patches (2x-speed resample of the
        // following audio, 3 ms crossfades) spliced into a COPY of our
        // render. The tracker must read them as excursions or the zero above
        // is worth nothing.
        std::vector<float> doctored = ours;
        int injected = 0;
        const int segLen = (int) (0.060 * fs), xf = (int) (0.003 * fs);
        for (int k = 0; k < 10; ++k)
        {
            const size_t p = (size_t) ((1.6 + 5.2 * k / 9.0) * fs);
            if (p + (size_t) (2 * segLen) + 8 >= doctored.size()) continue;
            for (int i = 0; i < segLen; ++i)
            {
                const float v = ours[p + (size_t) (2 * i)];
                float w = 1.0f;
                if (i < xf)              w = (float) i / (float) xf;
                if (i >= segLen - xf)    w = (float) (segLen - 1 - i) / (float) xf;
                doctored[p + (size_t) i] = doctored[p + (size_t) i] * (1.0f - w) + v * w;
            }
            ++injected;
        }
        const Track tdoc = trackFile (doctored, fs);
        const int excDoc = excursionEvents (tdoc, hopSec);
        const int found  = excDoc - excOurs;

        std::snprintf (msg, sizeof (msg),
                       "excursion control: %d of %d injected octave patches found", found, injected);
        check (found >= injected / 2, msg);

        std::snprintf (msg, sizeof (msg),
                       "pitch excursions (>%.0fc from local median, back within %.0f ms): "
                       "ours %d vs Antares %d, dry %d (margin %d)",
                       kExcCents, kExcMaxMs, excOurs, excAnt, excDry, kExcMargin);
        check (excOurs <= excAnt + kExcMargin, msg);
    }

    // ---- clicks, Antares-anchored, with the shared injection control ------
    {
        const int clkOurs = clickEvents (ours, dry, fs);
        const int clkAnt  = clickEvents (antares, dry, fs);

        // Control: the same 25-step doctored copy the HF metric uses -
        // injected steps are CREATED content, which the drift-aware
        // exclusion cannot excuse by construction.
        std::vector<float> doctored = ours;
        int injected = 0;
        for (int k = 0; k < 25; ++k)
        {
            const size_t p = (size_t) ((1.5 + 5.5 * k / 24.0) * fs);
            if (p < 1200 || p + 4800 >= doctored.size()) continue;
            double s = 0.0;
            for (int i = -1200; i < 1200; i += 4)
                s += (double) doctored[p + (size_t) i] * doctored[p + (size_t) i];
            const float amp = (float) (3.0 * std::sqrt (s / 600.0));
            for (int i = 0; i < 24; ++i)
                doctored[p + (size_t) i] += amp * (1.0f - (float) i / 24.0f);
            ++injected;
        }
        const int found = clickEvents (doctored, dry, fs) - clkOurs;
        std::snprintf (msg, sizeof (msg),
                       "click control: %d of %d injected steps found", found, injected);
        check (found >= injected / 2, msg);

        std::snprintf (msg, sizeof (msg),
                       "clicks (pred-err 20x, drift-aware dry exclusion): ours %d vs Antares %d (margin 1)",
                       clkOurs, clkAnt);
        check (clkOurs <= clkAnt + 1, msg);
    }

    // ---- preserve == off inside the band: the PROVABLE equivalence --------
    // Inside +/-2.5 st the splice-resampler moves formants with the ratio,
    // which IS off's semantics - so at correction-sized shifts the two modes
    // are the same path doing the same thing, and any divergence is a
    // regression (the one Sean heard twice: off silently running a rougher
    // synthesis at settings where it should change nothing). This check
    // renders both and asserts the six metrics agree within tight
    // tolerances; the POSITIVE CONTROL forces off onto its grain path (the
    // old defect, via debugDisableSplice) and requires the comparator to
    // catch the divergence - an equivalence check that cannot detect
    // inequivalence proves nothing.
    {
        const std::vector<float> offOurs = renderEchoJay (dry, fs,
                                                          PsolaEngine::kFormantOff);
        double maxDiff = 0.0;
        size_t nDiff = 0;
        for (size_t i = 0; i < ours.size() && i < offOurs.size(); ++i)
        {
            const double d = std::fabs ((double) ours[i] - offOurs[i]);
            maxDiff = std::max (maxDiff, d);
            if (d > 1e-4) ++nDiff;
        }
        std::printf ("  preserve vs off in-band: max sample diff %.6f, %zu samples differ > 1e-4\n",
                     maxDiff, nDiff);

        auto sixOf = [&] (const std::vector<float>& x)
        {
            const Track t = trackFile (x, fs);
            Metrics m = measure (x, t, fs, sel, hop, dryFlux);
            struct Six { double cents, hnr, flux; int hfx, exc, clk; };
            Six s;
            s.cents = m.medCents; s.hnr = m.medHnr; s.flux = m.fluxPct;
            int hEnv = 0;
            const auto eX = hfEnvDb (x, fs, hEnv);
            const auto eA = hfEnvDb (antares, fs, hEnv);
            s.hfx = hfExcessEvents (eX, eA, hEnv, fs);
            PitchEngine hp2; hp2.prepare (fs, 8192);
            s.exc = excursionEvents (t, (double) hp2.inputHopLength (PitchEngine::kLowMale) / fs);
            s.clk = clickEvents (x, dry, fs);
            return s;
        };
        const auto sp = sixOf (ours);
        const auto so = sixOf (offOurs);
        auto agrees = [] (const decltype (sp)& a, const decltype (sp)& b)
        {
            return std::fabs (a.cents - b.cents) <= 0.3
                && std::fabs (a.hnr   - b.hnr)   <= 0.10
                && std::fabs (a.flux  - b.flux)  <= 0.5
                && std::abs (a.hfx - b.hfx) <= 1
                && std::abs (a.exc - b.exc) <= 1
                && std::abs (a.clk - b.clk) <= 1;
        };
        std::snprintf (msg, sizeof (msg),
                       "preserve == off in-band across the six metrics "
                       "(cents %.1f/%.1f  HNR %.2f/%.2f  flux %.1f/%.1f  hfx %d/%d  exc %d/%d  clk %d/%d)",
                       sp.cents, so.cents, sp.hnr, so.hnr, sp.flux, so.flux,
                       sp.hfx, so.hfx, sp.exc, so.exc, sp.clk, so.clk);
        check (agrees (sp, so), msg);

        // Control: off forced onto its grain path must DIVERGE.
        const std::vector<float> offForced = renderEchoJay (dry, fs,
                                                            PsolaEngine::kFormantOff,
                                                            0.0f, /*disableSplice*/ true);
        const auto sf = sixOf (offForced);
        std::snprintf (msg, sizeof (msg),
                       "equivalence control: forced-divergent off is CAUGHT "
                       "(HNR %.2f vs %.2f, flux %.1f vs %.1f)",
                       sf.hnr, sp.hnr, sf.flux, sp.flux);
        check (! agrees (sp, sf), msg);
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
