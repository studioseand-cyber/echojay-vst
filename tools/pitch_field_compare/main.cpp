// pitch_field_compare — the FIELD ruler (29 Aug 2026 ruling): EchoJay
// against the Antares bounce at the same instants on the standing NEW
// reference set. Reproduces the reviewer's span measurement in-house so
// both sides of a change are measured with the SAME ruler:
//   - rough spans vs Antares: windows where EchoJay's cycle similarity
//     drops > 0.10 below Antares's at the same instant, merged into spans
//     (count + duration) — the binding field number
//   - inversions (EchoJay similarity < 0): must be zero
//   - tuning: in-scale %, same-semitone % vs Antares, improve-rate vs the
//     source, median off-grid cents (D minor)
//
// RULER CALIBRATION (measured 2026-08-29 on the standing NEW set - read
// this before quoting any absolute number from this tool):
//   Antares reads improve-rate 62.4% / median off-grid 5.1c here.
//   EchoJay at HARD (retune 0/flex 0/hum 0) reads 63.5% / 4.1c;
//   EchoJay at NATURAL (retune 120/flex 55/hum 60) reads 49.4% / 6.7c.
//   Sean's NEW bounces read 61.7% / 4.4c - they are HARD bounces (the
//   2026-08-29 settings audit: tuning AND rough-span columns match the
//   hard render, not the natural one). An earlier header here blamed the
//   offline preset table for the 61.7-vs-49.4 gap; that was WRONG - the
//   gap is hard-vs-natural, and natural legitimately improves less
//   because retune 120ms leaves the performance's own deviation in.
// Therefore: quote tuning numbers ONLY against the operating point of the
// render, and compare files ONLY at matched settings. The rough-span
// column scales with operating point too (hard ~84-94 spans vs natural
// ~51-56 on the same take, same engine) - a cross-operating-point
// comparison measures the preset, not the build.
//
// Build: g++ -std=c++17 -O2 -ISource tools/pitch_field_compare/main.cpp -o fieldcmp
// Run:   ./fieldcmp <source.wav> <antares.wav> <echojay.wav>

#include "EedPitchEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace echojay;

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

// Cycle similarity at a, best over lags around the seed period: NCC of
// x[a..a+L) vs x[a+L..a+2L). Each signal is scored at its OWN best lag so
// a corrected pitch is not charged for differing from the source's period.
static double cycleSim (const std::vector<float>& x, long a, int Tseed)
{
    double best = -2.0;
    const int lo = std::max (8, (int) (0.8 * Tseed)), hi = (int) (1.2 * Tseed);
    for (int L = lo; L <= hi; ++L)
    {
        if (a < 0 || a + 2 * (long) L >= (long) x.size()) continue;
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < L; ++i)
        {
            const double xa = x[(size_t) (a + i)];
            const double xb = x[(size_t) (a + L + i)];
            sab += xa * xb; saa += xa * xa; sbb += xb * xb;
        }
        if (saa > 1e-12 && sbb > 1e-12)
            best = std::max (best, sab / std::sqrt (saa * sbb));
    }
    return best;
}

struct Track { std::vector<float> f0, conf; int hop = 0; };

static Track trackFile (const std::vector<float>& x, double fs)
{
    PitchEngine e;
    e.prepare (fs, 8192);
    e.setVoiceType (PitchEngine::kLowMale);
    e.setTracking (PitchEngine::kNormal);
    Track t;
    t.hop = e.inputHopLength (PitchEngine::kLowMale);
    for (size_t p = 0; p + (size_t) t.hop <= x.size(); p += (size_t) t.hop)
    {
        e.process (x.data() + p, nullptr, t.hop);
        const PitchReading r = e.getReading();
        t.f0.push_back (r.voiced ? r.f0Hz : 0.0f);
        t.conf.push_back (r.confidence);
    }
    std::vector<float> f = t.f0;
    for (size_t i = 2; i + 2 < f.size(); ++i)
    {
        float v[5]; int n = 0;
        for (int k = -2; k <= 2; ++k) if (f[i + (size_t) k] > 0.0f) v[n++] = f[i + (size_t) k];
        if (n >= 3 && f[i] > 0.0f) { std::sort (v, v + n); t.f0[i] = v[n / 2]; }
    }
    return t;
}

// Cents to the nearest D-minor scale tone (root 2: D E F G A Bb C).
static double offGrid (double f0)
{
    static const int pcs[7] = { 2, 4, 5, 7, 9, 10, 0 };
    const double midi = 69.0 + 12.0 * std::log2 (f0 / 440.0);
    double best = 1e9;
    for (int k = -1; k <= 1; ++k)
        for (int j = 0; j < 7; ++j)
        {
            const double note = 12.0 * std::floor (midi / 12.0 + k) + pcs[j];
            best = std::min (best, std::fabs (midi - note) * 100.0);
        }
    return best;
}

int main (int argc, char** argv)
{
    if (argc < 4) { std::printf ("usage: fieldcmp <source> <antares> <echojay>\n"); return 1; }
    std::vector<float> src, ant, ej; double fs = 0, fs2 = 0, fs3 = 0;
    if (! readWavMono (argv[1], src, fs) || ! readWavMono (argv[2], ant, fs2)
        || ! readWavMono (argv[3], ej, fs3))
    { std::printf ("cannot read inputs\n"); return 1; }

    const Track ts = trackFile (src, fs), ta = trackFile (ant, fs),
                te = trackFile (ej, fs);

    // ---- rough spans vs Antares -------------------------------------------
    int wins = 0, deficit = 0, spans = 0, inversions = 0;
    bool inSpan = false;
    double deficitS = 0, worst = 0; double worstAt = 0;
    std::vector<double> defTimes;
    const long lead = (long) (0.3 * fs);
    for (size_t h = 0; h < ts.f0.size(); ++h)
    {
        const float f0 = ts.f0[h];
        if (f0 <= 0.0f) { inSpan = false; continue; }
        const long a = (long) h * ts.hop;
        if (a < lead || a > (long) src.size() - lead) { inSpan = false; continue; }
        const int T = (int) std::lround (fs / f0);
        const double sS = cycleSim (src, a, T);
        if (sS < 0.5) { inSpan = false; continue; }     // source aperiodic
        const double sA = cycleSim (ant, a, T);
        const double sE = cycleSim (ej,  a, T);
        if (sA < -1.5 || sE < -1.5) { inSpan = false; continue; }
        ++wins;
        if (sE < 0.0) ++inversions;
        const double d = sA - sE;
        if (d > 0.10)
        {
            ++deficit; deficitS += (double) ts.hop / fs;
            if (! inSpan) { ++spans; inSpan = true;
                if (getenv ("FIELD_SPANS")) std::printf ("    span @ %.2fs\n", a / fs); }
            if (d > worst) { worst = d; worstAt = a / fs; }
        }
        else inSpan = false;
    }

    // ---- tuning ------------------------------------------------------------
    int voiced = 0, inScale = 0, sameSemi = 0, semiBoth = 0, improve = 0, impBoth = 0;
    std::vector<double> ogE;
    for (size_t h = 0; h < ts.f0.size() && h < te.f0.size(); ++h)
    {
        const float fe = te.f0[h];
        if (fe <= 0.0f) continue;
        ++voiced;
        const double midiE = 69.0 + 12.0 * std::log2 ((double) fe / 440.0);
        static const int pcs[7] = { 2, 4, 5, 7, 9, 10, 0 };
        const int pc = ((int) std::lround (midiE) % 12 + 12) % 12;
        for (int j = 0; j < 7; ++j) if (pc == pcs[j]) { ++inScale; break; }
        ogE.push_back (offGrid (fe));
        if (h < ta.f0.size() && ta.f0[h] > 0.0f)
        {
            ++semiBoth;
            if (std::lround (midiE) == std::lround (69.0 + 12.0 * std::log2 ((double) ta.f0[h] / 440.0)))
                ++sameSemi;
        }
        if (ts.f0[h] > 0.0f)
        {
            ++impBoth;
            if (offGrid (fe) < offGrid (ts.f0[h])) ++improve;
        }
    }
    std::sort (ogE.begin(), ogE.end());

    // Signed STEADY-FRAME deviation from the nearest semitone (the
    // flat-bias metric, 29 Aug 2026): frames whose pitch moves <8c against
    // both neighbours. A healthy corrector centres this near 0; a constant
    // one-directional offset here is a tuning-reference error ("never
    // arrives at the note") that |off-grid| medians blur and slope-based
    // under-correction metrics miss entirely.
    auto steadyBias = [] (const Track& t) -> double
    {
        std::vector<double> dev;
        for (size_t h = 1; h + 1 < t.f0.size(); ++h)
        {
            const float f = t.f0[h];
            if (f <= 0.0f || t.f0[h - 1] <= 0.0f || t.f0[h + 1] <= 0.0f) continue;
            if (std::fabs (1200.0 * std::log2 ((double) f / t.f0[h - 1])) >= 8.0) continue;
            if (std::fabs (1200.0 * std::log2 ((double) t.f0[h + 1] / f)) >= 8.0) continue;
            const double midi = 69.0 + 12.0 * std::log2 ((double) f / 440.0);
            dev.push_back ((midi - std::round (midi)) * 100.0);
        }
        if (dev.empty()) return 0.0;
        std::sort (dev.begin(), dev.end());
        return dev[dev.size() / 2];
    };
    const double biasE = steadyBias (te), biasA = steadyBias (ta), biasS = steadyBias (ts);

    // CONVERGENCE, unsigned and per-note (29 Aug 2026 correction to the
    // bleed A/B: a signed steady-frame bias CANCELS an undershoot that
    // opposes the correction direction across up- vs down-corrected notes;
    // the unsigned per-note centre is the statistic that can see it).
    // A note = a voiced run whose nearest semitone holds, >= 80 ms. Its
    // centre is the median deviation from that semitone; the headline is
    // the median |centre| across notes, plus % of voiced frames within 3c
    // of the nearest semitone.
    auto convergence = [] (const Track& t, double& medCentre, double& pctWithin3)
    {
        std::vector<double> centres, run;
        int within = 0, voicedN = 0, runSemi = 1000;
        auto flush = [&] ()
        {
            if ((int) run.size() * 1 >= 16)   // >= ~80ms at ~5ms hops
            {
                std::vector<double> r = run;
                std::sort (r.begin(), r.end());
                centres.push_back (std::fabs (r[r.size() / 2]));
            }
            run.clear();
        };
        for (size_t h = 0; h < t.f0.size(); ++h)
        {
            const float f = t.f0[h];
            if (f <= 0.0f) { flush(); runSemi = 1000; continue; }
            const double midi = 69.0 + 12.0 * std::log2 ((double) f / 440.0);
            const int semi = (int) std::lround (midi);
            const double dev = (midi - semi) * 100.0;
            ++voicedN;
            if (std::fabs (dev) <= 3.0) ++within;
            if (semi != runSemi) { flush(); runSemi = semi; }
            run.push_back (dev);
        }
        flush();
        std::sort (centres.begin(), centres.end());
        medCentre  = centres.empty() ? 0.0 : centres[centres.size() / 2];
        pctWithin3 = voicedN > 0 ? 100.0 * within / voicedN : 0.0;
    };
    double ncE = 0, w3E = 0, ncA = 0, w3A = 0, ncS = 0, w3S = 0;
    convergence (te, ncE, w3E);
    convergence (ta, ncA, w3A);
    convergence (ts, ncS, w3S);

    std::printf ("%s vs %s\n", argv[3], argv[2]);
    std::printf ("  rough spans vs Antares (>0.10 deficit): %d spans / %.2fs "
                 "(%d of %d windows)  worst -%.2f at %.2fs\n",
                 spans, deficitS, deficit, wins, worst, worstAt);
    std::printf ("  inversions (EchoJay sim < 0): %d\n", inversions);
    std::printf ("  tuning: in-scale %.1f%%  same-semitone %.1f%%  "
                 "improve-rate %.1f%%  median off-grid %.1fc\n",
                 100.0 * inScale / std::max (1, voiced),
                 100.0 * sameSemi / std::max (1, semiBoth),
                 100.0 * improve / std::max (1, impBoth),
                 ogE.empty() ? 0.0 : ogE[ogE.size() / 2]);
    std::printf ("  steady-frame signed bias: echojay %+.2fc  antares %+.2fc  source %+.2fc\n",
                 biasE, biasA, biasS);
    std::printf ("  convergence: note-centre |med| echojay %.2fc antares %.2fc source %.2fc"
                 "   within-3c echojay %.1f%% antares %.1f%% source %.1f%%\n",
                 ncE, ncA, ncS, w3E, w3A, w3S);
    return 0;
}
