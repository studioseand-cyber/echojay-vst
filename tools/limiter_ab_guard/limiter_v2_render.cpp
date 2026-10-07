// limiter_v2_render (session L, 7 Oct 2026): renders a WAV through the Limiter v2 core, offline, the way a DAW
// bounce would - in 512-sample blocks, with the reported latency compensated (the output is shifted back by
// latencySamples so it lands at offset 0 against the source, like a PDC'd bounce; --no-pdc keeps it raw).
//
//   limiter_v2_render <in.wav> <out.wav> [--gain 8.2] [--ceiling 0.0] [--tp 1] [--lookahead 5] [--stages 3]
//                     [--style transparent|clean] [--fast 40] [--slow 500] [--slowatk 60] [--slowwin 20] [--slowfrac 1] [--link 1] [--margin 0.1] [--post 1] [--block 512] [--no-pdc]
//   --style resets every tuning field to that style; put it FIRST and override single fields after it.
#include "ejwav.h"
#include "../../Source/EJLimiterV2Core.h"
#include <cstdio>
#include <cstdlib>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: limiter_v2_render <in.wav> <out.wav> [options]\n"); return 2; }
    double gain = 8.2, ceiling = 0.0; bool tp = true, pdc = true; int block = 512;
    echojay::limv2::Tuning t = echojay::limv2::transparent();
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i]; auto next = [&] { return i + 1 < argc ? std::atof (argv[++i]) : 0.0; };
        if (a == "--gain") gain = next(); else if (a == "--ceiling") ceiling = next(); else if (a == "--tp") tp = next() >= 0.5;
        else if (a == "--lookahead") t.lookaheadMs = next(); else if (a == "--stages") t.smoothStages = (int) next();
        else if (a == "--fast") t.fastReleaseMs = next(); else if (a == "--slow") t.slowReleaseMs = next(); else if (a == "--slowatk") t.slowAttackMs = next();
        else if (a == "--link") t.link = next(); else if (a == "--margin") t.tpMarginDb = next(); else if (a == "--block") block = (int) next();
        else if (a == "--slowwin") t.slowWindowMs = next(); else if (a == "--post") t.postMs = next(); else if (a == "--slowfrac") t.slowFraction = next();
        else if (a == "--style") { const std::string st = i + 1 < argc ? argv[++i] : ""; if (st == "clean") t = echojay::limv2::clean(); else if (st == "transparent") t = echojay::limv2::transparent(); else { std::fprintf (stderr, "unknown style %s\n", st.c_str()); return 2; } }
        else if (a == "--no-pdc") pdc = false; else { std::fprintf (stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    ejwav::Audio a; try { a = ejwav::read (argv[1]); } catch (const std::exception& ex) { std::fprintf (stderr, "%s\n", ex.what()); return 2; }
    const int nch = std::min (2, a.channels()); const size_t N = a.frames();
    echojay::limv2::Core core; core.prepare (a.sampleRate, t); core.setInputGainDb (gain); core.setCeilingDb (ceiling); core.setTruePeak (tp); core.reset();
    const int latency = core.latencySamples();
    // the audio plus `latency` samples of silence at the end, so the compensated output keeps the whole tail
    std::vector<std::vector<float>> buf ((size_t) nch, std::vector<float> (N + (size_t) latency, 0.0f));
    for (int c = 0; c < nch; ++c) for (size_t n = 0; n < N; ++n) buf[(size_t) c][n] = (float) a.ch[(size_t) c][n];
    float* ptrs[2] = { buf[0].data(), nch > 1 ? buf[1].data() : nullptr };
    float grMin = 0.0f;
    for (size_t pos = 0; pos < N + (size_t) latency; pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, N + (size_t) latency - pos);
        float* p[2] = { ptrs[0] + pos, nch > 1 ? ptrs[1] + pos : nullptr };
        core.process (p, nch, n); grMin = std::min (grMin, core.gainReductionDb());
    }
    ejwav::Audio out; out.sampleRate = a.sampleRate; out.ch.assign ((size_t) nch, std::vector<double> (N));
    const size_t shift = pdc ? (size_t) latency : 0;
    for (int c = 0; c < nch; ++c) for (size_t n = 0; n < N; ++n) out.ch[(size_t) c][n] = buf[(size_t) c][n + shift];
    ejwav::writeFloat32 (argv[2], out);
    std::printf ("limiter_v2_render: %s -> %s  gain %+.1f ceiling %+.1f tp %d  lookahead %.1f ms stages %d fast %.0f slow %.0f slowatk %.0f slowwin %.0f link %.2f margin %.2f post %.1f  latency %d samples (%s)  GR max %.2f dB\n",
                 argv[1], argv[2], gain, ceiling, (int) tp, t.lookaheadMs, t.smoothStages, t.fastReleaseMs, t.slowReleaseMs, t.slowAttackMs, t.slowWindowMs, t.link, t.tpMarginDb, t.postMs, latency, pdc ? "compensated" : "raw", grMin);
    return 0;
}
