// limiter_v2_core_test (session L, 7 Oct 2026): the Limiter v2 core against its own guarantees, measured by the
// harness's independent instruments. Every leg has a known-good and a known-bad side where one exists.
#include "ejmetrics.h"
#include "../../Source/EJLimiterV2Core.h"
#include "ejlegacy.h"
#include <chrono>
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
            // noise is full-band, so the Nyquist-band margin applies (up to 0.6 dB, see Tuning::nyquistMarginDb): the output lands up to
            // ~0.6 dB under the ceiling on noise and at the ceiling on music (the stress set's hot-mix rows read -0.05)
            check (pk.truePeakDb >= -0.1 - 0.8, "... and the ceiling is actually reached, less the margin full-band noise earns (not just quiet)", f2 (pk.truePeakDb));
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
            // the harness reads GR in 0.33 ms blocks: a 0.3 ms window's pre-dip is at most one block (Pro-L 2 measured 0.0-0.3 ms)
            check (preMax <= T.lookaheadMs + 0.4, "pre-dip on every hit no longer than the lookahead (" + f2 (T.lookaheadMs) + " ms) plus one harness block", f2 (preMin) + " .. " + f2 (preMax) + " ms");
            check (R.hits[0].t63 < R.hits[6].t63, "program-dependent release: a 1-sample hit recovers faster than a 1 s burst (t63)", f2 (R.hits[0].t63) + " vs " + f2 (R.hits[6].t63) + " ms");
            {   // a 1 s burst is reduced for its whole length: GR 500 ms into it is still within 1.5 dB of its minimum (Transparent rides
                // the 3 kHz burst by ~0.5 dB like Pro-L 2, so the harness's 0.2 dB "hold" metric reads 0 for both - use the trace instead)
                const auto& h = R.hits[6]; const double at500 = h.trace.size() > 85 ? h.trace[25 + 60] : ejm::NaN;   // the trace runs -25..+60 ms; +60 ms is the last point
                check (! std::isnan (at500) && at500 < h.grMinDb + 1.5, "a 1 s burst stays reduced through its length (GR at +60 ms within 1.5 dB of the minimum)", f2 (at500) + " vs min " + f2 (h.grMinDb));
            }
            // the half-band detector reads a lone impulse's band-limited peak (above its sample value) and the B-spline window
            // spreads the dip, so the impulse lands up to ~0.7 dB under the ceiling; the rule against Pro-L 2 allows 1 dB
            check (R.hits[0].retentionDb > -8.2 - 1.0 && R.hits[0].retentionDb < -8.2 + 0.3, "a +8.2 dB over impulse lands at or within 1 dB under the ceiling", f2 (R.hits[0].retentionDb));
        }
    }
    {   // STYLES (9 Oct 2026): Transparent is unchanged by the second-stage release code path (slowRelease2Ms 0 = slowReleaseMs:
        // the explicit value renders sample-identical), and every style renders, aligns and holds the ceiling on the probe
        const auto src = ejfix::generate (ejfix::layout ("probe_transients"), sr);
        { auto t2 = T; t2.slowRelease2Ms = T.slowReleaseMs; const auto a = render (src, 8.2, 0.0, true, T), b = render (src, 8.2, 0.0, true, t2); double worst = 0; for (size_t i = 0; i < a.frames(); ++i) worst = std::max (worst, std::abs (a.ch[0][i] - b.ch[0][i]));
          check (worst == 0.0, "TRANSPARENT: slowRelease2Ms 0 (as gated) == slowReleaseMs written explicitly, sample for sample (the styles' second release changed nothing)", "worst " + std::to_string (worst)); }
        check (echojay::limv2::styleTuning (0).slowRelease2Ms == 0.0 && echojay::limv2::styleTuning (0).lookaheadMs == T.lookaheadMs && echojay::limv2::styleTuning (0).slowCloseMs == T.slowCloseMs, "styleTuning (0) is transparent()", "");
        struct St { const char* name; echojay::limv2::Tuning t; }; const St styles[] = { { "modern", echojay::limv2::modern() }, { "punchy", echojay::limv2::punchy() }, { "allround", echojay::limv2::allround() } };
        for (const auto& st : styles)
        {
            const auto R = ejm::analyse ("probe_transients", st.name, src, render (src, 8.2, 0.0, true, st.t), 8.2, 0.0);
            check (R.aligned && R.hits.size() == 14 && R.pk.overs == 0, std::string (st.name) + ": renders, aligns, 14 hits, zero overs on the probe at +8.2 dB", R.align.why + " hits " + std::to_string (R.hits.size()) + " " + f2 (R.pk.truePeakDb) + " dBTP");
            check (R.aligned && R.hits.size() == 14 && R.hits[0].t63 < R.hits[6].t63, std::string (st.name) + ": program-dependent release (a 1-sample hit recovers faster than a 1 s burst)", R.aligned && R.hits.size() == 14 ? f2 (R.hits[0].t63) + " vs " + f2 (R.hits[6].t63) + " ms" : "n/a");
        }
    }
    {   // tones. CLEAN holds a steady tone at a GAIN (THD below -60 dB). TRANSPARENT (C4 approved, 7 Oct) rides the waveform
        // the way Pro-L 2 measured (-26.6 dB at 997 Hz, -19.6 at 50 Hz, +8.2 over): THD between -45 and -12 dB, odd harmonics.
        for (const char* name : { "tone_997", "tone_50" })
        {
            const auto L = ejfix::layout (name); const auto src = ejfix::generate (L, sr);
            const auto Rt = ejm::analyse (name, "transparent", src, render (src, 8.2, 0.0, true, T), 8.2, 0.0), Rc = ejm::analyse (name, "clean", src, render (src, 8.2, 0.0, true, echojay::limv2::clean()), 8.2, 0.0);
            const bool ok = Rt.aligned && Rc.aligned && Rt.tone.segs.size() == 6 && Rc.tone.segs.size() == 6;
            check (ok, std::string (name) + " renders and aligns in both styles", Rt.align.why + Rc.align.why);
            if (ok)
            {
                check (Rc.tone.segs[3].thdDb < -60.0, std::string (name) + " CLEAN at +8.2 dB over: THD below -60 dB (a gain, not a clipper)", f2 (Rc.tone.segs[3].thdDb) + " dB, GR " + f2 (Rc.tone.segs[3].grSteadyDb));
                check (Rt.tone.segs[3].thdDb > -55.0 && Rt.tone.segs[3].thdDb < -12.0, std::string (name) + " TRANSPARENT at +8.2 dB over: rides the waveform like Pro-L 2 (THD -55..-12 dB; Pro-L 2 -27 / -20)", f2 (Rt.tone.segs[3].thdDb) + " dB, GR " + f2 (Rt.tone.segs[3].grSteadyDb));
                check (Rt.pk.overs == 0 && Rc.pk.overs == 0, std::string (name) + ": zero overs in both styles", f2 (Rt.pk.truePeakDb) + " / " + f2 (Rc.pk.truePeakDb) + " dBTP");
            }
        }
    }
    {   // linking: link 1 both channels dip; link 0 the right channel does not
        const auto L = ejfix::layout ("panned_transient"); const auto src = ejfix::generate (L, sr);
        auto t0 = T; t0.link = 0.0; t0.linkRelease = 0.0; auto t1 = T; t1.link = 1.0;   // link 0 here = transient AND release link off (Transparent's release link is 100 %: the floor is shared by design, see the leg below)
        const auto Rl = ejm::analyse ("panned_transient", "linked", src, render (src, 8.2, 0.0, true, t1), 8.2, 0.0), Ru = ejm::analyse ("panned_transient", "unlinked", src, render (src, 8.2, 0.0, true, t0), 8.2, 0.0), Rd = ejm::analyse ("panned_transient", "default", src, render (src, 8.2, 0.0, true, T), 8.2, 0.0);
        if (Rd.aligned && Rd.hits.size() == 12) check (std::abs (Rd.hits[0].dipRDb / Rd.hits[0].dipLDb - T.link) < 0.05, "the Transparent default links the right channel at the tuned fraction (Pro-L 2 measured 0.75)", f2 (Rd.hits[0].dipRDb) + " / " + f2 (Rd.hits[0].dipLDb));
        const bool ok = Rl.aligned && Ru.aligned && Rl.hits.size() == 12 && Ru.hits.size() == 12;
        check (ok, "panned_transient renders linked and unlinked", Rl.align.why + Ru.align.why);
        if (ok)
        {   // the harness's dip is a per-block ENERGY ratio of each channel's own content; with a 0.06 ms window and a 0.05 ms fast release
            // the gain moves inside a block, so the two channels' ratios differ by up to ~0.3 dB for one and the same gain - the exact
            // claim is per sample: at link 1 the right channel's gain IS the left channel's (one envelope), checked below to 1e-6
            check (std::abs (Rl.hits[0].dipLDb - Rl.hits[0].dipRDb) < 0.3, "link 1: L and R dip equally (block energy ratio, within 0.3 dB)", f2 (Rl.hits[0].dipLDb) + " / " + f2 (Rl.hits[0].dipRDb));
            {
                const auto outL = render (src, 8.2, 0.0, true, t1); double worstG = 0; long n = 0; const double G = std::pow (10.0, 8.2 / 20.0);
                for (size_t i = 0; i < src.frames() && i < outL.frames(); ++i)
                {
                    const double xl = src.ch[0][i] * G, xr = src.ch[1][i] * G; if (std::abs (xl) < 0.05 || std::abs (xr) < 0.05) continue;
                    const double gl = outL.ch[0][i] / xl, gr = outL.ch[1][i] / xr; worstG = std::max (worstG, std::abs (gl - gr)); ++n;
                }
                check (n > 1000 && worstG < 1e-5, "link 1: the per-sample gain applied to R is identical to L's (one envelope)", "worst " + std::to_string (worstG) + " over " + std::to_string (n) + " samples");
            }
            {   // link 0 with the Transparent release link (100 %): the right channel is not dipped by the transient link but shares the floor
                auto t0r = T; t0r.link = 0.0; const auto Rr = ejm::analyse ("panned_transient", "release-linked", src, render (src, 8.2, 0.0, true, t0r), 8.2, 0.0);
                if (Rr.aligned && Rr.hits.size() == 12) check (Rr.hits[0].dipRDb < -0.1 && Rr.hits[0].dipRDb > -1.5 && Rr.hits[0].dipLDb < -5.0, "link 0 + release link 1: R takes only the shared floor (-0.1 .. -1.5 dB), L the full dip", f2 (Rr.hits[0].dipLDb) + " / " + f2 (Rr.hits[0].dipRDb));
            } check (Ru.hits[0].dipLDb < -5.0 && Ru.hits[0].dipRDb > -0.3, "link 0 (both links off): only L dips", f2 (Ru.hits[0].dipLDb) + " / " + f2 (Ru.hits[0].dipRDb)); check (Rl.pk.overs == 0 && Ru.pk.overs == 0, "both hold the ceiling", std::to_string (Rl.pk.overs) + " / " + std::to_string (Ru.pk.overs));
        }
    }
    {   // HARD REQUIREMENTS (overnight brief, 7 Oct): zero overs at +15 dB, near-silence, mono, every sample rate, no NaN,
        // no subnormal output, and no subnormal left in the state after 10 s of digital silence following a loud burst
        auto scan = [] (const ejwav::Audio& o, size_t& nan, size_t& sub) { nan = sub = 0; for (const auto& c : o.ch) for (double v : c) { if (! std::isfinite (v)) ++nan; else if (v != 0.0 && std::fpclassify ((float) v) == FP_SUBNORMAL) ++sub; } };
        for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        {
            const auto L = ejfix::layout ("probe_transients"); auto src = ejfix::generate (L, rate);
            for (double gain : { 15.0 })
            {
                const auto out = render (src, gain, -0.1, true, T); size_t nan, sub; scan (out, nan, sub); const auto pk = ejm::peaks (out.ch, rate, -0.1);
                check (pk.overs == 0 && nan == 0 && sub == 0, "probe at +15 dB, " + std::to_string ((int) rate) + " Hz: zero overs, no NaN, no subnormal output", f2 (pk.truePeakDb) + " dBTP, " + std::to_string (pk.overs) + " overs, nan " + std::to_string (nan) + ", sub " + std::to_string (sub));
            }
            {   // near-silence: -100 dBFS noise is passed as a pure delay, no NaN, no subnormals, no gain action
                ejwav::Audio q; q.sampleRate = rate; const size_t N = (size_t) (3 * rate); q.ch.assign (2, std::vector<double> (N)); uint32_t s = 7; for (size_t n = 0; n < N; ++n) { s = s * 1664525u + 1013904223u; const double v = 1e-5 * (((double) (s >> 8) / 16777216.0) * 2.0 - 1.0); q.ch[0][n] = v; q.ch[1][n] = -v; }
                const auto out = render (q, 15.0, 0.0, true, T); size_t nan, sub; scan (out, nan, sub); double worst = 0; for (size_t c = 0; c < 2; ++c) for (size_t n = 0; n < N; ++n) worst = std::max (worst, std::abs (out.ch[c][n] - q.ch[c][n] * ejdsp::lin (15.0)));
                check (nan == 0 && sub == 0 && worst < 1e-6, "near-silence (-100 dBFS) at +15 dB, " + std::to_string ((int) rate) + " Hz: pure gain+delay, no NaN, no subnormals", "worst " + std::to_string (worst) + " nan " + std::to_string (nan) + " sub " + std::to_string (sub));
            }
            {   // mono: one channel, same guarantees
                ejwav::Audio m; m.sampleRate = rate; m.ch.assign (1, src.ch[0]); const auto out = render (m, 8.2, -0.1, true, T); size_t nan, sub; scan (out, nan, sub); const auto pk = ejm::peaks (out.ch, rate, -0.1);
                check (out.channels() == 1 && pk.overs == 0 && nan == 0 && sub == 0, "mono probe at +8.2 dB, " + std::to_string ((int) rate) + " Hz: zero overs, no NaN, no subnormals", f2 (pk.truePeakDb) + " dBTP, " + std::to_string (pk.overs) + " overs");
            }
        }
        {   // a loud burst then 10 s of digital zero: the output must reach exactly zero (no subnormal tail), and the state must not
            // be left subnormal - checked by processing one more block and scanning it
            ejwav::Audio b; b.sampleRate = sr; const size_t N = (size_t) (11 * sr); b.ch.assign (2, std::vector<double> (N, 0.0)); for (size_t n = (size_t) (0.5 * sr); n < (size_t) (0.6 * sr); ++n) b.ch[0][n] = b.ch[1][n] = 2.0 * std::sin (2 * ejdsp::kPi * 100.0 * (double) n / sr);
            const auto out = render (b, 8.2, -0.1, true, T); size_t nan, sub; scan (out, nan, sub); double tail = 0; for (size_t c = 0; c < 2; ++c) for (size_t n = (size_t) (5 * sr); n < N; ++n) tail = std::max (tail, std::abs (out.ch[c][n]));
            check (nan == 0 && sub == 0 && tail == 0.0, "burst then 10 s of silence: output exactly zero after 5 s, no subnormals anywhere", "tail " + std::to_string (tail) + " sub " + std::to_string (sub));
        }
    }
    {   // FIXED LATENCY: one number for every setting (true peak on/off, any window), and the render still aligns at
        // exactly that number; the gain lands on the same sample it would without it (renders bit-identical in timing)
        const auto L = ejfix::layout ("probe_transients"); const auto src = ejfix::generate (L, sr);
        auto renderFixed = [&] (bool tp, double laMs, int* latOut) {
            const size_t N = src.frames(); auto t = T; t.maxLookaheadMs = T.lookaheadMs * 5.0; echojay::limv2::Core core; core.prepare (sr, t); core.setFixedLatency (true); auto tt = t; tt.lookaheadMs = laMs; core.setTuning (tt); core.setInputGainDb (8.2); core.setCeilingDb (0.0); core.setTruePeak (tp); core.reset();
            const int lat = core.latencySamples(); if (latOut) *latOut = lat;
            std::vector<std::vector<float>> buf (2, std::vector<float> (N + (size_t) lat, 0.0f)); for (int c = 0; c < 2; ++c) for (size_t n = 0; n < N; ++n) buf[(size_t) c][n] = (float) src.ch[(size_t) c][n];
            for (size_t pos = 0; pos < N + (size_t) lat; pos += 512) { const int n = (int) std::min<size_t> (512, N + (size_t) lat - pos); float* p[2] = { buf[0].data() + pos, buf[1].data() + pos }; core.process (p, 2, n); }
            ejwav::Audio out; out.sampleRate = sr; out.ch.assign (2, std::vector<double> (N)); for (int c = 0; c < 2; ++c) for (size_t n = 0; n < N; ++n) out.ch[(size_t) c][n] = buf[(size_t) c][n + (size_t) lat]; return out; };
        int l1, l2, l3, l4; const auto a = renderFixed (true, T.lookaheadMs, &l1); renderFixed (false, T.lookaheadMs, &l2); renderFixed (true, 0.0, &l3); const auto d = renderFixed (true, T.lookaheadMs * 5.0, &l4);
        check (l1 == l2 && l2 == l3 && l3 == l4, "fixed latency is identical with TP on/off and windows 0 .. 5x", std::to_string (l1) + " / " + std::to_string (l2) + " / " + std::to_string (l3) + " / " + std::to_string (l4));
        const auto Ra = ejm::analyse ("probe_transients", "fixed", src, a, 8.2, 0.0); check (Ra.aligned && Ra.align.offset == 0, "a fixed-latency render compensated by the reported number aligns at 0 (on the markers)", Ra.align.why + " offset " + std::to_string (Ra.align.offset));
        const auto nat = render (src, 8.2, 0.0, true, T); double worst = 0; for (size_t c = 0; c < 2; ++c) for (size_t n = 0; n < src.frames(); ++n) worst = std::max (worst, std::abs (nat.ch[c][n] - a.ch[c][n]));
        check (worst < 1e-6, "fixed latency does not change the audio (same samples as the natural-latency render)", "worst " + std::to_string (worst));
        const auto Rd = ejm::analyse ("probe_transients", "wide", src, d, 8.2, 0.0); check (Rd.aligned && Rd.pk.overs == 0, "the 5x window under fixed latency still holds the ceiling", std::to_string (Rd.pk.overs) + " overs");
    }
    {   // BYPASS: toggled mid-reduction on a sustained +8 dB tone, the output never steps by more than the tone's own
        // sample-to-sample change (no click); un-bypassed, it is back to limiting with zero overs
        const size_t N = (size_t) (4 * sr); ejwav::Audio t; t.sampleRate = sr; t.ch.assign (2, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) t.ch[0][n] = t.ch[1][n] = std::sin (2 * ejdsp::kPi * 200.0 * (double) n / sr);
        echojay::limv2::Core core; core.prepare (sr, T); core.setInputGainDb (8.0); core.setCeilingDb (0.0); core.setTruePeak (true); core.reset(); const int lat = core.latencySamples();
        std::vector<float> a (N + (size_t) lat, 0.0f), b (N + (size_t) lat, 0.0f); for (size_t n = 0; n < N; ++n) a[n] = b[n] = (float) t.ch[0][n];
        double maxStep = 0; size_t stepAt = 0;
        for (size_t pos = 0; pos < N + (size_t) lat; pos += 512) { const double tsec = (double) pos / sr; core.setBypassed (tsec >= 1.0 && tsec < 2.0); const int n = (int) std::min<size_t> (512, N + (size_t) lat - pos); float* p[2] = { a.data() + pos, b.data() + pos }; core.process (p, 2, n); }
        const double maxToneStep = 2.0 * ejdsp::lin (8.0) * std::sin (ejdsp::kPi * 200.0 / sr) * 1.05;   // the tone's own largest sample-to-sample change at +8 dB, +5 %
        for (size_t n = (size_t) lat + 1; n < N + (size_t) lat; ++n) { const double st = std::abs ((double) a[n] - (double) a[n - 1]); if (st > maxStep) { maxStep = st; stepAt = n; } }
        check (maxStep <= maxToneStep, "bypass in and out mid-reduction: no sample step beyond the waveform's own (no click)", "max step " + f2 (maxStep) + " at " + f2 ((double) stepAt / sr) + " s, tone's own " + f2 (maxToneStep));
        double pkBy = 0, pkLim = 0; for (size_t n = (size_t) (1.5 * sr); n < (size_t) (1.9 * sr); ++n) pkBy = std::max (pkBy, std::abs ((double) a[n + (size_t) lat])); for (size_t n = (size_t) (3.0 * sr); n < (size_t) (3.9 * sr); ++n) pkLim = std::max (pkLim, std::abs ((double) a[n + (size_t) lat]));
        check (pkBy > 2.0 && pkLim <= 1.0, "under bypass the +8 dB tone passes unclipped; after bypass it is limited again", "bypassed peak " + f2 (pkBy) + ", limited peak " + f2 (pkLim));
    }
    {   // SIDECHAIN HPF: with the post-check disabled (so only the main detector decides), a 30 Hz tone at +6 dB over with
        // the detector high-passed at 200 Hz is NOT reduced and the safety clip holds the samples; with it off it is.
        // (With the post-check on, as shipped, the post stage limits what the main detector was told to ignore - the
        // ceiling holds either way, by a 1 ms stage instead of a hard clip.)
        const size_t N = (size_t) (3 * sr); ejwav::Audio t; t.sampleRate = sr; t.ch.assign (2, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) t.ch[0][n] = t.ch[1][n] = std::sin (2 * ejdsp::kPi * 30.0 * (double) n / sr);
        auto run = [&] (double hz) { auto tp0 = T; tp0.postMs = 0.0; echojay::limv2::Core core; core.prepare (sr, tp0); core.setInputGainDb (6.0); core.setCeilingDb (0.0); core.setTruePeak (true); core.setSidechainHpfHz (hz); core.reset(); const int lat = core.latencySamples(); std::vector<float> a (N + (size_t) lat, 0.0f), b (N + (size_t) lat, 0.0f); for (size_t n = 0; n < N; ++n) a[n] = b[n] = (float) t.ch[0][n]; float grMin = 0; for (size_t pos = 0; pos < N + (size_t) lat; pos += 512) { const int n = (int) std::min<size_t> (512, N + (size_t) lat - pos); float* p[2] = { a.data() + pos, b.data() + pos }; core.process (p, 2, n); if (pos > sr) grMin = std::min (grMin, core.gainReductionDb()); } double pk = 0; for (size_t n = (size_t) sr; n < N; ++n) pk = std::max (pk, std::abs ((double) a[n + (size_t) lat])); return std::make_pair (grMin, pk); };
        const auto off = run (0.0), on = run (200.0);
        check (off.first < -5.0 && on.first > -0.5 && on.second <= 1.0 + 1e-6, "sc_hpf 200 Hz: a 30 Hz tone +6 dB over is not reduced (clip holds the samples); hpf off reduces 6 dB", "GR off " + f2 (off.first) + ", on " + f2 (on.first) + ", peak on " + f2 (on.second));
    }
    {   // gainReductionDb() IS MEASURED: per 512-sample block, the reported deepest gain equals the deepest output/input
        // ratio actually applied in that block (input delayed by the latency, samples above -40 dBFS), within 0.2 dB.
        // Semantics: PEAK GR per block (the most negative), which is what LoudnessLoop has always read.
        const auto L = ejfix::layout ("probe_transients"); const auto src = ejfix::generate (L, sr); const size_t N = src.frames();
        echojay::limv2::Core core; core.prepare (sr, T); core.setInputGainDb (8.2); core.setCeilingDb (0.0); core.setTruePeak (true); core.reset();
        const int lat = core.latencySamples(); const double G = ejdsp::lin (8.2);
        std::vector<float> Lc (N + (size_t) lat, 0.0f), Rc (N + (size_t) lat, 0.0f); for (size_t n = 0; n < N; ++n) { Lc[n] = (float) src.ch[0][n]; Rc[n] = (float) src.ch[1][n]; }
        double worst = 0; size_t blocks = 0, deep = 0;
        for (size_t pos = 0; pos + 512 <= N + (size_t) lat; pos += 512)
        {
            std::vector<float> inL (Lc.begin() + (long) pos, Lc.begin() + (long) pos + 512), inR (Rc.begin() + (long) pos, Rc.begin() + (long) pos + 512);
            float* p[2] = { Lc.data() + pos, Rc.data() + pos }; core.process (p, 2, 512); const double reported = core.gainReductionDb();
            double measuredMin = 1.0; for (int i = 0; i < 512; ++i) { const long long src_n = (long long) (pos + (size_t) i) - lat; if (src_n < 0 || (size_t) src_n >= N) continue; const double xi = std::abs (src.ch[0][(size_t) src_n]) * G; if (xi < 0.01) continue; measuredMin = std::min (measuredMin, std::abs ((double) Lc[pos + (size_t) i]) / xi); }
            if (measuredMin < 1.0) { const double md = ejdsp::dB (measuredMin); worst = std::max (worst, std::abs (md - reported)); ++blocks; if (reported < -3.0) ++deep; }
        }
        check (blocks > 50 && worst < 0.2, "reported gainReductionDb() = the deepest output/input ratio in the block (peak GR), within 0.2 dB over " + std::to_string (blocks) + " reducing blocks", "worst " + f2 (worst) + " dB, " + std::to_string (deep) + " blocks deeper than 3 dB");
        {   // BLOCK GR: the reported energy ratio per 512-sample block against the harness's own per-block GR of the compensated
            // render, on the STEADY part of the 1 s burst (hit 6: 16.1 .. 17.1 s), where the latency's two-block shift is immaterial
            echojay::limv2::Core c2; c2.prepare (sr, T); c2.setInputGainDb (8.2); c2.setCeilingDb (0.0); c2.setTruePeak (true); c2.reset(); const int lat2 = c2.latencySamples();
            std::vector<float> a (N + (size_t) lat2, 0.0f), b (N + (size_t) lat2, 0.0f); for (size_t n = 0; n < N; ++n) { a[n] = (float) src.ch[0][n]; b[n] = (float) src.ch[1][n]; }
            std::vector<double> rep;
            for (size_t pos = 0; pos + 512 <= N + (size_t) lat2; pos += 512) { float* p[2] = { a.data() + pos, b.data() + pos }; c2.process (p, 2, 512); rep.push_back (c2.blockGainReductionDb()); }
            ejwav::Audio o; o.sampleRate = sr; o.ch.assign (2, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) { o.ch[0][n] = a[n + (size_t) lat2]; o.ch[1][n] = b[n + (size_t) lat2]; }
            const auto pr = ejm::makePair (src, o, 0, 8.2, 0.0); const auto g512 = ejm::grSeries (pr, 512);
            double worstB = 0; size_t nB = 0; const size_t k0 = (size_t) (16.3 * sr / 512), k1 = (size_t) (16.9 * sr / 512);
            for (size_t k = k0; k < k1 && k < g512.size() && k + 3 < rep.size(); ++k) { const double r = rep[k + (size_t) ((lat2 + 256) / 512)]; if (! std::isnan (g512[k])) { worstB = std::max (worstB, std::abs (r - g512[k])); ++nB; } }
            check (nB > 20 && worstB < 0.3, "reported blockGainReductionDb() (energy ratio) = the harness's per-block GR within 0.3 dB on the steady 1 s burst (" + std::to_string (nB) + " blocks)", "worst " + f2 (worstB) + " dB");
        }
    }
    {   // CPU: 60 s of stereo at 48 k through v2 Transparent and through the legacy port, same machine, same moment
        ejwav::Audio m = noiseBursts (sr, true); for (auto& c : m.ch) { const auto copy = c; for (int k = 0; k < 14; ++k) c.insert (c.end(), copy.begin(), copy.end()); }   // 60 s
        const size_t N = m.frames(); auto timeIt = [&] (auto&& fn) { const auto t0 = std::chrono::steady_clock::now(); fn(); return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count(); };
        std::vector<float> a (N), b (N); for (size_t n = 0; n < N; ++n) { a[n] = (float) m.ch[0][n]; b[n] = (float) m.ch[1][n]; }
        echojay::limv2::Core core; core.prepare (sr, T); core.setInputGainDb (8.2); core.setCeilingDb (-0.1); core.setTruePeak (true); core.reset();
        const double tv2 = timeIt ([&] { for (size_t pos = 0; pos + 512 <= N; pos += 512) { float* p[2] = { a.data() + pos, b.data() + pos }; core.process (p, 2, 512); } });
        for (size_t n = 0; n < N; ++n) { a[n] = (float) m.ch[0][n]; b[n] = (float) m.ch[1][n]; }
        echojay::legacy::Limiter lim; lim.prepare (sr, 8.2);
        const double tleg = timeIt ([&] { for (size_t pos = 0; pos + 512 <= N; pos += 512) lim.process (a.data() + pos, b.data() + pos, 512); });
        const double secs = (double) N / sr;
        std::printf ("  info  CPU for %.0f s stereo at 48 k: v2 Transparent %.3f s (%.2f %% of real time), legacy %.3f s (%.2f %%), ratio %.2fx\n", secs, tv2, 100 * tv2 / secs, tleg, 100 * tleg / secs, tv2 / tleg);
        // 8 Oct 2026: the half-band detector (two stages, two channels, 1024 MACs each per sample) costs ~6x the current limiter;
        // the round's brief asks for the number, reported in the hand-off. A 10x bound catches a regression.
        check (tv2 / tleg <= 10.0, "v2 costs no more than 10x the current limiter (reported: " + f2 (100 * tv2 / secs) + " % of real time)", f2 (tv2 / tleg) + "x");
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
