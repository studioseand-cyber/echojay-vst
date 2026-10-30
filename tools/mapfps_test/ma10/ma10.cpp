// MA10 (C4 section 10): run C4 section 6's six-band crest on real stems, and
// record the figures before the 1.0 dB margin leaves PROPOSED.
//
// PROVISIONAL. The hop series comes from ma10_hops.h, a reproduction of
// MeterEngine's perOct path, because A's accumulator (C2 section 5) does not
// exist yet. Rerun on A's accumulator after k-base-1.
//
// THE STEMS ARE READ ONLY. They live in Dropbox and sync, so this program opens
// them with "rb" and never anything else, and it refuses an output path that is
// not under the work folder it is given. Every file it writes goes there.
//
// SECTION 10 (28 Oct): the same runs through crest variants V1 and V2
// (ma10_variants.h) beside V0, plus a second, faster compressor, DRUMS_COMP_FAST.
//
// usage: ma10 <stem folder> <work folder>
//   <stem folder> holds BEAT/MUSIC.wav, BEAT/808.wav, BEAT/KICK.wav,
//   BEAT/SN.wav, BEAT/TOMS.wav and VOX/LEAD.wav.

#include <JuceHeader.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "EJMatchWindow.h"
#include "ma10_hops.h"
#include "ma10_variants.h"

namespace
{

struct Stereo
{
    double sr = 0.0;
    std::vector<float> l, r;
    size_t frames() const { return l.size(); }
};

std::uint32_t rd32 (const unsigned char* p) { return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24); }
std::uint16_t rd16 (const unsigned char* p) { return (std::uint16_t) (p[0] | (p[1] << 8)); }

// A 32-bit float stereo WAV, read with "rb". Anything else is refused by name.
bool readWav (const std::string& path, Stereo& out, std::string& why)
{
    FILE* f = std::fopen (path.c_str(), "rb");
    if (! f) { why = "cannot open"; return false; }
    std::vector<unsigned char> all;
    std::fseek (f, 0, SEEK_END);
    const long size = std::ftell (f);
    std::fseek (f, 0, SEEK_SET);
    all.resize ((size_t) size);
    const size_t got = std::fread (all.data(), 1, all.size(), f);
    std::fclose (f);
    if (got != all.size() || size < 12 || std::memcmp (all.data(), "RIFF", 4) || std::memcmp (all.data() + 8, "WAVE", 4))
    { why = "not a RIFF WAVE"; return false; }

    int tag = 0, ch = 0, bits = 0, align = 0; std::uint32_t sr = 0;
    size_t dataOff = 0, dataLen = 0;
    for (size_t p = 12; p + 8 <= all.size();)
    {
        const std::uint32_t len = rd32 (&all[p + 4]);
        if (! std::memcmp (&all[p], "fmt ", 4))
        {
            tag = rd16 (&all[p + 8]); ch = rd16 (&all[p + 10]); sr = rd32 (&all[p + 12]);
            align = rd16 (&all[p + 20]); bits = rd16 (&all[p + 22]);
            if (tag == 0xFFFE && len >= 40) tag = rd16 (&all[p + 8 + 24]);
        }
        else if (! std::memcmp (&all[p], "data", 4)) { dataOff = p + 8; dataLen = len; }
        p += 8 + len + (len & 1);
    }
    if (tag != 3 || bits != 32 || ch != 2 || align != 8 || dataOff == 0)
    { why = "not 32-bit float stereo"; return false; }
    const size_t n = dataLen / 8;
    out.sr = sr; out.l.resize (n); out.r.resize (n);
    const float* s = reinterpret_cast<const float*> (&all[dataOff]);
    for (size_t i = 0; i < n; ++i) { out.l[i] = s[2 * i]; out.r[i] = s[2 * i + 1]; }
    return true;
}

bool under (const std::string& path, const std::string& dir)
{
    return path.size() > dir.size() && path.compare (0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

// A 32-bit float stereo WAV into the work folder, nowhere else.
bool writeWav (const std::string& path, const std::string& workDir, const Stereo& s)
{
    if (! under (path, workDir)) { std::fprintf (stderr, "refused: %s is outside %s\n", path.c_str(), workDir.c_str()); return false; }
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return false;
    const std::uint32_t n = (std::uint32_t) s.frames(), dataLen = n * 8;
    auto w32 = [f] (std::uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [f] (std::uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataLen); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (3); w16 (2); w32 ((std::uint32_t) s.sr);
    w32 ((std::uint32_t) s.sr * 8); w16 (8); w16 (32);
    std::fwrite ("data", 1, 4, f); w32 (dataLen);
    for (std::uint32_t i = 0; i < n; ++i) { std::fwrite (&s.l[i], 4, 1, f); std::fwrite (&s.r[i], 4, 1, f); }
    std::fclose (f);
    return true;
}

double peakOf (const Stereo& s)
{
    double p = 0.0;
    for (size_t i = 0; i < s.frames(); ++i) p = std::max (p, (double) std::max (std::abs (s.l[i]), std::abs (s.r[i])));
    return p;
}

Stereo scaled (const Stereo& s, double g)
{
    Stereo o = s;
    for (size_t i = 0; i < o.frames(); ++i) { o.l[i] = (float) (o.l[i] * g); o.r[i] = (float) (o.r[i] * g); }
    return o;
}

struct Run
{
    std::string name;
    size_t frames = 0;
    std::vector<ma10::Hop> hops;
    echojay::BandCrest crest;
};

Run runCrest (const std::string& name, const Stereo& s, int block)
{
    Run r;
    r.name = name;
    r.frames = s.frames();
    r.hops = ma10::hopsFor (s.l, s.r, s.sr, block);
    r.crest = echojay::bandCrest (r.hops);
    return r;
}

std::string fmtCrest (const std::optional<float>& v)
{
    if (! v) return "null";
    char b[32]; std::snprintf (b, sizeof b, "%.2f", *v); return b;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc != 3) { std::fprintf (stderr, "usage: ma10 <stem folder> <work folder>\n"); return 2; }
    const std::string S = argv[1], W = argv[2];
    const char* bands[6] = { "sub", "low", "lowMid", "mid", "highMid", "air" };

    struct Src { const char* key; std::string path; Stereo s; };
    std::vector<Src> src = {
        { "DENSE", S + "/BEAT/MUSIC.wav", {} }, { "BASS",  S + "/BEAT/808.wav", {} },
        { "KICK",  S + "/BEAT/KICK.wav", {} },  { "SNARE", S + "/BEAT/SN.wav", {} },
        { "TOMS",  S + "/BEAT/TOMS.wav", {} },  { "VOCAL", S + "/VOX/LEAD.wav", {} } };
    for (auto& x : src)
    {
        std::string why;
        if (! readWav (x.path, x.s, why)) { std::printf ("STOP: %s: %s\n", x.path.c_str(), why.c_str()); return 1; }
        std::printf ("read %-6s sr=%.0f frames=%zu (%.4f s)  %s\n", x.key, x.s.sr, x.s.frames(),
                     x.s.frames() / x.s.sr, x.path.c_str());
    }
    const Stereo& kick = src[2].s; const Stereo& sn = src[3].s; const Stereo& toms = src[4].s;
    if (kick.sr != sn.sr || kick.sr != toms.sr || kick.frames() != sn.frames() || kick.frames() != toms.frames())
    { std::printf ("STOP: the drum stems differ in sample rate or length\n"); return 1; }
    const double sr = kick.sr;

    // 1. DRUMS: unity sum in float, no normalising.
    Stereo drums; drums.sr = sr; drums.l.resize (kick.frames()); drums.r.resize (kick.frames());
    for (size_t i = 0; i < kick.frames(); ++i)
    {
        drums.l[i] = kick.l[i] + sn.l[i] + toms.l[i];
        drums.r[i] = kick.r[i] + sn.r[i] + toms.r[i];
    }
    const double pk = peakOf (drums);
    std::printf ("DRUMS sample peak %.6f (%.2f dBFS)\n", pk, 20.0 * std::log10 (pk));
    double drumsGainDb = 0.0;
    if (pk > 1.0)
    {
        // One stated gain: the smallest whole dB that brings the peak under 0 dBFS.
        drumsGainDb = -std::ceil (20.0 * std::log10 (pk));
        drums = scaled (drums, std::pow (10.0, drumsGainDb / 20.0));
        std::printf ("DRUMS scaled by %.1f dB, peak now %.6f\n", drumsGainDb, peakOf (drums));
    }
    else std::printf ("DRUMS not scaled (peak under 0 dBFS)\n");

    // The DRUMS hop series at 512, for the silence flags the compressor's
    // statistics are read on.
    const Run rDrums = runCrest ("DRUMS", drums, 512);
    const int hopLen = (int) (sr * 0.1);

    // 2. DRUMS_COMP and DRUMS_COMP_FAST: feed-forward, stereo-linked peak
    // detector, 4:1, no makeup, no lookahead. The detector reads the INPUT, so
    // the gain reduction is a function of the threshold alone: the threshold is
    // solved for a 6 dB mean on non-silent hops, then rounded to 0.1 dB.
    // DRUMS_COMP is attack 5 ms, release 100 ms; DRUMS_COMP_FAST is 0.5 / 50.
    auto meanOf = [] (const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / v.size(); };
    auto compress = [&] (const char* name, double attMs, double relMs)
    {
        const double ratio = 4.0;
        const double aA = std::exp (-1.0 / (attMs * 0.001 * sr)), aR = std::exp (-1.0 / (relMs * 0.001 * sr));
        std::vector<float> envDb (drums.frames());
        {
            double env = 0.0;
            for (size_t i = 0; i < drums.frames(); ++i)
            {
                const double x = std::max (std::abs (drums.l[i]), std::abs (drums.r[i]));
                env = x > env ? aA * env + (1.0 - aA) * x : aR * env + (1.0 - aR) * x;
                envDb[i] = env > 1e-12 ? (float) (20.0 * std::log10 (env)) : -240.0f;
            }
        }
        auto hopGr = [&] (double T)
        {
            std::vector<double> v;
            for (size_t h = 0; h < rDrums.hops.size(); ++h)
            {
                if (rDrums.hops[h].flags & echojay::kHopFlagSilent) continue;
                const size_t a = h * (size_t) hopLen, e = std::min (drums.frames(), a + (size_t) hopLen);
                double s = 0.0;
                for (size_t i = a; i < e; ++i) s += std::max (0.0, (double) envDb[i] - T) * (1.0 - 1.0 / ratio);
                v.push_back (s / (double) (e - a));
            }
            return v;
        };
        double lo = -80.0, hi = 0.0;
        for (int it = 0; it < 60; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            if (meanOf (hopGr (mid)) > 6.0) lo = mid; else hi = mid;
        }
        const double T = std::round (0.5 * (lo + hi) * 10.0) / 10.0;
        auto gr = hopGr (T);
        const double grMean = meanOf (gr);
        std::sort (gr.begin(), gr.end());
        const double grP95 = gr.empty() ? 0.0 : gr[echojay::nearestRankIndex (gr.size(), 95)];
        std::printf ("%s threshold %.1f dBFS, ratio %.0f:1, attack %.1f ms, release %.0f ms; GR on %zu non-silent hops: "
                     "mean %.2f dB, p95 %.2f dB\n", name, T, ratio, attMs, relMs, gr.size(), grMean, grP95);
        Stereo c = drums;
        for (size_t i = 0; i < c.frames(); ++i)
        {
            const double g = std::pow (10.0, -std::max (0.0, (double) envDb[i] - T) * (1.0 - 1.0 / ratio) / 20.0);
            c.l[i] = (float) (c.l[i] * g); c.r[i] = (float) (c.r[i] * g);
        }
        return c;
    };
    const Stereo comp     = compress ("COMP", 5.0, 100.0);
    const Stereo compFast = compress ("COMP_FAST", 0.5, 50.0);

    // 3. Noise checks.
    const Stereo m6 = scaled (drums, std::pow (10.0, -6.0 / 20.0));
    Stereo fade = drums;
    {
        const size_t n = fade.frames(), f3 = (size_t) (3.0 * sr);
        for (size_t i = 0; i < f3 && i < n; ++i)
        {
            const float gIn  = (float) ((double) i / (double) f3);
            fade.l[i] *= gIn; fade.r[i] *= gIn;
            const size_t j = n - 1 - i;
            const float gOut = (float) ((double) i / (double) f3);
            fade.l[j] *= gOut; fade.r[j] *= gOut;
        }
    }

    writeWav (W + "/DRUMS.wav", W, drums);
    writeWav (W + "/DRUMS_COMP.wav", W, comp);
    writeWav (W + "/DRUMS_COMP_FAST.wav", W, compFast);
    writeWav (W + "/DRUMS_minus6.wav", W, m6);
    writeWav (W + "/DRUMS_fade3s.wav", W, fade);

    std::vector<Run> runs;
    runs.push_back (runCrest ("DENSE", src[0].s, 512));
    runs.push_back (runCrest ("BASS",  src[1].s, 512));
    runs.push_back (runCrest ("VOCAL", src[5].s, 512));
    runs.push_back (rDrums);
    runs.push_back (runCrest ("DRUMS_COMP", comp, 512));
    runs.push_back (runCrest ("DRUMS_minus6", m6, 512));
    runs.push_back (runCrest ("DRUMS_fade3s", fade, 512));
    // Supplementary: the block-size claim of C4 section 6, on real drums.
    runs.push_back (runCrest ("DRUMS_block128", drums, 128));
    runs.push_back (runCrest ("DRUMS_block2048", drums, 2048));

    std::printf ("\nRUN | frames | seconds | hops | qualifying | crest dB sub low lowMid mid highMid air | ratio hops per band\n");
    for (const auto& r : runs)
    {
        std::printf ("%s | %zu | %.3f | %zu | %d |", r.name.c_str(), r.frames, r.frames / sr, r.hops.size(), r.crest.qualifyingHops);
        for (int b = 0; b < 6; ++b) std::printf (" %s", fmtCrest (r.crest.db[(size_t) b]).c_str());
        std::printf (" |");
        for (int b = 0; b < 6; ++b) std::printf (" %d", r.crest.ratioHops[(size_t) b]);
        std::printf ("\n");
    }

    auto diff = [&] (const char* label, const Run& a, const Run& b)
    {
        std::printf ("DIFF %s |", label);
        bool ok = true;
        for (int b2 = 0; b2 < 6; ++b2)
        {
            const auto& x = a.crest.db[(size_t) b2]; const auto& y = b.crest.db[(size_t) b2];
            if (x && y) std::printf (" %s %+.4f", bands[b2], *x - *y);
            else { std::printf (" %s null", bands[b2]); ok = false; }
        }
        std::printf ("\n");
        return ok;
    };
    std::printf ("\n");
    diff ("DRUMS minus DRUMS_COMP", runs[3], runs[4]);
    diff ("DRUMS minus DRUMS_minus6", runs[3], runs[5]);
    diff ("DRUMS minus DRUMS_fade3s", runs[3], runs[6]);
    diff ("DRUMS(512) minus DRUMS(128)", runs[3], runs[7]);
    diff ("DRUMS(512) minus DRUMS(2048)", runs[3], runs[8]);

    // The -6 dB check, by hop: which hops changed their qualifying state.
    int flipped = 0;
    for (size_t h = 0; h < std::min (runs[3].hops.size(), runs[5].hops.size()); ++h)
        if (echojay::crestHopQualifies (runs[3].hops[h]) != echojay::crestHopQualifies (runs[5].hops[h])) ++flipped;
    std::printf ("hops whose qualifying state differs between DRUMS and DRUMS_minus6: %d\n", flipped);
    std::printf ("drums gain applied: %.1f dB\n", drumsGainDb);

    // ---- Section 10: V0, V1 and V2 side by side ----
    // Every variant reads the same ten signals. V0 is bandCrest on ma10_hops.h's
    // series; V1 and V2 are peakMeanCrest on the 256-sample cadence.
    struct Sig { const char* name; const Stereo* s; int block; };
    const std::vector<Sig> sigs = {
        { "DRUMS", &drums, 512 }, { "DRUMS_COMP", &comp, 512 }, { "DRUMS_COMP_FAST", &compFast, 512 },
        { "DRUMS_minus6", &m6, 512 }, { "DRUMS_fade3s", &fade, 512 },
        { "DENSE", &src[0].s, 512 }, { "BASS", &src[1].s, 512 }, { "VOCAL", &src[5].s, 512 },
        { "DRUMS_block128", &drums, 128 }, { "DRUMS_block2048", &drums, 2048 } };
    enum { iD, iC, iF, iM6, iFade, iDense, iBass, iVocal, i128, i2048 };
    using Fig = std::array<std::optional<float>, 6>;
    bool stop = false;
    for (int v = 0; v <= 2; ++v)
    {
        std::vector<Fig> fig; std::vector<int> qual;
        for (const auto& g : sigs)
        {
            if (v == 0)
            {
                const auto c = echojay::bandCrest (ma10::hopsFor (g.s->l, g.s->r, sr, g.block));
                fig.push_back (c.db); qual.push_back (c.qualifyingHops);
            }
            else
            {
                const auto c = ma10::peakMeanCrest (ma10::cadenceHopsFor (g.s->l, g.s->r, sr, g.block, v == 2));
                fig.push_back (c.db); qual.push_back (c.qualifyingHops);
            }
        }
        std::printf ("\nV%d RUN | qualifying | sub low lowMid mid highMid air\n", v);
        for (size_t k = 0; k < sigs.size(); ++k)
        {
            std::printf ("V%d %s | %d |", v, sigs[k].name, qual[k]);
            for (int b = 0; b < 6; ++b) std::printf (" %s", fmtCrest (fig[k][(size_t) b]).c_str());
            std::printf ("\n");
        }
        auto row = [&] (const char* label, auto f)
        {
            std::printf ("V%d %s |", v, label);
            for (int b = 0; b < 6; ++b)
            {
                const auto x = f ((size_t) b);
                if (x) std::printf (" %s %+.4f", bands[b], *x); else std::printf (" %s null", bands[b]);
            }
            std::printf ("\n");
        };
        auto d = [&] (int a, int c) { return [&fig, a, c] (size_t b) -> std::optional<double> {
            if (! fig[(size_t) a][b] || ! fig[(size_t) c][b]) return std::nullopt;
            return (double) *fig[(size_t) a][b] - (double) *fig[(size_t) c][b]; }; };
        row ("SIGNAL DRUMS minus DRUMS_COMP", d (iD, iC));
        row ("SIGNAL DRUMS minus DRUMS_COMP_FAST", d (iD, iF));
        row ("NOISE max |diff| among blocks 128 512 2048", [&] (size_t b) -> std::optional<double> {
            const int ix[3] = { i128, iD, i2048 };
            double m = 0.0;
            for (int p = 0; p < 3; ++p) for (int q = p + 1; q < 3; ++q)
            {
                const auto& x = fig[(size_t) ix[p]][b]; const auto& y = fig[(size_t) ix[q]][b];
                if (! x || ! y) return std::nullopt;
                m = std::max (m, std::abs ((double) *x - (double) *y));
            }
            return m; });
        row ("DRUMS minus DRUMS_minus6", d (iD, iM6));
        row ("DRUMS minus DRUMS_fade3s", d (iD, iFade));
        for (size_t b = 0; b < 6; ++b)
        {
            const auto x = d (iD, iM6) (b);
            if (! x || std::abs (*x) >= 0.00005) stop = true;
        }
    }
    if (stop) { std::printf ("STOP: DRUMS minus DRUMS at -6 dB is not 0.0 in some variant\n"); return 1; }

    return 0;
}
