// limiter_ab_guard (session L, 7 Oct 2026): the offline A/B instrument for Limiter v2 (docs/LIMITER_V2_PLAN.md §1-2).
// It renders no audio itself and loads no plugin. It reads WAV renders, ALIGNS them against the source and asserts
// the alignment, then measures loudness, true peak and overs, the gain-reduction envelope, per-hit transient
// retention and release limbs, THD/IMD on the tone cases, pumping and channel linking - and compares every
// render against the Pro-L 2 render on the plan's stopping rule (§4).
//
//   limiter_ab_guard selftest                      - synthetic signals, every metric proven both directions
//   limiter_ab_guard gen <folder> [--rate 48000]   - write source_<case>.wav for the synthetic cases
//   limiter_ab_guard analyse <folder> [--gain 8.2] [--ceiling 0.0] [--trace] [--strict] [--case <name>]
//        files are <tag>_<case>.wav; tag "source" is the input, every other tag is a render of it
//        (proL2_fullmix.wav, echojay_fullmix.wav, v2a_fullmix.wav ...). Tags carry no underscore.
#include "ejmetrics.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check (bool ok, const std::string& what, const std::string& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : ("  [" + detail + "]").c_str());
    if (! ok) ++failures;
}
std::string f1 (double v, int d = 1) { if (std::isnan (v)) return "  n/a"; char b[64]; std::snprintf (b, sizeof b, "%.*f", d, v); return b; }
std::string f2 (double v) { return f1 (v, 2); }

// ------------------------------------------------------------------------------------------------------------
// the report
// ------------------------------------------------------------------------------------------------------------
void printReport (const ejm::Report& R, bool trace)
{
    std::printf ("\n== case %-18s tag %-10s", R.caseName.c_str(), R.tag.c_str());
    if (! R.aligned) { std::printf ("  REFUSED: %s\n", R.align.why.c_str()); for (size_t i = 0; i < R.align.windowOffsets.size(); ++i) std::printf ("     window @%7.2f s  offset %6d  corr %6.3f %s\n", (double) R.align.windowStarts[i] / R.sr, R.align.windowOffsets[i], R.align.windowPeaks[i], R.align.windowValid[i] ? "" : R.align.windowAmbiguous[i] ? "(ambiguous: periodic)" : "(ignored)"); std::printf ("ROW case=%s tag=%s aligned=0\n", R.caseName.c_str(), R.tag.c_str()); return; }
    std::printf ("  %.0f Hz  %zu frames  offset %+d samples (%.2f ms)%s  corr", R.sr, R.frames, R.align.offset, 1000.0 * R.align.offset / R.sr, R.align.tailSeconds > 0 ? ("  [render " + f1 (R.align.tailSeconds) + " s longer, tail silent]").c_str() : "");
    for (size_t i = 0; i < R.align.windowPeaks.size(); ++i) std::printf (" %.3f", R.align.windowPeaks[i]);
    std::printf ("\n  gain %+.1f dB  ceiling %+.1f dB\n", R.gainDb, R.ceilingDb);
    std::printf ("  loudness   in+gain  I %s LUFS  Mmax %s  Smax %s   |  out  I %s LUFS  Mmax %s  Smax %s   |  S std %s -> %s LU\n",
                 f2 (R.inLoud.integrated).c_str(), f1 (R.inLoud.maxMomentary).c_str(), f1 (R.inLoud.maxShortTerm).c_str(), f2 (R.outLoud.integrated).c_str(), f1 (R.outLoud.maxMomentary).c_str(), f1 (R.outLoud.maxShortTerm).c_str(), f2 (R.inLoud.stdShortTerm).c_str(), f2 (R.outLoud.stdShortTerm).c_str());
    std::printf ("  peaks      sample %s dBFS  TRUE PEAK %s dBTP  overs above ceiling+0.05 dB: %zu%s\n", f2 (R.pk.samplePeakDb).c_str(), f2 (R.pk.truePeakDb).c_str(), R.pk.overs, R.pk.overs ? ("  (worst at " + f2 (R.pk.worstSec) + " s)").c_str() : "");
    std::printf ("  dynamics   GR mean %s dB  max %s  std %s  p2p %s  |  crest in %s -> out %s dB  |  L-R GR std %s dB\n", f2 (R.dyn.grMeanDb).c_str(), f2 (R.dyn.grMaxDb).c_str(), f2 (R.dyn.grStdDb).c_str(), f2 (R.dyn.grP2pDb).c_str(), f1 (R.dyn.crestInDb).c_str(), f1 (R.dyn.crestOutDb).c_str(), f2 (R.dyn.grLRstdDb).c_str());
    if (R.isTone)
    {
        std::printf ("  tone       seg  file dB   out RMS   GR steady   fund    THD     THD+N   h2     h3     h4     h5   | imd 1k   2k   18k   21k\n");
        for (size_t i = 0; i < R.tone.segs.size(); ++i) { const auto& s = R.tone.segs[i]; std::printf ("             %2zu   %+5.1f    %6s    %6s    %6s  %6s  %6s", i, s.levelDb, f1 (s.outRmsDb).c_str(), f2 (s.grSteadyDb).c_str(), f1 (s.fundDb).c_str(), f1 (s.thdDb).c_str(), f1 (s.thdnDb).c_str()); for (size_t h = 0; h < 4; ++h) std::printf (" %6s", h < s.harmDb.size() ? f1 (s.harmDb[h]).c_str() : "     -"); std::printf ("  | %6s %6s %6s %6s\n", f1 (s.imd1k).c_str(), f1 (s.imd2k).c_str(), f1 (s.imd18k).c_str(), f1 (s.imd21k).c_str()); }
        std::printf ("  steps      t(s)   GR from -> to     t10    t50    t90 (ms)\n");
        for (const auto& s : R.tone.steps) std::printf ("             %5.1f   %6s -> %6s   %6s %6s %6s\n", s.tSec, f2 (s.fromDb).c_str(), f2 (s.toDb).c_str(), f1 (s.t10).c_str(), f1 (s.t50).c_str(), f1 (s.t90).c_str());
    }
    else
    {
        std::printf ("  hits       %zu detected%s\n", R.hits.size(), R.expectedHits ? ("  (layout: " + std::to_string (R.expectedHits) + ")").c_str() : "");
        std::printf ("              #    t(s)   over   retained   GRmin   base   pre(ms)   hold(ms)    t10    t50    t63    t90 (ms)   dipL   dipR\n");
        for (size_t i = 0; i < R.hits.size(); ++i) { const auto& h = R.hits[i]; std::printf ("             %2zu  %6.2f  %5s  %8s  %6s  %5s  %7s  %8s  %6s %6s %6s %6s   %6s %6s\n", i, h.tSec, f1 (h.overDb).c_str(), f2 (h.retentionDb).c_str(), f2 (h.grMinDb).c_str(), f1 (h.baselineDb).c_str(), f1 (h.preDipMs).c_str(), f1 (h.holdMs).c_str(), f1 (h.t10).c_str(), f1 (h.t50).c_str(), f1 (h.t63).c_str(), f1 (h.t90).c_str(), f2 (h.dipLDb).c_str(), f2 (h.dipRDb).c_str()); if (trace) { std::printf ("                 GR trace -25..+60 ms:"); for (size_t k = 0; k < h.trace.size(); ++k) { if (k % 20 == 0) std::printf ("\n                 %+3d ms ", (int) k - 25); std::printf (" %5s", std::isnan (h.trace[k]) ? "  n/a" : f1 (h.trace[k]).c_str()); } std::printf ("\n"); } }
    }
    std::vector<double> t63, t90, ret; for (const auto& h : R.hits) { t63.push_back (h.t63); t90.push_back (h.t90); ret.push_back (h.retentionDb); }
    std::printf ("ROW case=%s tag=%s aligned=1 offset=%d outI=%s inI=%s tp=%s overs=%zu grMean=%s grStd=%s crestOut=%s hits=%zu retMed=%s t63med=%s t90med=%s lrStd=%s",
                 R.caseName.c_str(), R.tag.c_str(), R.align.offset, f2 (R.outLoud.integrated).c_str(), f2 (R.inLoud.integrated).c_str(), f2 (R.pk.truePeakDb).c_str(), R.pk.overs, f2 (R.dyn.grMeanDb).c_str(), f2 (R.dyn.grStdDb).c_str(), f1 (R.dyn.crestOutDb).c_str(), R.hits.size(), f2 (ejm::median (ret)).c_str(), f1 (ejm::median (t63)).c_str(), f1 (ejm::median (t90)).c_str(), f2 (R.dyn.grLRstdDb).c_str());
    if (R.isTone && ! R.tone.segs.empty()) { const auto& s = R.tone.segs[std::min<size_t> (3, R.tone.segs.size() - 1)]; std::printf (" thdMax=%s thdnMax=%s", f1 (s.thdDb).c_str(), f1 (s.thdnDb).c_str()); }
    std::printf ("\n");
}

// ------------------------------------------------------------------------------------------------------------
// the stopping rule (plan §4): a render against the Pro-L 2 render of the same case
// ------------------------------------------------------------------------------------------------------------
void compare (const ejm::Report& ref, const ejm::Report& t, bool strict)
{
    std::printf ("\n-- rule: %s vs %s on %s\n", t.tag.c_str(), ref.tag.c_str(), t.caseName.c_str());
    auto rule = [&] (bool ok, const std::string& what, const std::string& detail) { std::printf ("  %s  %s  [%s]\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str()); if (! ok && strict) ++failures; };
    rule (std::abs (t.outLoud.integrated - ref.outLoud.integrated) <= 0.1, "level matched within 0.1 LU", f2 (t.outLoud.integrated) + " vs " + f2 (ref.outLoud.integrated) + " LUFS");
    rule (t.pk.overs == 0, "zero true-peak overs above the ceiling", std::to_string (t.pk.overs) + " overs, " + f2 (t.pk.truePeakDb) + " dBTP (Pro-L 2: " + std::to_string (ref.pk.overs) + ", " + f2 (ref.pk.truePeakDb) + " dBTP)");
    if (! t.isTone)
    {
        if (t.hits.size() != ref.hits.size()) rule (false, "same hits detected", std::to_string (t.hits.size()) + " vs " + std::to_string (ref.hits.size()));
        else if (! t.hits.empty())
        {
            double worst = 0; size_t wi = 0; for (size_t i = 0; i < t.hits.size(); ++i) { const double d = std::abs (t.hits[i].retentionDb - ref.hits[i].retentionDb); if (! std::isnan (d) && d > worst) { worst = d; wi = i; } }
            rule (worst <= 1.0, "transient retention within 1 dB of Pro-L 2 on every hit", "worst " + f2 (worst) + " dB at hit " + std::to_string (wi) + " (" + f2 (t.hits[wi].tSec) + " s)");
            std::vector<double> a63, b63, a90, b90; for (size_t i = 0; i < t.hits.size(); ++i) { a63.push_back (t.hits[i].t63); b63.push_back (ref.hits[i].t63); a90.push_back (t.hits[i].t90); b90.push_back (ref.hits[i].t90); }
            const double m63 = ejm::median (a63), r63 = ejm::median (b63), m90 = ejm::median (a90), r90 = ejm::median (b90);
            const bool ok63 = std::isnan (r63) || std::abs (m63 - r63) <= 0.1 * r63, ok90 = std::isnan (r90) || std::abs (m90 - r90) <= 0.1 * r90;
            rule (ok63 && ok90, "release limbs within 10% (median t63, t90)", "t63 " + f1 (m63) + " vs " + f1 (r63) + " ms, t90 " + f1 (m90) + " vs " + f1 (r90) + " ms");
        }
        rule (std::isnan (ref.dyn.grStdDb) || t.dyn.grStdDb <= ref.dyn.grStdDb + 0.2, "pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB", f2 (t.dyn.grStdDb) + " vs " + f2 (ref.dyn.grStdDb) + " dB");
    }
    else
    {
        // THD at the highest drive: no harmonic worse than Pro-L 2's by more than 3 dB
        const size_t si = std::min<size_t> (3, t.tone.segs.size() - 1);
        if (si < t.tone.segs.size() && si < ref.tone.segs.size())
        {
            const auto& a = t.tone.segs[si]; const auto& b = ref.tone.segs[si]; bool ok = true; std::string d;
            for (size_t h = 0; h < a.harmDb.size() && h < b.harmDb.size(); ++h) { if (a.harmDb[h] > b.harmDb[h] + 3.0 && a.harmDb[h] > -90.0) ok = false; d += "h" + std::to_string (h + 2) + " " + f1 (a.harmDb[h]) + "/" + f1 (b.harmDb[h]) + " "; }
            if (a.harmDb.empty()) d = "IMD 1k " + f1 (a.imd1k) + "/" + f1 (b.imd1k) + "  18k " + f1 (a.imd18k) + "/" + f1 (b.imd18k);
            rule (ok, "THD at the highest drive: no harmonic more than 3 dB worse than Pro-L 2", d);
        }
    }
}

// ------------------------------------------------------------------------------------------------------------
// selftest: synthetic signals with KNOWN answers; every assertion has a known-good and a known-bad leg
// ------------------------------------------------------------------------------------------------------------
ejwav::Audio noiseStereo (double sr, double secs, unsigned seed)
{
    ejwav::Audio a; a.sampleRate = sr; const size_t N = (size_t) (sr * secs); a.ch.assign (2, std::vector<double> (N));
    uint32_t s = seed; auto rnd = [&] { s = s * 1664525u + 1013904223u; return ((double) (s >> 8) / 16777216.0) * 2.0 - 1.0; };
    for (size_t n = 0; n < N; ++n) { a.ch[0][n] = 0.3 * rnd(); a.ch[1][n] = 0.3 * rnd(); }
    return a;
}
ejwav::Audio delayed (const ejwav::Audio& a, int d, double gain)
{
    ejwav::Audio b = a; for (auto& c : b.ch) for (double& v : c) v = 0; const size_t N = a.frames();
    for (size_t c = 0; c < a.ch.size(); ++c) for (size_t n = 0; n < N; ++n) { const long long i = (long long) n - d; if (i >= 0 && (size_t) i < N) b.ch[c][n] = gain * a.ch[c][(size_t) i]; }
    return b;
}
// an idealised limiter's gain envelope applied to a fixture: a linear ramp over `preMs` before each event down to
// `dipDb`, HELD for the event's length, then an exponential (in dB) recovery with time constant `tauMs`. Exact
// answers for the hit analysis.
ejwav::Audio applyEnvelope (const ejwav::Audio& src, const ejfix::Layout& L, double gainDb, double preMs, double dipDb, double tauMs, int chanMask)
{
    ejwav::Audio out = src; const double sr = src.sampleRate; const size_t N = src.frames(); const double G = ejdsp::lin (gainDb);
    std::vector<double> gdb (N, 0.0);
    for (const auto& e : L.events)
    {
        const size_t n0 = (size_t) (std::llround (e.t * sr) / 48) * 48; const size_t pre = (size_t) std::llround (preMs * 0.001 * sr);
        for (size_t n = n0 >= pre ? n0 - pre : 0; n < n0; ++n) gdb[n] = std::min (gdb[n], dipDb * (double) (n - (n0 - pre)) / (double) pre);
        const size_t hold = (size_t) std::llround (e.lengthSec * sr);
        for (size_t n = n0; n < std::min (N, n0 + hold); ++n) gdb[n] = std::min (gdb[n], dipDb);
        for (size_t n = n0 + hold; n < N; ++n) { const double v = dipDb * std::exp (-(double) (n - n0 - hold) / (tauMs * 0.001 * sr)); if (v < gdb[n]) gdb[n] = v; else if (v > -0.01) break; }
    }
    for (size_t c = 0; c < out.ch.size(); ++c) for (size_t n = 0; n < N; ++n) out.ch[c][n] = G * src.ch[c][n] * ((chanMask & (1 << c)) ? ejdsp::lin (gdb[n]) : 1.0);
    return out;
}

int selftest()
{
    const double sr = 48000.0;
    std::printf ("limiter_ab_guard selftest\n");
    {   // alignment
        const auto src = noiseStereo (sr, 10.0, 1);
        auto r = ejm::align (src, delayed (src, 480, 0.5)); check (r.ok && r.offset == 480, "alignment finds a +480-sample, -6 dB render", "offset " + std::to_string (r.offset) + " " + r.why);
        r = ejm::align (src, delayed (src, -7, 1.0)); check (r.ok && r.offset == -7, "alignment finds a -7-sample render", "offset " + std::to_string (r.offset));
        ejwav::Audio slipped = delayed (src, 480, 0.5); for (auto& c : slipped.ch) c.insert (c.begin() + (long) (5 * sr), 0.0), c.pop_back();
        r = ejm::align (src, slipped); check (! r.ok && r.why.find ("NOT constant") != std::string::npos, "a render with one sample inserted mid-file is REFUSED (offset not constant)", r.why);
        r = ejm::align (src, delayed (src, 100, -1.0)); check (! r.ok && r.why.find ("inverted") != std::string::npos, "a polarity-inverted render is REFUSED", r.why);
        r = ejm::align (src, noiseStereo (sr, 10.0, 2)); check (! r.ok, "unrelated material is REFUSED", r.why);
        ejwav::Audio clipped = delayed (src, 480, 3.0); for (auto& c : clipped.ch) for (double& v : c) v = std::max (-1.0, std::min (1.0, v));
        r = ejm::align (src, clipped); check (r.ok && r.offset == 480, "a hard-clipped (heavily limited) render still aligns", "offset " + std::to_string (r.offset) + " corr " + f2 (r.windowPeaks[0]));
        ejwav::Audio longer = delayed (src, 480, 0.5); for (auto& c : longer.ch) c.resize (c.size() + (size_t) (3 * sr), 0.0);
        r = ejm::align (src, longer); check (r.ok && r.offset == 480 && std::abs (r.tailSeconds - 3.0) < 0.01, "a fixed-length bounce 3 s longer than the source, silent tail, aligns (tail reported)", r.why + " tail " + f2 (r.tailSeconds));
        for (auto& c : longer.ch) for (size_t n = c.size() - (size_t) sr; n < c.size(); ++n) c[n] = 0.1;
        r = ejm::align (src, longer); check (! r.ok && r.why.find ("NOT silent") != std::string::npos, "...but a longer render whose tail carries signal is REFUSED", r.why);
    }
    {   // loudness
        ejwav::Audio t; t.sampleRate = sr; const size_t N = (size_t) (5 * sr); t.ch.assign (2, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) t.ch[0][n] = t.ch[1][n] = ejdsp::lin (-20.0) * std::sin (2 * ejdsp::kPi * 997.0 * (double) n / sr);
        const auto l = ejm::loudness (t.ch, sr); check (std::abs (l.integrated + 20.0) <= 0.15, "a -20 dBFS 997 Hz sine on both channels reads -20.0 LUFS (BS.1770)", f2 (l.integrated));
        const auto l2 = ejm::loudness (t.ch, sr, 8.2); check (std::abs (l2.integrated + 11.8) <= 0.15, "the same with the +8.2 dB stated gain reads -11.8 LUFS", f2 (l2.integrated));
        check (l.stdShortTerm < 0.05, "short-term std of a steady tone is ~0", f2 (l.stdShortTerm));
    }
    {   // true peak
        // every test sine is faded over 10 ms at each end: a sine that starts from silence in one sample has a REAL
        // inter-sample overshoot (Gibbs), and the meter must report it - so the edges are not what is tested here
        const size_t N = (size_t) (2 * sr); auto fade = [&] (size_t n) { const size_t e = (size_t) (0.01 * sr); const double k = n < e ? (double) n / e : (N - n <= e ? (double) (N - n) / e : 1.0); return 0.5 - 0.5 * std::cos (ejdsp::kPi * k); };
        ejwav::Audio t; t.sampleRate = sr; t.ch.assign (1, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) t.ch[0][n] = fade (n) * std::sin (2 * ejdsp::kPi * 12000.0 * (double) n / sr + ejdsp::kPi / 4);
        const auto p = ejm::peaks (t.ch, sr, 0.0); check (std::abs (p.samplePeakDb + 3.01) < 0.05 && std::abs (p.truePeakDb) < 0.02, "fs/4 sine at 45 deg: sample peak -3.01 dBFS, true peak 0.0 dBTP (+-0.02)", f2 (p.samplePeakDb) + " / " + f2 (p.truePeakDb));
        ejwav::Audio t20; t20.sampleRate = sr; t20.ch.assign (1, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) t20.ch[0][n] = fade (n) * std::sin (2 * ejdsp::kPi * 20000.0 * (double) n / sr + 0.3);
        const auto p20 = ejm::peaks (t20.ch, sr, 0.0); check (std::abs (p20.truePeakDb) < 0.05, "20 kHz sine at 0 dBFS: true peak 0.0 dBTP (+-0.05)", f2 (p20.samplePeakDb) + " / " + f2 (p20.truePeakDb));
        ejwav::Audio t1; t1.sampleRate = sr; t1.ch.assign (1, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) t1.ch[0][n] = fade (n) * std::sin (2 * ejdsp::kPi * 997.0 * (double) n / sr);
        const auto p1 = ejm::peaks (t1.ch, sr, 0.0); check (std::abs (p1.truePeakDb) < 0.01, "997 Hz sine at 0 dBFS: true peak 0.0 dBTP (+-0.01)", f2 (p1.truePeakDb));
        { ejwav::Audio hard; hard.sampleRate = sr; hard.ch.assign (1, std::vector<double> (N)); for (size_t n = 0; n < N; ++n) hard.ch[0][n] = std::sin (2 * ejdsp::kPi * 20000.0 * (double) n / sr + 0.3); const auto ph = ejm::peaks (hard.ch, sr, 0.0); check (ph.truePeakDb > 0.3, "a 20 kHz sine that starts from silence in one sample overs by its Gibbs overshoot (the meter sees the edge)", f2 (ph.truePeakDb)); }
        check (ejm::peaks (t.ch, sr, -0.5).overs > 0, "overs are counted against a -0.5 dB ceiling", std::to_string (ejm::peaks (t.ch, sr, -0.5).overs));
        check (ejm::peaks (t.ch, sr, 0.2).overs == 0, "no overs against a +0.2 dB ceiling", std::to_string (ejm::peaks (t.ch, sr, 0.2).overs));
    }
    {   // THD and the tone steps, on the tone_997 layout with a synthetic limiter: steady GR = -(level + 8.2) where
        // positive, reached through a 50 ms (dB-domain) one-pole at each boundary; plus a 3rd harmonic at -40 dB
        const auto L = ejfix::layout ("tone_997"); const auto src = ejfix::generate (L, sr);
        ejwav::Audio out = src; const size_t N = src.frames(); double g = 0; const double coef = 1.0 - std::exp (-1.0 / (0.05 * sr));
        for (size_t n = 0; n < N; ++n)
        {
            double target = 0; for (const auto& s : L.segments) if ((double) n / sr >= s.t0 && (double) n / sr < s.t1) target = std::min (0.0, -(s.levelDb + 8.2));
            g += (target - g) * coef; const double v = ejdsp::lin (8.2) * ejdsp::lin (g);
            for (int c = 0; c < 2; ++c) out.ch[(size_t) c][n] = v * (src.ch[(size_t) c][n] + 0.01 * std::sin (2 * ejdsp::kPi * 3 * 997.0 * (double) n / sr));   // h3 at -40 dB relative to the 0 dBFS segment
        }
        {   // the known-bad leg first: WITHOUT the markers a pure tone is ambiguous and must be refused, not guessed
            ejwav::Audio pure; pure.sampleRate = sr; pure.ch.assign (2, std::vector<double> ((size_t) (10 * sr))); for (size_t n = 0; n < pure.frames(); ++n) pure.ch[0][n] = pure.ch[1][n] = 0.5 * std::sin (2 * ejdsp::kPi * 997.0 * (double) n / sr);
            const auto un = ejm::align (pure, delayed (pure, 96, 1.0));
            check (! un.ok && un.why.find ("AMBIGUOUS") != std::string::npos, "a pure tone with no markers, delayed two periods, is REFUSED as ambiguous rather than aligned at a wrong period", un.why);
        }
        const auto R = ejm::analyse ("tone_997", "synth", src, delayed (out, 96, 1.0), 8.2, 0.0);
        check (R.aligned && R.align.offset == 96, "tone_997 synthetic, delayed 96 samples (two periods), aligns at +96 on the markers", R.align.why + " offset " + std::to_string (R.align.offset));
        const bool six = R.tone.segs.size() == 6 && R.tone.segs[3].harmDb.size() >= 2 && R.tone.steps.size() == 5;
        check (six, "six tone segments and five steps analysed", std::to_string (R.tone.segs.size()));
        if (six)
        {
            check (std::abs (R.tone.segs[3].grSteadyDb + 8.2) < 0.1 && std::abs (R.tone.segs[0].grSteadyDb) < 0.05, "steady GR per segment: 0 at -8.2 dBFS, -8.2 at 0 dBFS", f2 (R.tone.segs[0].grSteadyDb) + " / " + f2 (R.tone.segs[3].grSteadyDb));
            check (std::abs (R.tone.segs[3].harmDb[1] + 40.0) < 0.3 && std::abs (R.tone.segs[3].thdDb + 40.0) < 0.3, "h3 at -40 dB reads as -40 dB THD", f1 (R.tone.segs[3].harmDb[1]) + " / " + f1 (R.tone.segs[3].thdDb));
            check (std::abs (R.tone.steps[2].t50 - 34.7) < 5.0 && std::abs (R.tone.steps[2].t90 - 115.1) < 12.0, "step response of a 50 ms one-pole: t50 ~35 ms, t90 ~115 ms", f1 (R.tone.steps[2].t50) + " / " + f1 (R.tone.steps[2].t90));
            check (R.pk.overs > 0 && R.pk.truePeakDb > 0.0, "the synthetic (which does not hold a ceiling) shows overs", std::to_string (R.pk.overs));
        }
    }
    {   // hits: the probe layout under an idealised envelope - 5 ms linear pre-dip to -6 dB, tau 100 ms
        const auto L = ejfix::layout ("probe_transients"); const auto src = ejfix::generate (L, sr);
        const auto out = applyEnvelope (src, L, 8.2, 5.0, -6.0, 100.0, 3);
        const auto R = ejm::analyse ("probe_transients", "synth", src, out, 8.2, 0.0);
        check (R.aligned && R.hits.size() == L.events.size(), "onset detector finds every layout event on probe_transients", std::to_string (R.hits.size()) + " of " + std::to_string (L.events.size()) + " " + R.align.why);
        if (R.hits.size() == L.events.size())
        {
            double worstRet = 0, worstPre = 0, worst63 = 0, worst90 = 0, worstMin = 0, worstHold = 0;
            for (size_t i = 0; i < R.hits.size(); ++i) { const auto& h = R.hits[i]; worstRet = std::max (worstRet, std::abs (h.retentionDb + 6.0)); worstPre = std::max (worstPre, std::abs (h.preDipMs - 5.0)); worst63 = std::max (worst63, std::abs (h.t63 - 100.0) / 100.0); worst90 = std::max (worst90, std::abs (h.t90 - 230.3) / 230.3); worstMin = std::max (worstMin, std::abs (h.grMinDb + 6.0)); worstHold = std::max (worstHold, std::abs (h.holdMs - 1000.0 * L.events[i].lengthSec)); }
            check (worstHold < 5.0, "hold = the burst length on every hit (0 .. 1000 ms, +3.4 ms for the 0.2 dB rise)", "worst error " + f2 (worstHold) + " ms");
            check (worstRet < 0.3, "retention = -6.0 dB on every hit", "worst error " + f2 (worstRet));
            check (worstMin < 0.3, "GR minimum = -6.0 dB on every hit", "worst error " + f2 (worstMin));
            check (worstPre < 1.5, "pre-dip (lookahead) = 5 ms on every hit", "worst error " + f2 (worstPre) + " ms");
            check (worst63 < 0.1 && worst90 < 0.1, "release limbs t63 = 100 ms, t90 = 230 ms within 10%", "worst " + f2 (100 * worst63) + "% / " + f2 (100 * worst90) + "%");
            check (std::abs (R.hits[0].overDb - 8.2) < 0.2, "hit 0 is +8.2 dB over the ceiling", f2 (R.hits[0].overDb));
        }
        const auto R2 = ejm::analyse ("probe_transients", "synth2", src, applyEnvelope (src, L, 8.2, 5.0, -6.0, 30.0, 3), 8.2, 0.0);
        check (! R2.hits.empty() && std::abs (ejm::median ([&] { std::vector<double> v; for (auto& h : R2.hits) v.push_back (h.t63); return v; }()) - 30.0) < 3.0, "a 30 ms tau is measured as ~30 ms (the fit is not stuck on a constant)", f1 (R2.hits.empty() ? ejm::NaN : R2.hits[0].t63));
    }
    {   // stereo linking on the panned layout: unlinked (L only dips) vs linked
        const auto L = ejfix::layout ("panned_transient"); const auto src = ejfix::generate (L, sr);
        const auto Ru = ejm::analyse ("panned_transient", "unlinked", src, applyEnvelope (src, L, 8.2, 5.0, -6.0, 100.0, 1), 8.2, 0.0);
        const auto Rl = ejm::analyse ("panned_transient", "linked", src, applyEnvelope (src, L, 8.2, 5.0, -6.0, 100.0, 3), 8.2, 0.0);
        check (Ru.hits.size() == 12 && Rl.hits.size() == 12, "12 panned events detected", std::to_string (Ru.hits.size()) + " / " + std::to_string (Rl.hits.size()) + " " + Ru.align.why);
        if (Ru.hits.size() == 12 && Rl.hits.size() == 12)
        {
            check (std::abs (Ru.hits[0].dipLDb + 6.0) < 0.3 && std::abs (Ru.hits[0].dipRDb) < 0.3, "unlinked: L dips 6 dB, R does not", f2 (Ru.hits[0].dipLDb) + " / " + f2 (Ru.hits[0].dipRDb));
            check (std::abs (Rl.hits[0].dipLDb + 6.0) < 0.3 && std::abs (Rl.hits[0].dipRDb + 6.0) < 0.3, "linked: both channels dip 6 dB", f2 (Rl.hits[0].dipLDb) + " / " + f2 (Rl.hits[0].dipRDb));
            check (Ru.dyn.grLRstdDb > 0.1 && Rl.dyn.grLRstdDb < 0.02, "L-R GR std separates unlinked from linked", f2 (Ru.dyn.grLRstdDb) + " / " + f2 (Rl.dyn.grLRstdDb));
        }
    }
    {   // pumping: sustained material, gain modulated +-1 dB at 0.5 Hz vs steady (momentary blocks are 400 ms)
        const auto src = noiseStereo (sr, 10.0, 5); const size_t N = src.frames();   // broadband, so it aligns; a pure tone would be refused as ambiguous
        ejwav::Audio pump = src, steady = src; for (size_t n = 0; n < N; ++n) { const double m = ejdsp::lin (std::sin (2 * ejdsp::kPi * 0.5 * (double) n / sr)); for (int c = 0; c < 2; ++c) { pump.ch[(size_t) c][n] *= 0.5 * m; steady.ch[(size_t) c][n] *= 0.5; } }
        const auto Rp = ejm::analyse ("bass_sustain", "pump", src, pump, 0.0, 0.0), Rs = ejm::analyse ("bass_sustain", "steady", src, steady, 0.0, 0.0);
        check (Rp.dyn.grStdDb > 0.5 && Rp.dyn.grStdDb < 0.8 && Rs.dyn.grStdDb < 0.01, "momentary GR std: +-1 dB at 0.5 Hz reads 0.5-0.8 dB (0.707 unaveraged), steady ~0", f2 (Rp.dyn.grStdDb) + " / " + f2 (Rs.dyn.grStdDb) + " " + Rp.align.why + Rs.align.why);
        check (std::abs (Rs.dyn.grMeanDb + 6.02) < 0.05, "steady -6 dB render reads GR mean -6.02", f2 (Rs.dyn.grMeanDb));
    }
    {   // WAV round trip
        const auto a = noiseStereo (sr, 1.0, 3); const std::string path = "/tmp/limiter_ab_guard_selftest.wav"; ejwav::writeFloat32 (path, a); const auto b = ejwav::read (path); std::remove (path.c_str());
        double worst = 0; for (size_t c = 0; c < 2; ++c) for (size_t n = 0; n < a.frames(); ++n) worst = std::max (worst, std::abs (a.ch[c][n] - b.ch[c][n]));
        check (b.sampleRate == sr && b.frames() == a.frames() && b.channels() == 2 && worst < 1e-6, "float32 WAV round trip", b.sourceFormat + " worst " + std::to_string (worst));
    }
    std::printf ("\n==== limiter_ab_guard selftest: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------------------
int gen (const std::string& folder, double rate)
{
    for (const auto& name : ejfix::syntheticNames())
    {
        const auto L = ejfix::layout (name); const auto a = ejfix::generate (L, rate); const std::string path = folder + "/source_" + name + ".wav";
        ejwav::writeFloat32 (path, a); std::printf ("wrote %s  (%.1f s, %.0f Hz, float32 stereo)\n   %s\n", path.c_str(), L.seconds, rate, L.description.c_str());
    }
    return 0;
}

int analyse (const std::string& folder, double gainDb, double ceilingDb, bool trace, bool strict, const std::string& onlyCase)
{
    std::map<std::string, std::map<std::string, std::string>> cases;   // case -> tag -> path
    DIR* d = opendir (folder.c_str()); if (! d) { std::fprintf (stderr, "cannot open %s\n", folder.c_str()); return 2; }
    while (dirent* e = readdir (d)) { std::string f = e->d_name; if (f.size() < 5 || f.substr (f.size() - 4) != ".wav") continue; const auto us = f.find ('_'); if (us == std::string::npos) continue; const std::string tag = f.substr (0, us), cs = f.substr (us + 1, f.size() - 4 - us - 1); if (! onlyCase.empty() && cs != onlyCase) continue; cases[cs][tag] = folder + "/" + f; }
    closedir (d);
    if (cases.empty()) { std::fprintf (stderr, "no <tag>_<case>.wav files in %s\n", folder.c_str()); return 2; }
    std::printf ("limiter_ab_guard analyse %s  gain %+.1f dB  ceiling %+.1f dB\n", folder.c_str(), gainDb, ceilingDb);
    for (auto& [cs, tags] : cases)
    {
        if (! tags.count ("source")) { std::printf ("\n== case %s: no source_%s.wav - skipped\n", cs.c_str(), cs.c_str()); ++failures; continue; }
        ejwav::Audio src; try { src = ejwav::read (tags["source"]); } catch (const std::exception& ex) { std::printf ("\n== case %s: %s\n", cs.c_str(), ex.what()); ++failures; continue; }
        std::printf ("\n#### case %s: source %s (%s, %.0f Hz, %d ch, %.2f s)\n", cs.c_str(), tags["source"].c_str(), src.sourceFormat.c_str(), src.sampleRate, src.channels(), (double) src.frames() / src.sampleRate);
        std::map<std::string, ejm::Report> reports;
        for (auto& [tag, path] : tags)
        {
            if (tag == "source") continue;
            ejwav::Audio proc; try { proc = ejwav::read (path); } catch (const std::exception& ex) { std::printf ("\n== case %s tag %s: %s\n", cs.c_str(), tag.c_str(), ex.what()); ++failures; continue; }
            if (proc.sampleRate != src.sampleRate) { std::printf ("\n== case %s tag %s: REFUSED, sample rate %.0f vs source %.0f\n", cs.c_str(), tag.c_str(), proc.sampleRate, src.sampleRate); ++failures; continue; }
            auto R = ejm::analyse (cs, tag, src, proc, gainDb, ceilingDb); printReport (R, trace); if (! R.aligned) ++failures; reports[tag] = std::move (R);
        }
        if (reports.count ("proL2") && reports["proL2"].aligned) for (auto& [tag, R] : reports) if (tag != "proL2" && R.aligned) compare (reports["proL2"], R, strict);
    }
    std::printf ("\n==== limiter_ab_guard analyse: %s (%d problem(s)) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::fprintf (stderr, "usage: limiter_ab_guard selftest | gen <folder> [--rate 48000] | analyse <folder> [--gain 8.2] [--ceiling 0.0] [--trace] [--strict] [--case <name>]\n"); return 2; }
    const std::string cmd = argv[1];
    double gain = 8.2, ceiling = 0.0, rate = 48000.0; bool trace = false, strict = false; std::string folder, onlyCase;
    for (int i = 2; i < argc; ++i) { const std::string a = argv[i]; if (a == "--gain" && i + 1 < argc) gain = std::atof (argv[++i]); else if (a == "--ceiling" && i + 1 < argc) ceiling = std::atof (argv[++i]); else if (a == "--rate" && i + 1 < argc) rate = std::atof (argv[++i]); else if (a == "--trace") trace = true; else if (a == "--strict") strict = true; else if (a == "--case" && i + 1 < argc) onlyCase = argv[++i]; else folder = a; }
    if (cmd == "selftest") return selftest();
    if (cmd == "gen" && ! folder.empty()) return gen (folder, rate);
    if (cmd == "analyse" && ! folder.empty()) return analyse (folder, gain, ceiling, trace, strict, onlyCase);
    std::fprintf (stderr, "bad arguments\n"); return 2;
}
