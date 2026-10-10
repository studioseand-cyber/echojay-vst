// limiter_legacy_render (session L, 7 Oct 2026): renders a WAV through the SHIPPING EchoJay Limiter's ported path
// (ejlegacy.h), latency-compensated like limiter_v2_render.
//
//   limiter_legacy_render <in.wav> <out.wav> [--gain 8.2] [--ceiling 0.0] [--tp 1] [--lookahead 2] [--release 50] [--no-pdc]
#include "ejwav.h"
#include "ejlegacy.h"
#include <cstdio>
#include <cstdlib>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: limiter_legacy_render <in.wav> <out.wav> [options]\n"); return 2; }
    double gain = 8.2; bool pdc = true; echojay::legacy::Limiter lim;
    for (int i = 3; i < argc; ++i) { const std::string a = argv[i]; auto next = [&] { return i + 1 < argc ? std::atof (argv[++i]) : 0.0; };
        if (a == "--gain") gain = next(); else if (a == "--ceiling") lim.ceilingDb = next(); else if (a == "--tp") lim.truePeak = next() >= 0.5; else if (a == "--lookahead") lim.lookaheadMs = next(); else if (a == "--release") lim.releaseMs = next(); else if (a == "--no-pdc") pdc = false; else { std::fprintf (stderr, "unknown option %s\n", a.c_str()); return 2; } }
    ejwav::Audio a; try { a = ejwav::read (argv[1]); } catch (const std::exception& ex) { std::fprintf (stderr, "%s\n", ex.what()); return 2; }
    const size_t N = a.frames(); lim.prepare (a.sampleRate, gain); const int lat = lim.latencySamples();
    std::vector<float> L (N + (size_t) lat, 0.0f), R (N + (size_t) lat, 0.0f); for (size_t n = 0; n < N; ++n) { L[n] = (float) a.ch[0][n]; R[n] = (float) a.ch[a.channels() > 1 ? 1 : 0][n]; }
    for (size_t pos = 0; pos < N + (size_t) lat; pos += 512) { const int n = (int) std::min<size_t> (512, N + (size_t) lat - pos); lim.process (L.data() + pos, R.data() + pos, n); }
    ejwav::Audio out; out.sampleRate = a.sampleRate; out.ch.assign (2, std::vector<double> (N)); const size_t sh = pdc ? (size_t) lat : 0;
    for (size_t n = 0; n < N; ++n) { out.ch[0][n] = L[n + sh]; out.ch[1][n] = R[n + sh]; }
    ejwav::writeFloat32 (argv[2], out);
    std::printf ("limiter_legacy_render: %s -> %s  gain %+.2f ceiling %+.1f tp %d lookahead %.1f release %.0f  latency %d samples (%s)\n", argv[1], argv[2], gain, lim.ceilingDb, (int) lim.truePeak, lim.lookaheadMs, lim.releaseMs, lat, pdc ? "compensated" : "raw");
    return 0;
}
