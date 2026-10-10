// stress_tp (session L, 8 Oct 2026): the true-peak STRESS SET, judged by the arbiter (exact band-limited reconstruction).
// Signals: the wall guard's +6 dBFS uniform-noise bursts (3 ms / 50 ms / 1 s), white and pink noise bursts, square
// waves (100 Hz, 1 kHz), near-Nyquist tones (20 / 22 / 23.5 kHz), a hard-clipped mix excerpt (stand-in for a
// drum loop), and the hot full mix (file-based cases at 48 k only). Gains +6/+10/+15 dB, ceilings 0.0 / -1.0,
// 44.1 / 48 / 96 kHz, mono and stereo. Limiters: v2 Transparent, v2 Clean, the current shipping limiter (ejlegacy.h).
//   stress_tp [--quick] [--renders docs/limiter_ab/renders] [--only transparent|clean|legacy]
#include "ejmetrics.h"
#include "ejlegacy.h"
#include "../../Source/EJLimiterV2Core.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
struct Sig { std::string name; std::vector<std::vector<double>> ch; double sr; };
uint32_t g_seed = 1;
double rnd() { g_seed = g_seed * 1664525u + 1013904223u; return ((double) (g_seed >> 8) / 16777216.0) * 2.0 - 1.0; }

std::vector<Sig> synth (double sr, int nch)
{
    std::vector<Sig> out; const size_t N = (size_t) (2.5 * sr);
    auto make = [&] (const std::string& name, auto&& gen) { Sig s; s.name = name; s.sr = sr; s.ch.assign ((size_t) nch, std::vector<double> (N, 0.0)); for (int c = 0; c < nch; ++c) { g_seed = 11u + (uint32_t) c; for (size_t n = 0; n < N; ++n) s.ch[(size_t) c][n] = gen (n, c); } out.push_back (std::move (s)); };
    // the guard's bursts: +6 dBFS uniform noise, 3 ms / 50 ms / 1 s from 0.5 s, silence elsewhere (the limiter sees the +gain on top)
    for (double len : { 0.003, 0.05, 1.0 }) make ("guard_burst_" + std::to_string ((int) std::lround (len * 1000)) + "ms", [&] (size_t n, int) { const size_t a = (size_t) (0.5 * sr); return (n >= a && n < a + (size_t) (len * sr)) ? 2.0 * rnd() : 0.0; });
    make ("white_burst", [&] (size_t n, int) { const size_t a = (size_t) (0.3 * sr); return (n >= a && n < a + (size_t) (0.8 * sr)) ? rnd() : 0.0; });
    {   // pink: Paul Kellet's filter, normalised to ~0 dBFS peak
        std::vector<double> b (7, 0.0);
        make ("pink_burst", [&] (size_t n, int c) { if (n == 0) std::fill (b.begin(), b.end(), 0.0); (void) c; const double w = rnd(); b[0] = 0.99886 * b[0] + w * 0.0555179; b[1] = 0.99332 * b[1] + w * 0.0750759; b[2] = 0.96900 * b[2] + w * 0.1538520; b[3] = 0.86650 * b[3] + w * 0.3104856; b[4] = 0.55000 * b[4] + w * 0.5329522; b[5] = -0.7616 * b[5] - w * 0.0168980; const double p = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362) * 0.11; b[6] = w * 0.115926; const size_t a = (size_t) (0.3 * sr); return (n >= a && n < a + (size_t) (0.8 * sr)) ? 2.0 * p : 0.0; });
    }
    for (double f : { 100.0, 1000.0 }) make ("square_" + std::to_string ((int) f), [&] (size_t n, int) { return (std::fmod ((double) n * f / sr, 1.0) < 0.5 ? 1.0 : -1.0) * 0.9; });
    for (double f : { 20000.0, 22000.0, 23500.0 }) if (f < 0.49 * sr) make ("tone_" + std::to_string ((int) f), [&] (size_t n, int) { const double e = 0.05 * sr; const double w = n < e ? n / e : (N - n < e ? (N - n) / e : 1.0); return 0.9 * w * std::sin (2 * ejdsp::kPi * f * (double) n / sr); });
    return out;
}

Sig fromFile (const std::string& path, const std::string& name, double t0, double secs, int nch, double clipDb)
{
    const auto a = ejwav::read (path); Sig s; s.name = name; s.sr = a.sampleRate; const size_t n0 = (size_t) (t0 * a.sampleRate), N = std::min ((size_t) (secs * a.sampleRate), a.frames() - n0);
    s.ch.assign ((size_t) nch, std::vector<double> (N)); const double cl = clipDb < 0 ? ejdsp::lin (clipDb) : 1e9;
    for (int c = 0; c < nch; ++c) for (size_t n = 0; n < N; ++n) { double v = a.ch[(size_t) std::min (c, a.channels() - 1)][n0 + n]; v = std::max (-cl, std::min (cl, v)); s.ch[(size_t) c][n] = clipDb < 0 ? v / cl * 0.95 : v; }
    return s;
}

struct Res { double worstOverDb = -99; size_t overs = 0; double meterDb = -99; };
template <class F> Res judge (const Sig& s, double gainDb, double ceilingDb, F&& render)
{
    std::vector<std::vector<double>> out = render (s, gainDb, ceilingDb); Res r;
    for (const auto& c : out) { const auto a = ejdsp::truePeakExact (c, ejdsp::lin (ceilingDb), 0.02); r.worstOverDb = std::max (r.worstOverDb, ejdsp::dB (a.peakLin) - ceilingDb); r.overs += a.overs; const auto m = ejdsp::truePeak (c, ejdsp::lin (ceilingDb), 0.02); r.meterDb = std::max (r.meterDb, ejdsp::dB (m.peakLin) - ceilingDb); }
    return r;
}
}

int main (int argc, char** argv)
{
    std::string renders = "docs/limiter_ab/renders", only; bool quick = false; std::vector<double> optRates, optGains, optCeils; std::vector<int> optChans;
    auto parseList = [] (const char* v) { std::vector<double> r; for (const char* p = v; *p;) { r.push_back (std::atof (p)); while (*p && *p != ',') ++p; if (*p) ++p; } return r; };
    for (int i = 1; i < argc; ++i) { const std::string a = argv[i]; if (a == "--quick") quick = true; else if (a == "--renders" && i + 1 < argc) renders = argv[++i]; else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--rates" && i + 1 < argc) optRates = parseList (argv[++i]); else if (a == "--gains" && i + 1 < argc) optGains = parseList (argv[++i]); else if (a == "--ceilings" && i + 1 < argc) optCeils = parseList (argv[++i]); else if (a == "--chans" && i + 1 < argc) { for (double c : parseList (argv[++i])) optChans.push_back ((int) c); } }
    const auto T = echojay::limv2::transparent(), Cl = echojay::limv2::clean();
    auto v2 = [&] (const echojay::limv2::Tuning& t) { return [t] (const Sig& s, double g, double c) {
        const int nch = (int) s.ch.size(); const size_t N = s.ch[0].size(); echojay::limv2::Core core; core.prepare (s.sr, t); core.setInputGainDb (g); core.setCeilingDb (c); core.setTruePeak (true); core.reset(); const int lat = core.latencySamples();
        std::vector<std::vector<float>> b ((size_t) nch, std::vector<float> (N + (size_t) lat, 0.0f)); for (int ch = 0; ch < nch; ++ch) for (size_t n = 0; n < N; ++n) b[(size_t) ch][n] = (float) s.ch[(size_t) ch][n];
        for (size_t pos = 0; pos < N + (size_t) lat; pos += 512) { const int n = (int) std::min<size_t> (512, N + (size_t) lat - pos); float* p[2] = { b[0].data() + pos, nch > 1 ? b[1].data() + pos : nullptr }; core.process (p, nch, n); }
        std::vector<std::vector<double>> out ((size_t) nch, std::vector<double> (N)); for (int ch = 0; ch < nch; ++ch) for (size_t n = 0; n < N; ++n) out[(size_t) ch][n] = b[(size_t) ch][n + (size_t) lat]; return out; }; };
    auto legacy = [] (const Sig& s, double g, double c) {
        const int nch = (int) s.ch.size(); const size_t N = s.ch[0].size(); echojay::legacy::Limiter lim; lim.ceilingDb = c; lim.prepare (s.sr, g); const int lat = lim.latencySamples();
        std::vector<float> L (N + (size_t) lat, 0.0f), R (N + (size_t) lat, 0.0f); for (size_t n = 0; n < N; ++n) { L[n] = (float) s.ch[0][n]; R[n] = (float) s.ch[(size_t) (nch > 1 ? 1 : 0)][n]; }
        for (size_t pos = 0; pos < N + (size_t) lat; pos += 512) { const int n = (int) std::min<size_t> (512, N + (size_t) lat - pos); lim.process (L.data() + pos, R.data() + pos, n); }
        std::vector<std::vector<double>> out ((size_t) nch, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) { out[0][n] = L[n + (size_t) lat]; if (nch > 1) out[1][n] = R[n + (size_t) lat]; } return out; };
    struct Lim { std::string name; std::function<std::vector<std::vector<double>> (const Sig&, double, double)> fn; };
    std::vector<Lim> lims; if (only.empty() || only == "transparent") lims.push_back ({ "v2 Transparent", v2 (T) }); if (only.empty() || only == "clean") lims.push_back ({ "v2 Clean", v2 (Cl) }); if (only.empty() || only == "legacy") lims.push_back ({ "current limiter", legacy });
    if (only == "modern") lims.push_back ({ "v2 Modern", v2 (echojay::limv2::modern()) }); if (only == "punchy") lims.push_back ({ "v2 Punchy", v2 (echojay::limv2::punchy()) }); if (only == "allround") lims.push_back ({ "v2 Allround", v2 (echojay::limv2::allround()) });
    std::vector<double> gains = quick ? std::vector<double> { 10.0 } : std::vector<double> { 6.0, 10.0, 15.0 }; std::vector<double> ceilings = quick ? std::vector<double> { 0.0 } : std::vector<double> { 0.0, -1.0 };
    std::vector<double> rates = quick ? std::vector<double> { 48000.0 } : std::vector<double> { 44100.0, 48000.0, 96000.0 }; std::vector<int> chans = quick ? std::vector<int> { 2 } : std::vector<int> { 1, 2 };
    if (! optRates.empty()) rates = optRates; if (! optGains.empty()) gains = optGains; if (! optCeils.empty()) ceilings = optCeils; if (! optChans.empty()) chans = optChans;
    struct Row { std::string lim, sig; double sr; int nch; double gain, ceil, over, meter; size_t overs; };
    std::vector<Row> rows;
    for (double sr : rates) for (int nch : chans)
    {
        auto sigs = synth (sr, nch);
        if (sr == 48000.0) { try { sigs.push_back (fromFile (renders + "/source_fullmix.wav", "hot_fullmix_excerpt", 24.0, 12.0, nch, 1.0)); sigs.push_back (fromFile (renders + "/source_fullmix.wav", "hardclipped_mix_excerpt", 24.0, 8.0, nch, -6.0)); } catch (const std::exception& ex) { std::fprintf (stderr, "(file cases skipped: %s)\n", ex.what()); } }
        for (const auto& s : sigs) for (double g : gains) for (double c : ceilings) for (const auto& l : lims)
        {
            const auto r = judge (s, g, c, l.fn); rows.push_back ({ l.name, s.name, sr, nch, g, c, r.worstOverDb, r.meterDb, r.overs });
            std::printf ("%-15s %-26s %6.0f Hz %dch gain %+5.1f ceil %+4.1f : arbiter %+6.2f dB re ceiling (%5zu overs)  meter96 %+6.2f\n", l.name.c_str(), s.name.c_str(), sr, nch, g, c, r.worstOverDb, r.overs, r.meterDb);
        }
    }
    std::printf ("\n==== SUMMARY (arbiter: exact band-limited peak, over = more than +0.02 dB above the ceiling) ====\n");
    for (const auto& l : lims)
    {
        double worst = -99; std::string where; size_t nOver = 0, nCfg = 0; std::vector<Row> bad;
        for (const auto& r : rows) if (r.lim == l.name) { ++nCfg; if (r.over > worst) { worst = r.over; where = r.sig + " " + std::to_string ((int) r.sr) + " Hz " + std::to_string (r.nch) + "ch +" + std::to_string ((int) r.gain) + " dB ceil " + std::to_string ((int) r.ceil); } if (r.over > 0.02) { ++nOver; bad.push_back (r); } }
        std::printf ("  %-15s worst %+6.2f dB above the ceiling (%s); configs over by > 0.02 dB: %zu of %zu\n", l.name.c_str(), worst, where.c_str(), nOver, nCfg);
        std::sort (bad.begin(), bad.end(), [] (const Row& a, const Row& b) { return a.over > b.over; });
        for (size_t i = 0; i < std::min<size_t> (6, bad.size()); ++i) std::printf ("      %+6.2f  %s %d Hz %dch +%g dB ceil %g\n", bad[i].over, bad[i].sig.c_str(), (int) bad[i].sr, bad[i].nch, bad[i].gain, bad[i].ceil);
    }
    return 0;
}
