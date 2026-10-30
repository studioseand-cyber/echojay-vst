// MA10 section 11: THE DISCRIMINATOR TEST. Given a band that is hotter than the
// reference, is the excess peaky material or body? Same gap, two causes; this
// asks whether crest V0, V1 or V2 tells them apart.
//
// HARNESS ONLY, and provisional in the same way as ma10.cpp: the hop series
// comes from ma10_hops.h (V0) and ma10_variants.h (V1, V2), not A's
// accumulator.
//
// THE STEMS ARE READ ONLY. They live in Dropbox and sync, so this program opens
// them with "rb" and never anything else. It writes only its own stdout; the
// build script tees that into the work folder.
//
// MIX is all ten stems summed at unity in float, not normalised. REF is MIX.
// Each case is MIX + (g - 1) * GROUP, so at g = 1 it is MIX to the bit, and D is
// exactly 0 there. GROUP is the drums (KICK + SN + TOMS) for PEAKS, MUSIC for
// BODY_MUSIC and LEAD for BODY_VOCAL.
//
// bandsRel IS THE CODEBASE'S: per block, a 2048 Hann frame's perOct powers
// (MeterEngine.cpp:378-381, through ma10_variants.h's PerOctFrame<11>, the same
// arithmetic as ma10_hops.h), summed over the whole run with no silence gate
// (MeterEngine.cpp:386 and :402, BandAccum), turned into dB by echojay::bandMeanFromSum
// (EJBandScheme.h:167-176), then each band minus the six-band mean by
// matchBandRelatives (EJMatchProposal.h:325-335). D = bandsRel(case) minus
// bandsRel(ref), the order the section 11 brief gives.
//
// usage: ma10_discrim <stem folder>

#include <JuceHeader.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "EJMatchWindow.h"
#include "EJMatchProposal.h"
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

struct WavHeader { int tag = 0, ch = 0, bits = 0, align = 0; std::uint32_t sr = 0; size_t dataOff = 0, dataLen = 0; };

bool slurp (const std::string& path, std::vector<unsigned char>& all)
{
    FILE* f = std::fopen (path.c_str(), "rb");
    if (! f) return false;
    std::fseek (f, 0, SEEK_END);
    const long size = std::ftell (f);
    std::fseek (f, 0, SEEK_SET);
    all.resize ((size_t) size);
    const size_t got = std::fread (all.data(), 1, all.size(), f);
    std::fclose (f);
    return got == all.size();
}

bool parseHeader (const std::vector<unsigned char>& all, WavHeader& h)
{
    if (all.size() < 12 || std::memcmp (all.data(), "RIFF", 4) || std::memcmp (all.data() + 8, "WAVE", 4)) return false;
    for (size_t p = 12; p + 8 <= all.size();)
    {
        const std::uint32_t len = rd32 (&all[p + 4]);
        if (! std::memcmp (&all[p], "fmt ", 4))
        {
            h.tag = rd16 (&all[p + 8]); h.ch = rd16 (&all[p + 10]); h.sr = rd32 (&all[p + 12]);
            h.align = rd16 (&all[p + 20]); h.bits = rd16 (&all[p + 22]);
            if (h.tag == 0xFFFE && len >= 40) h.tag = rd16 (&all[p + 8 + 24]);
        }
        else if (! std::memcmp (&all[p], "data", 4)) { h.dataOff = p + 8; h.dataLen = len; break; }
        p += 8 + len + (len & 1);
    }
    return h.dataOff != 0 && h.dataOff + h.dataLen <= all.size();
}

// Stereo 32-bit float, or stereo 24-bit PCM (TAGS is 24-bit) scaled by 2^-23.
bool decode (const std::vector<unsigned char>& all, const WavHeader& h, Stereo& out)
{
    if (h.ch != 2) return false;
    out.sr = h.sr;
    if (h.tag == 3 && h.bits == 32 && h.align == 8)
    {
        const size_t n = h.dataLen / 8;
        out.l.resize (n); out.r.resize (n);
        const float* s = reinterpret_cast<const float*> (&all[h.dataOff]);
        for (size_t i = 0; i < n; ++i) { out.l[i] = s[2 * i]; out.r[i] = s[2 * i + 1]; }
        return true;
    }
    if (h.tag == 1 && h.bits == 24 && h.align == 6)
    {
        const size_t n = h.dataLen / 6;
        out.l.resize (n); out.r.resize (n);
        auto s24 = [] (const unsigned char* p) {
            std::int32_t v = (std::int32_t) ((std::uint32_t) p[0] << 8 | (std::uint32_t) p[1] << 16 | (std::uint32_t) p[2] << 24) >> 8;
            return (float) ((double) v / 8388608.0); };
        const unsigned char* d = &all[h.dataOff];
        for (size_t i = 0; i < n; ++i) { out.l[i] = s24 (d + 6 * i); out.r[i] = s24 (d + 6 * i + 3); }
        return true;
    }
    return false;
}

// The whole-run six bands in dB, as BandAccum and bandMeanFromSum give them.
std::array<float, 6> wholeRunBandsDb (const Stereo& s, int block)
{
    ma10::PerOctFrame<11> fr (s.sr);
    std::vector<float> ring (2048, 0.0f);
    int wp = 0, blocks = 0;
    std::array<double, 6> sum {};
    const size_t n = s.frames();
    for (size_t i = 0; i < n; i += (size_t) block)
    {
        const size_t e = std::min (n, i + (size_t) block);
        for (size_t k = i; k < e; ++k) { ring[(size_t) wp] = (s.l[k] + s.r[k]) * 0.5f; wp = (wp + 1) % 2048; }
        const auto p = fr.compute (ring.data(), 2048, wp);
        for (size_t b = 0; b < 6; ++b) sum[b] += p[b];
        ++blocks;
    }
    std::array<float, 6> db {};
    for (size_t b = 0; b < 6; ++b) db[b] = echojay::bandMeanFromSum (sum[b], blocks).db;
    return db;
}

bool bandsRel (const Stereo& s, int block, std::array<float, 6>& rel)
{
    return echojay::matchBandRelatives (wholeRunBandsDb (s, block), rel);
}

Stereo withGroup (const Stereo& mix, const Stereo& group, double g)
{
    Stereo o = mix;
    const float k = (float) (g - 1.0);
    if (k != 0.0f)
        for (size_t i = 0; i < o.frames(); ++i) { o.l[i] += k * group.l[i]; o.r[i] += k * group.r[i]; }
    return o;
}

using Fig = std::array<std::optional<float>, 6>;

Fig crestOf (int variant, const Stereo& s, int block)
{
    if (variant == 0) return echojay::bandCrest (ma10::hopsFor (s.l, s.r, s.sr, block)).db;
    return ma10::peakMeanCrest (ma10::cadenceHopsFor (s.l, s.r, s.sr, block, variant == 2)).db;
}

std::optional<double> delta (const Fig& a, const Fig& b, size_t band)
{
    if (! a[band] || ! b[band]) return std::nullopt;
    return (double) *a[band] - (double) *b[band];
}

std::string fmt (const std::optional<double>& v, const char* f = "%+.3f")
{
    if (! v) return "null";
    char b[32]; std::snprintf (b, sizeof b, f, *v); return b;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc != 2) { std::fprintf (stderr, "usage: ma10_discrim <stem folder>\n"); return 2; }
    const std::string S = argv[1];
    const char* bands[6] = { "sub", "low", "lowMid", "mid", "highMid", "air" };

    struct Stem { const char* key; std::string path; Stereo s; };
    std::vector<Stem> stems = {
        { "808", S + "/BEAT/808.wav", {} },       { "KICK", S + "/BEAT/KICK.wav", {} },
        { "MUSIC FX", S + "/BEAT/MUSIC FX.wav", {} }, { "MUSIC", S + "/BEAT/MUSIC.wav", {} },
        { "SN", S + "/BEAT/SN.wav", {} },         { "TAGS", S + "/BEAT/TAGS.wav", {} },
        { "TOMS", S + "/BEAT/TOMS.wav", {} },     { "ADLIBS", S + "/VOX/ADLIBS.wav", {} },
        { "LEAD", S + "/VOX/LEAD.wav", {} },      { "STABS", S + "/VOX/STABS.wav", {} } };

    // Every header first, before any audio is summed.
    std::vector<WavHeader> hdr (stems.size());
    std::vector<std::vector<unsigned char>> raw (stems.size());
    for (size_t i = 0; i < stems.size(); ++i)
    {
        if (! slurp (stems[i].path, raw[i]) || ! parseHeader (raw[i], hdr[i]))
        { std::printf ("STOP: %s: cannot read a RIFF WAVE header\n", stems[i].path.c_str()); return 1; }
        const auto& h = hdr[i];
        const size_t fr = h.align ? h.dataLen / (size_t) h.align : 0;
        std::printf ("HEADER %-8s tag=%d ch=%d sr=%u bits=%d align=%d data=%zu frames=%zu (%.4f s)  %s\n",
                     stems[i].key, h.tag, h.ch, h.sr, h.bits, h.align, h.dataLen, fr, h.sr ? fr / (double) h.sr : 0.0,
                     stems[i].path.c_str());
    }
    for (size_t i = 1; i < stems.size(); ++i)
        if (hdr[i].sr != hdr[0].sr) { std::printf ("STOP: %s is at %u Hz, %s at %u Hz\n", stems[i].key, hdr[i].sr, stems[0].key, hdr[0].sr); return 1; }
    for (size_t i = 0; i < stems.size(); ++i)
    {
        if (! decode (raw[i], hdr[i], stems[i].s)) { std::printf ("STOP: %s: not stereo 32-bit float or 24-bit PCM\n", stems[i].key); return 1; }
        std::vector<unsigned char>().swap (raw[i]);
    }
    size_t n = 0;
    for (const auto& x : stems) n = std::max (n, x.s.frames());
    for (auto& x : stems)
        if (x.s.frames() < n)
        {
            std::printf ("PAD %s: %zu frames short, zero-padded at the end\n", x.key, n - x.s.frames());
            x.s.l.resize (n, 0.0f); x.s.r.resize (n, 0.0f);
        }
        else if (! std::strcmp (x.key, "TAGS"))
            std::printf ("TAGS: same length as the others (%zu frames), no padding; it is smaller on disk because it is 24-bit PCM\n", n);
    const double sr = stems[0].s.sr;

    auto sumOf = [&] (std::initializer_list<const char*> keys)
    {
        Stereo o; o.sr = sr; o.l.assign (n, 0.0f); o.r.assign (n, 0.0f);
        for (const char* k : keys)
            for (const auto& x : stems)
                if (! std::strcmp (x.key, k))
                    for (size_t i = 0; i < n; ++i) { o.l[i] += x.s.l[i]; o.r[i] += x.s.r[i]; }
        return o;
    };
    const Stereo mix   = sumOf ({ "808", "KICK", "MUSIC FX", "MUSIC", "SN", "TAGS", "TOMS", "ADLIBS", "LEAD", "STABS" });
    const Stereo drums = sumOf ({ "KICK", "SN", "TOMS" });
    const Stereo music = sumOf ({ "MUSIC" });
    const Stereo lead  = sumOf ({ "LEAD" });
    const Stereo snToms = sumOf ({ "SN", "TOMS" });
    stems.clear(); stems.shrink_to_fit();
    {
        double pk = 0.0;
        for (size_t i = 0; i < n; ++i) pk = std::max (pk, (double) std::max (std::abs (mix.l[i]), std::abs (mix.r[i])));
        std::printf ("MIX: %zu frames at %.0f Hz, sample peak %.6f (%.2f dBFS), not normalised\n", n, sr, pk, 20.0 * std::log10 (pk));
    }

    std::array<float, 6> refRel {};
    if (! bandsRel (mix, 512, refRel)) { std::printf ("STOP: REF has a band on the floor\n"); return 1; }
    std::printf ("REF bandsRel (block 512):");
    for (int b = 0; b < 6; ++b) std::printf (" %s %+.3f", bands[b], refRel[(size_t) b]);
    std::printf ("\n");

    struct Grp { const char* name; const Stereo* s; };
    // PEAKS_SNTOMS is NOT in the brief: added after the first run found PEAKS
    // unreachable in all three bands. It is reported apart and never mixed into
    // the brief's separation line.
    constexpr int kGrps = 4;
    const Grp grps[kGrps] = { { "PEAKS", &drums }, { "BODY_MUSIC", &music }, { "BODY_VOCAL", &lead },
                              { "PEAKS_SNTOMS (supplementary)", &snToms } };
    const size_t targets[3] = { 2, 3, 4 };   // lowMid, mid, highMid
    constexpr double kTarget = 3.0, kTol = 0.05, kMaxGainDb = 20.0;

    auto Dof = [&] (const Stereo& g, double gainDb, size_t band, std::array<float, 6>& Dall) -> double
    {
        std::array<float, 6> rel {};
        const Stereo c = withGroup (mix, g, std::pow (10.0, gainDb / 20.0));
        if (! bandsRel (c, 512, rel)) return std::nan ("");
        for (size_t b = 0; b < 6; ++b) Dall[b] = rel[b] - refRel[b];
        return Dall[band];
    };

    // The REF crest, per variant and block, once.
    Fig refC[3][3];   // [variant][block index: 0 = 128, 1 = 512, 2 = 2048]
    const int blocksList[3] = { 128, 512, 2048 };
    for (int v = 0; v < 3; ++v) refC[v][1] = crestOf (v, mix, 512);
    refC[1][0] = crestOf (1, mix, 128);
    refC[1][2] = crestOf (1, mix, 2048);
    for (int v = 0; v < 3; ++v)
    {
        std::printf ("REF crest V%d (512):", v);
        for (size_t b = 0; b < 6; ++b) std::printf (" %s %s", bands[b], fmt (refC[v][1][b] ? std::optional<double> (*refC[v][1][b]) : std::nullopt, "%.3f").c_str());
        std::printf ("\n");
    }

    struct Res { bool reachable = false; double gainDb = 0, D = 0; std::array<float, 6> Dall {}; std::optional<double> dc[3]; Fig all[3]; std::optional<double> noise; };
    Res res[kGrps][3];   // [group][target]

    for (int t = 0; t < 3; ++t)
        for (int gi = 0; gi < kGrps; ++gi)
        {
            const size_t band = targets[t];
            Res& r = res[gi][t];
            std::array<float, 6> Dall {};
            const double dHi = Dof (*grps[gi].s, kMaxGainDb, band, Dall);
            std::printf ("\nCASE %s %s: D at +%.0f dB = %+.3f\n", grps[gi].name, bands[band], kMaxGainDb, dHi);
            // The scan: D on a 0.5 dB grid, so "unreachable" never rests on
            // bisection's assumption that D rises with g.
            {
                double best = -1e9, bestDb = 0.0;
                std::printf ("SCAN %s %s:", grps[gi].name, bands[band]);
                for (double gd = 0.0; gd <= kMaxGainDb + 1e-9; gd += 0.5)
                {
                    std::array<float, 6> tmp {};
                    const double d = Dof (*grps[gi].s, gd, band, tmp);
                    if (d > best) { best = d; bestDb = gd; }
                    if (std::fmod (gd, 2.0) == 0.0) std::printf (" %+.0f:%+.2f", gd, d);
                }
                std::printf (" | max D %+.3f at %+.1f dB\n", best, bestDb);
            }
            if (! (dHi >= kTarget))
            {
                std::printf ("CASE %s %s: UNREACHABLE (D at +20 dB is %+.3f, under +3.0), skipped\n", grps[gi].name, bands[band], dHi);
                continue;
            }
            double lo = 0.0, hi = kMaxGainDb, gDb = hi, D = dHi;
            for (int it = 0; it < 60; ++it)
            {
                gDb = 0.5 * (lo + hi);
                D = Dof (*grps[gi].s, gDb, band, Dall);
                if (std::abs (D - kTarget) <= kTol * 0.1) break;   // a tenth of the tolerance
                if (D < kTarget) lo = gDb; else hi = gDb;
            }
            if (std::abs (D - kTarget) > kTol) { std::printf ("CASE %s %s: bisection ended at D %+.4f, outside 3.0 +-0.05; skipped\n", grps[gi].name, bands[band], D); continue; }
            r.reachable = true; r.gainDb = gDb; r.D = D; r.Dall = Dall;
            std::printf ("CASE %s %s: g %+.3f dB (x%.4f), D %+.4f; D all bands:", grps[gi].name, bands[band], gDb, std::pow (10.0, gDb / 20.0), D);
            for (size_t b = 0; b < 6; ++b) std::printf (" %s %+.3f", bands[b], Dall[b]);
            std::printf ("\n");

            const Stereo c = withGroup (mix, *grps[gi].s, std::pow (10.0, gDb / 20.0));
            for (int v = 0; v < 3; ++v)
            {
                r.all[v] = crestOf (v, c, 512);
                r.dc[v] = delta (r.all[v], refC[v][1], band);
                std::printf ("CASE %s %s V%d dc:", grps[gi].name, bands[band], v);
                for (size_t b = 0; b < 6; ++b) std::printf (" %s %s", bands[b], fmt (delta (r.all[v], refC[v][1], b)).c_str());
                std::printf ("\n");
            }
            if (gi != 2)
            {
                double m = 0.0; bool ok = true;
                for (int bi : { 0, 2 })
                {
                    const Fig cb = crestOf (1, c, blocksList[bi]);
                    const auto d = delta (cb, refC[1][bi], band);
                    std::printf ("CASE %s %s V1 block %d dc %s (512: %s)\n", grps[gi].name, bands[band], blocksList[bi], fmt (d, "%+.4f").c_str(), fmt (r.dc[1], "%+.4f").c_str());
                    if (! d || ! r.dc[1]) ok = false; else m = std::max (m, std::abs (*d - *r.dc[1]));
                }
                if (ok) r.noise = m;
            }
        }

    std::printf ("\nTABLE dc in the target band (crest case minus crest ref, dB) | g dB | D\n");
    for (int t = 0; t < 3; ++t)
        for (int gi = 0; gi < kGrps; ++gi)
        {
            const Res& r = res[gi][t];
            if (! r.reachable) { std::printf ("TABLE %s %s | unreachable\n", bands[targets[t]], grps[gi].name); continue; }
            std::printf ("TABLE %s %s | V0 %s | V1 %s | V2 %s | g %+.3f | D %+.4f | V1 noise %s\n", bands[targets[t]], grps[gi].name,
                         fmt (r.dc[0]).c_str(), fmt (r.dc[1]).c_str(), fmt (r.dc[2]).c_str(), r.gainDb, r.D,
                         gi != 2 ? fmt (r.noise, "%.4f").c_str() : "n/a");
        }
    for (int v = 0; v < 3; ++v)
    {
        std::optional<double> lowPeaks, highBody;
        for (int t = 0; t < 3; ++t)
        {
            if (res[0][t].reachable && res[0][t].dc[v]) lowPeaks = lowPeaks ? std::min (*lowPeaks, *res[0][t].dc[v]) : *res[0][t].dc[v];
            for (int gi : { 1, 2 })
                if (res[gi][t].reachable && res[gi][t].dc[v]) highBody = highBody ? std::max (*highBody, *res[gi][t].dc[v]) : *res[gi][t].dc[v];
        }
        std::printf ("SEPARATION V%d | lowest PEAKS dc %s | highest BODY dc %s | gap %s\n", v, fmt (lowPeaks).c_str(), fmt (highBody).c_str(),
                     lowPeaks && highBody ? fmt (*lowPeaks - *highBody).c_str() : "n/a");
        std::optional<double> lowSup;
        for (int t = 0; t < 3; ++t)
            if (res[3][t].reachable && res[3][t].dc[v]) lowSup = lowSup ? std::min (*lowSup, *res[3][t].dc[v]) : *res[3][t].dc[v];
        std::printf ("SEPARATION V%d (supplementary, PEAKS_SNTOMS in place of PEAKS) | lowest PEAKS_SNTOMS dc %s | highest BODY dc %s | gap %s\n", v,
                     fmt (lowSup).c_str(), fmt (highBody).c_str(), lowSup && highBody ? fmt (*lowSup - *highBody).c_str() : "n/a");
    }
    return 0;
}
