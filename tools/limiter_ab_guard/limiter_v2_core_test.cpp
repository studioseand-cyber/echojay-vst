// limiter_v2_core_test (session L, 7 Oct 2026): the Limiter v2 core against its own guarantees, measured by the
// harness's independent instruments. Every leg has a known-good and a known-bad side where one exists.
#include "ejmetrics.h"
#include "../../Source/EJLimiterV2Core.h"
#include "ejlegacy.h"
#include <cstdio>
#include <sys/stat.h>

namespace {
int failures = 0;
void check (bool ok, const std::string& what, const std::string& detail = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : ("  [" + detail + "]").c_str()); if (! ok) ++failures; }
std::string f2 (double v) { if (std::isnan (v)) return "n/a"; char b[32]; std::snprintf (b, sizeof b, "%.2f", v); return b; }

// render through the core in blocks, latency-compensated, like limiter_v2_render
ejwav::Audio render (const ejwav::Audio& src, double gainDb, double ceilingDb, bool tp, const echojay::limv2::Tuning& t, int block = 512, int* latencyOut = nullptr)
{
    const int nch = src.channels(); const size_t N = src.frames();
    echojay::limv2::Core core; core.prepare (src.sampleRate, t); core.setInputGainDb (gainDb); core.setCeilingDb (ceilingDb); core.setTruePeak (tp); core.reset();
    const int lat = core.latencySamples(); if (latencyOut) *latencyOut = lat;
    std::vector<std::vector<float>> buf ((size_t) nch, std::vector<float> (N + (size_t) lat, 0.0f));
    for (int c = 0; c < nch; ++c) for (size_t n = 0; n < N; ++n) buf[(size_t) c][n] = (float) src.ch[(size_t) c][n];
    for (size_t pos = 0; pos < N + (size_t) lat; pos += (size_t) block) { const int n = (int) std::min<size_t> ((size_t) block, N + (size_t) lat - pos); float* p[2] = { buf[0].data() + pos, nch > 1 ? buf[1].data() + pos : nullptr }; core.process (p, nch, n); }
    ejwav::Audio out; out.sampleRate = src.sampleRate; out.ch.assign ((size_t) nch, std::vector<double> (N));
    for (int c = 0; c < nch; ++c) for (size_t n = 0; n < N; ++n) out.ch[(size_t) c][n] = buf[(size_t) c][n + (size_t) lat];
    return out;
}
// 4 s: noise bursts of 3 ms, 50 ms and 1 s at +6 dBFS peak over a -20 dBFS bed (so GR is visible). `bandLimited`
// low-passes the noise at 19 kHz (65-tap windowed sinc): full-band white noise has content at Nyquist where no
// finite interpolator agrees with another, and no mastering signal has it; the full-band leg is printed, not asserted.
ejwav::Audio noiseBursts (double sr, bool bandLimited)
{
    ejwav::Audio a; a.sampleRate = sr; const size_t N = (size_t) (4 * sr); a.ch.assign (2, std::vector<double> (N, 0.0));
    uint32_t s = 99; auto rnd = [&] { s = s * 1664525u + 1013904223u; return ((double) (s >> 8) / 16777216.0) * 2.0 - 1.0; };
    std::vector<double> w (N); for (size_t n = 0; n < N; ++n) w[n] = 0.05 * rnd();
    size_t starts[3] = { (size_t) (0.5 * sr), (size_t) (1.5 * sr), (size_t) (2.5 * sr) }; size_t lens[3] = { (size_t) (0.003 * sr), (size_t) (0.05 * sr), (size_t) (1.0 * sr) };
    for (int b = 0; b < 3; ++b) for (size_t n = starts[b]; n < starts[b] + lens[b]; ++n) w[n] = rnd();
    if (bandLimited)
    {
        const int T = 65; std::vector<double> h (T); double sum = 0; const double fc = 19000.0 / sr;
        for (int k = 0; k < T; ++k) { const double x = k - (T - 1) / 2.0; const double sinc = x == 0 ? 2 * fc : std::sin (2 * ejdsp::kPi * fc * x) / (ejdsp::kPi * x); const double win = 0.42 - 0.5 * std::cos (2 * ejdsp::kPi * k / (T - 1)) + 0.08 * std::cos (4 * ejdsp::kPi * k / (T - 1)); h[(size_t) k] = sinc * win; sum += h[(size_t) k]; }
        for (auto& v : h) v /= sum;
        std::vector<double> y (N, 0.0); for (size_t n = 0; n < N; ++n) for (int k = 0; k < T; ++k) if (n >= (size_t) k) y[n] += h[(size_t) k] * w[n - (size_t) k];
        w = y;
    }
    double pk = 0; for (int b = 0; b < 3; ++b) for (size_t n = starts[b]; n < starts[b] + lens[b]; ++n) pk = std::max (pk, std::abs (w[n]));
    for (size_t n = 0; n < N; ++n) { const double v = w[n] * 2.0 / pk; a.ch[0][n] = v; a.ch[1][n] = v; }   // bursts peak at +6 dBFS
    return a;
}
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    const double sr = 48000.0; const auto T = echojay::limv2::transparent();
    std::printf ("limiter_v2_core_test\n");
    {   // the interpolator's passband: per-phase |H| at 1, 12, 20 kHz within 0.05 dB of unity
        echojay::limv2::TruePeak8x tp; tp.prepare(); double worst = 0;
        for (double f : { 1000.0, 12000.0, 20000.0 }) for (int ph = 0; ph < tp.kPhases; ++ph) { std::complex<double> H = 0; for (int k = 0; k < tp.kTaps; ++k) H += (double) tp.coef[ph][k] * std::exp (std::complex<double> (0, -2 * ejdsp::kPi * f / sr * k)); worst = std::max (worst, std::abs (20 * std::log10 (std::abs (H)))); }
        check (worst < 0.05, "8x/48-tap detector: |H| within 0.05 dB of unity at 1, 12 and 20 kHz, every phase", f2 (worst) + " dB");
    }
    {   // THE CEILING, on band-limited noise bursts at +6 dBFS peak with gain 0 and +8.2 - zero overs by the harness's 8x meter
        for (double gain : { 0.0, 8.2 })
        {
            const auto src = noiseBursts (sr, true); int lat = 0; const auto out = render (src, gain, -0.1, true, T, 512, &lat);
            const auto pk = ejm::peaks (out.ch, sr, -0.1);
            check (pk.overs == 0 && pk.truePeakDb <= -0.1 + 0.05, "noise bursts +6 dBFS, gain " + f2 (gain) + ", ceiling -0.1: ZERO true-peak overs", f2 (pk.truePeakDb) + " dBTP, " + std::to_string (pk.overs) + " overs, sample " + f2 (pk.samplePeakDb));
            check (pk.truePeakDb >= -0.1 - 0.3, "... and the ceiling is actually reached (not just quiet)", f2 (pk.truePeakDb));
            const auto al = ejm::align (src, out); check (al.ok && al.offset == 0, "latency-compensated render aligns at 0 (reported latency " + std::to_string (lat) + " is the real one)", al.why + " offset " + std::to_string (al.offset));
        }
        {   // the known-bad leg of the TP button: true peak OFF must show inter-sample overs on HF-rich bursts
            const auto src = noiseBursts (sr, true); const auto out = render (src, 8.2, -0.1, false, T); const auto pk = ejm::peaks (out.ch, sr, -0.1);
            check (pk.overs > 0, "true peak OFF: the sample-domain limiter lets inter-sample peaks over (the known-bad leg of the TP button)", f2 (pk.truePeakDb) + " dBTP, " + std::to_string (pk.overs) + " overs");
        }
        {   // full-band white noise: information only (see noiseBursts)
            const auto src = noiseBursts (sr, false); const auto out = render (src, 8.2, -0.1, true, T); const auto pk = ejm::peaks (out.ch, sr, -0.1);
            std::printf ("  info  full-band white-noise bursts (content to Nyquist): %s dBTP, %zu overs - not asserted\n", f2 (pk.truePeakDb).c_str(), pk.overs);
        }
    }
    {   // pass-through: below the ceiling the output is the delayed input, bit for bit at float precision
        const auto src = noiseBursts (sr, true); ejwav::Audio quiet = src; for (auto& c : quiet.ch) for (double& v : c) v *= 0.1;
        const auto out = render (quiet, 0.0, 0.0, true, T); double worst = 0; for (size_t c = 0; c < 2; ++c) for (size_t n = 0; n < quiet.frames(); ++n) worst = std::max (worst, std::abs (out.ch[c][n] - quiet.ch[c][n]));
        check (worst < 2e-7, "below the ceiling the core is a pure delay", "worst " + std::to_string (worst));
    }
    {   // block-size invariance: 64 vs 512 vs 1000 give the same samples
        const auto src = noiseBursts (sr, true); const auto a = render (src, 8.2, -0.1, true, T, 64), b = render (src, 8.2, -0.1, true, T, 512), c = render (src, 8.2, -0.1, true, T, 1000);
        double worst = 0; for (size_t ch = 0; ch < 2; ++ch) for (size_t n = 0; n < src.frames(); ++n) worst = std::max (worst, std::max (std::abs (a.ch[ch][n] - b.ch[ch][n]), std::abs (c.ch[ch][n] - b.ch[ch][n])));
        check (worst < 1e-6, "block size does not change the output (64 / 512 / 1000)", "worst " + std::to_string (worst));
    }
    {   // the probe fixture through the harness: pre-dip = lookahead, release program-dependent, ceiling held
        const auto L = ejfix::layout ("probe_transients"); const auto src = ejfix::generate (L, sr);
        const auto out = render (src, 8.2, 0.0, true, T); const auto R = ejm::analyse ("probe_transients", "v2", src, out, 8.2, 0.0);
        check (R.aligned && R.hits.size() == 14, "probe_transients renders, aligns and shows 14 hits", R.align.why + " " + std::to_string (R.hits.size()));
        if (R.aligned && R.hits.size() == 14)
        {
            check (R.pk.overs == 0, "probe: zero overs", f2 (R.pk.truePeakDb) + " dBTP");
            double preMin = 1e9, preMax = 0; for (const auto& h : R.hits) { preMin = std::min (preMin, h.preDipMs); preMax = std::max (preMax, h.preDipMs); }
            // the harness marks the dip at -0.5 dB; a smooth window crosses that well after it starts, so the measured
            // pre-dip is shorter than the lookahead - the same bias applies to the Pro-L 2 render it is compared with
            check (preMin >= 0.5 * T.lookaheadMs && preMax <= T.lookaheadMs + 1.0, "pre-dip on every hit within (0.5 .. 1) x the lookahead (" + f2 (T.lookaheadMs) + " ms)", f2 (preMin) + " .. " + f2 (preMax) + " ms");
            check (R.hits[0].t63 < R.hits[6].t63, "program-dependent release: a 1-sample hit recovers faster than a 1 s burst (t63)", f2 (R.hits[0].t63) + " vs " + f2 (R.hits[6].t63) + " ms");
            check (std::abs (R.hits[6].holdMs - 1000.0) < 50.0, "a 1 s burst is held for 1 s (+ the slow window and the first 0.2 dB of its release)", f2 (R.hits[6].holdMs));
            check (R.hits[0].retentionDb > -8.2 - 0.3 && R.hits[0].retentionDb < -8.2 + 0.3, "a +8.2 dB over impulse retains -8.2 dB (lands at the ceiling)", f2 (R.hits[0].retentionDb));
        }
    }
    {   // tones: a steady tone over the ceiling is held at a GAIN (THD very low), at 997 Hz and at 50 Hz
        for (const char* name : { "tone_997", "tone_50" })
        {
            const auto L = ejfix::layout (name); const auto src = ejfix::generate (L, sr); const auto out = render (src, 8.2, 0.0, true, T); const auto R = ejm::analyse (name, "v2", src, out, 8.2, 0.0);
            const bool ok = R.aligned && R.tone.segs.size() == 6;
            check (ok, std::string (name) + " renders and aligns", R.align.why);
            if (ok) { check (R.tone.segs[3].thdDb < -60.0, std::string (name) + " at +8.2 dB over: THD below -60 dB (a gain, not a clipper)", f2 (R.tone.segs[3].thdDb) + " dB, GR " + f2 (R.tone.segs[3].grSteadyDb)); check (R.pk.overs == 0, std::string (name) + ": zero overs", f2 (R.pk.truePeakDb) + " dBTP"); }
        }
    }
    {   // linking: link 1 both channels dip; link 0 the right channel does not
        const auto L = ejfix::layout ("panned_transient"); const auto src = ejfix::generate (L, sr);
        auto t0 = T; t0.link = 0.0;
        const auto Rl = ejm::analyse ("panned_transient", "linked", src, render (src, 8.2, 0.0, true, T), 8.2, 0.0), Ru = ejm::analyse ("panned_transient", "unlinked", src, render (src, 8.2, 0.0, true, t0), 8.2, 0.0);
        const bool ok = Rl.aligned && Ru.aligned && Rl.hits.size() == 12 && Ru.hits.size() == 12;
        check (ok, "panned_transient renders linked and unlinked", Rl.align.why + Ru.align.why);
        if (ok) { check (std::abs (Rl.hits[0].dipLDb - Rl.hits[0].dipRDb) < 0.1, "link 1: L and R dip equally", f2 (Rl.hits[0].dipLDb) + " / " + f2 (Rl.hits[0].dipRDb)); check (Ru.hits[0].dipLDb < -5.0 && Ru.hits[0].dipRDb > -0.3, "link 0: only L dips", f2 (Ru.hits[0].dipLDb) + " / " + f2 (Ru.hits[0].dipRDb)); check (Rl.pk.overs == 0 && Ru.pk.overs == 0, "both hold the ceiling", std::to_string (Rl.pk.overs) + " / " + std::to_string (Ru.pk.overs)); }
    }
    {   // THE LEGACY PORT against the real Pro Tools prints (skipped, and said so, when the renders are not on this machine)
        struct Case { const char* src; const char* print; double gain; } cases[] = { { "docs/limiter_ab/renders/source_bass_sustain.wav", "docs/limiter_ab/renders/echojay_bass_sustain.wav", 8.41 }, { "docs/limiter_ab/renders/source_fullmix.wav", "docs/limiter_ab/renders/echojay_fullmix.wav", 8.32 } };
        for (const auto& c : cases)
        {
            struct stat st; if (stat (c.src, &st) != 0 || stat (c.print, &st) != 0) { std::printf ("  skip  legacy port vs %s: file not present on this machine\n", c.print); continue; }
            const auto src = ejwav::read (c.src), print = ejwav::read (c.print); const size_t N = src.frames();
            echojay::legacy::Limiter lim; lim.prepare (src.sampleRate, c.gain); const int lat = lim.latencySamples();
            std::vector<float> L (N + (size_t) lat, 0.0f), R (N + (size_t) lat, 0.0f); for (size_t n = 0; n < N; ++n) { L[n] = (float) src.ch[0][n]; R[n] = (float) src.ch[1][n]; }
            for (size_t pos = 0; pos < N + (size_t) lat; pos += 512) { const int n = (int) std::min<size_t> (512, N + (size_t) lat - pos); lim.process (L.data() + pos, R.data() + pos, n); }
            ejwav::Audio out; out.sampleRate = src.sampleRate; out.ch.assign (2, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) { out.ch[0][n] = L[n + (size_t) lat]; out.ch[1][n] = R[n + (size_t) lat]; }
            const auto al = ejm::align (print, out); if (! al.ok) { check (false, std::string ("legacy port aligns with ") + c.print, al.why); continue; }
            const auto p = ejm::makePair (print, out, al.offset, 0.0, 0.0); double es = 0, ed = 0; for (int ch = 0; ch < 2; ++ch) for (size_t n = (size_t) (0.2 * sr); n < p.frames(); ++n) { const double x = p.in[(size_t) ch][n], y = p.out[(size_t) ch][n]; es += x * x; ed += (x - y) * (x - y); }
            const double resDb = 10 * std::log10 (ed / es);
            check (resDb < -40.0, std::string ("legacy port reproduces the Pro Tools print ") + c.print + " at the print's measured gain (residual < -40 dB re signal)", f2 (resDb) + " dB, offset " + std::to_string (al.offset));
        }
    }
    std::printf ("\n==== limiter_v2_core_test: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
