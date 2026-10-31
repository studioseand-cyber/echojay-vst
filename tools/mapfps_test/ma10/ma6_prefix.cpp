// MA6: THE CAPTURE-LENGTH EXPERIMENT (DESIGN_MATCH_TAB plan section 7; round 8h).
//
// HARNESS ONLY. No Source change. On the Marathon full mix (all ten stems
// summed at unity in float, as MA10 section 11 built it), the six band
// relatives over the whole 123 s, then over prefixes of 10, 20, 30, 60 and
// 120 s, and where each band settles within 0.5 dB of the whole.
//
// bandsRel IS MA10 SECTION 11's, which is the codebase's: per 512-sample block
// a 2048 Hann frame's perOct powers (ma10_variants.h's PerOctFrame<11>), summed
// with no silence gate (MeterEngine.cpp BandAccum), turned into dB by
// echojay::bandMeanFromSum and made relative by echojay::matchBandRelatives.
// ONE PASS: the running sums are read at every block, so a prefix is exactly
// the blocks that start inside it, and no prefix is computed twice.
//
// SETTLES = the earliest whole second t at which the band is within 0.5 dB of
// the whole AND stays within it at every later whole second to the end.
//
// The WAV reader below is copied unchanged from ma10_discrim.cpp (its anonymous
// namespace), so the stems are read exactly as MA10 read them: "rb" only.
//
// usage: ma6_prefix <stem folder>

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


} // namespace

int main (int argc, char** argv)
{
    if (argc != 2) { std::fprintf (stderr, "usage: ma6_prefix <stem folder>\n"); return 2; }
    const std::string S = argv[1];
    struct Stem { const char* key; std::string path; Stereo s; };
    std::vector<Stem> stems = {
        { "808", S + "/BEAT/808.wav", {} },       { "KICK", S + "/BEAT/KICK.wav", {} },
        { "MUSIC FX", S + "/BEAT/MUSIC FX.wav", {} }, { "MUSIC", S + "/BEAT/MUSIC.wav", {} },
        { "SN", S + "/BEAT/SN.wav", {} },         { "TAGS", S + "/BEAT/TAGS.wav", {} },
        { "TOMS", S + "/BEAT/TOMS.wav", {} },     { "ADLIBS", S + "/VOX/ADLIBS.wav", {} },
        { "LEAD", S + "/VOX/LEAD.wav", {} },      { "STABS", S + "/VOX/STABS.wav", {} } };
    for (auto& x : stems)
    {
        std::vector<unsigned char> raw;
        WavHeader h;
        if (! slurp (x.path, raw) || ! parseHeader (raw, h))
        { std::printf ("STOP: %s: cannot read a RIFF WAVE header\n", x.path.c_str()); return 1; }
        if (! decode (raw, h, x.s)) { std::printf ("STOP: %s: not stereo 32-bit float or 24-bit PCM\n", x.key); return 1; }
        std::printf ("STEM %-8s tag %d, %d ch, %u Hz, %d bit, %zu frames (%.4f s)\n", x.key, h.tag, h.ch, h.sr, h.bits,
                     x.s.frames(), x.s.frames() / (double) h.sr);
        if (x.s.sr != stems[0].s.sr && stems[0].s.sr > 0.0) { std::printf ("STOP: %s differs in rate\n", x.key); return 1; }
    }
    size_t n = 0;
    for (const auto& x : stems) n = std::max (n, x.s.frames());
    Stereo mix; mix.sr = stems[0].s.sr; mix.l.assign (n, 0.0f); mix.r.assign (n, 0.0f);
    for (const auto& x : stems)
        for (size_t i = 0; i < x.s.frames(); ++i) { mix.l[i] += x.s.l[i]; mix.r[i] += x.s.r[i]; }
    for (const auto& x : stems)
        if (x.s.frames() < n) std::printf ("PAD %s: %zu frames short, zero at the end\n", x.key, n - x.s.frames());
    stems.clear();
    const double sr = mix.sr, total = n / sr;
    std::printf ("MIX: ten stems at unity, %zu frames, %.4f s\n\n", n, total);

    // ONE PASS, the running sums kept at every block.
    constexpr int kBlock = 512;
    ma10::PerOctFrame<11> fr (sr);
    std::vector<float> ring (2048, 0.0f);
    int wp = 0;
    std::array<double, 6> sum {};
    std::vector<std::array<double, 6>> sums;   // after block k
    std::vector<size_t> starts;
    for (size_t i = 0; i < n; i += (size_t) kBlock)
    {
        const size_t e = std::min (n, i + (size_t) kBlock);
        for (size_t k = i; k < e; ++k) { ring[(size_t) wp] = (mix.l[k] + mix.r[k]) * 0.5f; wp = (wp + 1) % 2048; }
        const auto p = fr.compute (ring.data(), 2048, wp);
        for (size_t b = 0; b < 6; ++b) sum[b] += p[b];
        sums.push_back (sum);
        starts.push_back (i);
    }
    // The relatives over the blocks that start before t seconds.
    auto relAt = [&] (double t, std::array<float, 6>& rel) -> bool
    {
        size_t k = 0;
        while (k < starts.size() && (double) starts[k] < t * sr) ++k;
        if (k == 0) return false;
        std::array<float, 6> db {};
        for (size_t b = 0; b < 6; ++b) db[b] = echojay::bandMeanFromSum (sums[k - 1][b], (int) k).db;
        return echojay::matchBandRelatives (db, rel);
    };

    const char* names[6] = { "sub", "low", "lowMid", "mid", "highMid", "air" };
    std::array<float, 6> whole {};
    if (! relAt (total + 1.0, whole)) { std::printf ("STOP: the whole mix has a band on the floor\n"); return 1; }

    std::printf ("BAND RELATIVES (dB, each band minus the six-band mean)\n");
    std::printf ("%-8s", "prefix");
    for (auto* nm : names) std::printf (" %9s", nm);
    std::printf ("\n");
    const double prefixes[] = { 10, 20, 30, 60, 120 };
    for (double t : prefixes)
    {
        std::array<float, 6> rel {};
        std::printf ("%-8s", (juce::String ((int) t) + " s").toRawUTF8());
        if (! relAt (t, rel)) { std::printf (" a band on the floor\n"); continue; }
        for (size_t b = 0; b < 6; ++b) std::printf (" %+9.2f", rel[b]);
        std::printf ("\n");
    }
    std::printf ("%-8s", "whole");
    for (size_t b = 0; b < 6; ++b) std::printf (" %+9.2f", whole[b]);
    std::printf ("   (%.1f s)\n\n", total);

    std::printf ("DIFFERENCE FROM THE WHOLE (dB)\n");
    for (double t : prefixes)
    {
        std::array<float, 6> rel {};
        std::printf ("%-8s", (juce::String ((int) t) + " s").toRawUTF8());
        if (! relAt (t, rel)) { std::printf (" a band on the floor\n"); continue; }
        for (size_t b = 0; b < 6; ++b) std::printf (" %+9.2f", rel[b] - whole[b]);
        std::printf ("\n");
    }

    // WHERE EACH BAND SETTLES, on a 1 s grid.
    std::printf ("\nSETTLES WITHIN 0.5 dB OF THE WHOLE, and stays there (1 s grid)\n");
    const int lastS = (int) std::floor (total);
    std::vector<std::array<float, 6>> grid ((size_t) lastS + 1);
    std::vector<bool> ok ((size_t) lastS + 1, false);
    for (int t = 1; t <= lastS; ++t) ok[(size_t) t] = relAt ((double) t, grid[(size_t) t]);
    int allBands = 0;
    for (size_t b = 0; b < 6; ++b)
    {
        int settle = -1;
        for (int t = lastS; t >= 1; --t)
        {
            if (! ok[(size_t) t] || std::abs (grid[(size_t) t][b] - whole[b]) > 0.5f) break;
            settle = t;
        }
        allBands = std::max (allBands, settle < 0 ? lastS + 1 : settle);
        if (settle < 0) std::printf ("%-8s does not settle on the grid\n", names[b]);
        else std::printf ("%-8s %3d s\n", names[b], settle);
    }
    std::printf ("all six  %3d s\n", allBands);
    return 0;
}
