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
// humanize 0, natural_vibrato 0, targeting_ignores_vibrato off. NOT the
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
static std::vector<float> renderEchoJay (const std::vector<float>& in, double fs)
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
    corr.setIgnoreVibrato (false);
    corr.setNaturalVibrato (0.0f);
    corr.reset();

    PitchEngine det2;
    det2.prepare (fs, 256);
    det2.setVoiceType (vt);
    det2.setTracking (PitchEngine::kNormal);

    float worst = PitchEngine::voiceRange (0).fMinHz;
    for (int t = 1; t < PitchEngine::kNumVoiceTypes; ++t)
        worst = std::min (worst, PitchEngine::voiceRange (t).fMinHz);

    PsolaEngine sh;
    sh.prepare (fs, 256, PitchEngine::voiceRange (vt).fMinHz, worst);
    sh.setFormantMode (PsolaEngine::kFormantPreserve);
    sh.setPitchLagSamples (det2.pitchLagFor (vt));
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
                            sliceEnd - cursor, sliceF0, sliceVoiced, target);
                cursor = sliceEnd;
            }
            if (h < nHops)
            {
                sliceF0 = ev[h].f0Hz; sliceVoiced = ev[h].voiced;
                const float t = corr.process (ev[h].f0Hz, ev[h].voiced, hopMs);
                if (t > 0.0f) target = t;              // hold through gaps
            }
        }
        if (cursor < 256)
            sh.process (in.data() + p + (size_t) cursor,
                        out.data() + p + (size_t) cursor,
                        256 - cursor, sliceF0, sliceVoiced, target);
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
    if (! readWavMono ((dir + "/dry.wav").c_str(), dry, fs)
        || ! readWavMono ((dir + "/antares.wav").c_str(), antares, fs3))
    { std::printf ("cannot read dry.wav/antares.wav in %s\n", dir.c_str()); return 1; }
    const bool haveBounce = readWavMono ((dir + "/echojay.wav").c_str(), bounce, fs2);

    std::printf ("material: %s (%.0f Hz, %.1f s)\n", dir.c_str(), fs, (double) dry.size() / fs);

    // The gated column: rendered HERE from the current engine.
    const std::vector<float> ours = renderEchoJay (dry, fs);

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

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
