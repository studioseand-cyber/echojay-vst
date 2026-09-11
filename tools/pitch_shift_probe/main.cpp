// pitch_shift_probe — the SHIFTER in isolation (28 Aug 2026, the roughness
// investigation). A steady synthetic periodic tone through PsolaEngine at
// constant shifts, reporting cycle-to-cycle similarity and HNR against
// shift, on three paths: the splice band (shift mode), the grain path
// (splice force-disabled), and the legacy target/f0 mode. Also measures the
// PHASE SLIP at every splice as executed — one period before vs after each
// recorded splice, best-lag cross-correlation — because a splice one or two
// samples off produces exactly the field signature: successive periods that
// stop matching, with no click and no HF splatter.
//
// The f0 fed here is EXACT (the tone's own), which is the discriminator:
//   - roughness that grows with shift even at exact f0  -> the machinery
//   - clean here but rough in the field                 -> f0-error-driven
//
// Build: g++ -std=c++17 -O2 -ISource tools/pitch_shift_probe/main.cpp -o probe

#include "EedPsolaEngine.h"

#include <cmath>
#include <cstdio>
#include <vector>

using echojay::PsolaEngine;

static constexpr double kFs = 48000.0;
static constexpr float  kF0 = 110.0f;
static constexpr int    kBlock = 256;
static constexpr double kSeconds = 5.0;

// Harmonic-rich periodic tone (1/n rolloff, 12 harmonics) — similarity
// ceiling is 1.0 by construction.
static std::vector<float> makeTone()
{
    const int n = (int) (kFs * kSeconds);
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        double s = 0.0;
        for (int h = 1; h <= 12; ++h)
            s += std::sin (2.0 * M_PI * kF0 * h * i / kFs) / (double) h;
        v[(size_t) i] = (float) (0.25 * s);
    }
    return v;
}

// Best-lag (+-2) normalised cross-correlation of two length-T windows.
static double ncc (const std::vector<float>& x, long a, long b, int T)
{
    double best = -2.0;
    for (int lag = -2; lag <= 2; ++lag)
    {
        double sab = 0, saa = 0, sbb = 0;
        for (int i = 0; i < T; ++i)
        {
            const double xa = x[(size_t) (a + i)];
            const double xb = x[(size_t) (b + i + lag)];
            sab += xa * xb; saa += xa * xa; sbb += xb * xb;
        }
        if (saa > 1e-12 && sbb > 1e-12)
            best = std::max (best, sab / std::sqrt (saa * sbb));
    }
    return best;
}

struct Stats { double simMedian, simP10, hnrDb; };

static Stats measure (const std::vector<float>& out, int latency, double outT)
{
    const int T = (int) std::lround (outT);
    std::vector<double> sims;
    long start = latency + (long) (0.5 * kFs);          // settle
    for (long a = start; a + 2 * T + 4 < (long) out.size() - T; a += T)
        sims.push_back (ncc (out, a, a + T, T));
    std::sort (sims.begin(), sims.end());
    Stats s {};
    s.simMedian = sims.empty() ? 0 : sims[sims.size() / 2];
    s.simP10    = sims.empty() ? 0 : sims[sims.size() / 10];
    // HNR from the median period autocorrelation r: 10log10(r/(1-r)).
    const double r = std::clamp (s.simMedian, 1e-6, 1.0 - 1e-9);
    s.hnrDb = 10.0 * std::log10 (r / (1.0 - r));
    return s;
}

int main()
{
    const auto in = makeTone();
    const float shifts[] = { 0, 5, 10, 20, 40, 80, 200, 400 };

    std::printf ("path, shift_c, sim_median, sim_p10, hnr_db, splices, "
                 "slip_med_smp, slip_max_smp\n");

    for (int mode = 0; mode < 3; ++mode)   // 0 shift/splice, 1 shift/grain, 2 legacy
        for (float sc : shifts)
        {
            PsolaEngine e;
            float worst = 25.0f;
            e.prepare (kFs, kBlock, 55.0f, worst);
            e.debugRecordPhaseEvents (true);
            if (mode == 1) e.debugDisableSplice (true);

            const float target = kF0 * std::pow (2.0f, sc / 1200.0f);
            std::vector<float> out (in.size(), 0.0f);
            for (size_t p = 0; p + kBlock <= in.size(); p += kBlock)
                e.process (in.data() + p, out.data() + p, kBlock, kF0, true,
                           target,
                           mode == 2 ? PsolaEngine::kNoShift : sc);

            const int latency = e.latencySamples();
            const double outT = kFs / (double) target;
            const auto st = measure (out, latency, outT);

            // Phase slip at each executed splice: one period before vs one
            // period after (skipping the ~4ms crossfade), best-lag xcorr —
            // the lag IS the slip. Only meaningful on the splice path.
            std::vector<double> slips;
            const int T = (int) std::lround (outT);
            const int fade = (int) (0.004 * kFs) + 4;
            for (uint64_t sp : e.debugSplices())
            {
                const long o = (long) sp + latency;      // output index
                if (o - T < latency || o + fade + 2 * T + 8 >= (long) out.size())
                    continue;
                double best = -2.0; int bestLag = 0;
                for (int lag = -8; lag <= 8; ++lag)
                {
                    double sab = 0, saa = 0, sbb = 0;
                    for (int i = 0; i < T; ++i)
                    {
                        const double xa = out[(size_t) (o - T + i)];
                        const double xb = out[(size_t) (o + fade + i + lag)];
                        sab += xa * xb; saa += xa * xa; sbb += xb * xb;
                    }
                    const double v = (saa > 1e-12 && sbb > 1e-12)
                                       ? sab / std::sqrt (saa * sbb) : -2.0;
                    if (v > best) { best = v; bestLag = lag; }
                }
                // slip = residual after removing the whole periods the
                // fade+lag span: phase error modulo the period.
                double ph = std::fmod ((double) fade + bestLag, outT);
                if (ph > outT / 2) ph -= outT;
                slips.push_back (std::fabs (ph));
            }
            std::sort (slips.begin(), slips.end());
            const double slipMed = slips.empty() ? 0 : slips[slips.size() / 2];
            const double slipMax = slips.empty() ? 0 : slips.back();

            static const char* names[3] = { "splice", "grain", "legacy" };
            std::printf ("%s, %5.0f, %.4f, %.4f, %6.2f, %3d, %5.2f, %5.2f\n",
                         names[mode], sc, st.simMedian, st.simP10, st.hnrDb,
                         (int) e.debugSplices().size(), slipMed, slipMax);
        }
    // ---- EXPERIMENT 2: the field's ingredients, one at a time ----------
    // The steady sweep above is CLEAN at exact f0 — so the roughness needs
    // something the field has: a MOVING f0 (vibrato), detection CADENCE
    // (f0 held across hops), or detection ERROR. Constant 40c shift (the
    // field's rough-moment magnitude), one ingredient per row.
    std::printf ("\nexp2: vibrato tone, shift=40c, splice path\n");
    std::printf ("f0feed, sim_median, sim_p10, rough_per_s, splices\n");
    {
        const int n = (int) (kFs * kSeconds);
        std::vector<float> vib ((size_t) n);
        std::vector<float> truef0 ((size_t) n);
        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double f = kF0 * std::pow (2.0,
                (30.0 / 1200.0) * std::sin (2.0 * M_PI * 6.0 * i / kFs));
            truef0[(size_t) i] = (float) f;
            double s = 0.0;
            double hph = ph;
            for (int h = 1; h <= 12; ++h)
            { s += std::sin (2.0 * M_PI * hph) / (double) h; hph += ph; }
            // simple harmonic stack on a common phase accumulator
            s = 0.0; for (int h = 1; h <= 12; ++h) s += std::sin (2.0 * M_PI * h * ph) / (double) h;
            vib[(size_t) i] = (float) (0.25 * s);
            ph += f / kFs;
        }
        // the tone's OWN similarity ceiling (source column)
        {
            // measure the input against itself at its average period
            PsolaEngine dummy; (void) dummy;
        }
        const float sc = 40.0f;
        const float target = kF0 * std::pow (2.0f, sc / 1200.0f);
        for (int feed = 0; feed < 3; ++feed)   // 0 true/sample, 1 hop-held, 2 hop+err
        {
            PsolaEngine e;
            e.prepare (kFs, kBlock, 55.0f, 25.0f);
            e.debugRecordPhaseEvents (true);
            std::vector<float> out ((size_t) n, 0.0f);
            unsigned rng = 12345;
            float held = kF0, heldErr = kF0;
            for (int p = 0; p + kBlock <= n; p += kBlock)
            {
                // per-hop (128) update inside the block, like the detector
                for (int off = 0; off < kBlock; off += 128)
                {
                    const float f = truef0[(size_t) (p + off)];
                    rng = rng * 1664525u + 1013904223u;
                    const float err = 1.0f + 0.01f * (((rng >> 8) & 0xFFFF) / 65535.0f - 0.5f);
                    held = f; heldErr = f * err;
                    const float use = feed == 0 ? 0.0f    // per-sample truth: pass below
                                    : feed == 1 ? held : heldErr;
                    const int len = std::min (128, kBlock - off);
                    if (feed == 0)
                        e.process (vib.data() + p + off, out.data() + p + off, len,
                                   truef0[(size_t) (p + off)], true, target, sc);
                    else
                        e.process (vib.data() + p + off, out.data() + p + off, len,
                                   use, true, target, sc);
                }
            }
            const int latency = e.latencySamples();
            const double outT = kFs / (double) target;
            const auto st = measure (out, latency, outT);
            // rough spans per second: consecutive-period sim below 0.90
            const int T = (int) std::lround (outT);
            int rough = 0, total = 0;
            for (long a = latency + (long) (0.5 * kFs);
                 a + 2 * T + 4 < (long) out.size() - T; a += T)
            { ++total; if (ncc (out, a, a + T, T) < 0.90) ++rough; }
            const double per_s = rough / (kSeconds - 0.5);
            static const char* fn[3] = { "true/hop", "held/hop", "held+1%err" };
            std::printf ("%s, %.4f, %.4f, %.2f, %d\n",
                         fn[feed], st.simMedian, st.simP10, per_s,
                         (int) e.debugSplices().size());
        }
    }
    // ---- EXPERIMENT 3: voiced/unvoiced SEAMS (syllable rate) -----------
    // The field count (~6 rough spans/sec) matches syllable rate, not
    // splice rate. At every unvoiced gap the engine emits the DELAYED DRY
    // (unshifted) and crossfades back to WET (shifted): at 40c the two
    // sides differ in pitch AND phase — faded, so no click, no HF splatter,
    // just periods that stop matching. Vibrato tone, 60ms unvoiced gap
    // every 500ms (f0 reported 0/unvoiced through the gap, tone continues).
    std::printf ("\nexp3: v/uv seams, vibrato tone, splice path\n");
    std::printf ("shift_c, sim_median, sim_p10, rough_per_s\n");
    {
        const int n = (int) (kFs * kSeconds);
        std::vector<float> vib ((size_t) n);
        std::vector<float> truef0 ((size_t) n);
        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double f = kF0 * std::pow (2.0,
                (30.0 / 1200.0) * std::sin (2.0 * M_PI * 6.0 * i / kFs));
            truef0[(size_t) i] = (float) f;
            double s = 0.0;
            for (int h = 1; h <= 12; ++h) s += std::sin (2.0 * M_PI * h * ph) / (double) h;
            vib[(size_t) i] = (float) (0.25 * s);
            ph += f / kFs;
        }
        for (float sc : { 0.0f, 10.0f, 40.0f, 80.0f })
        {
            PsolaEngine e;
            e.prepare (kFs, kBlock, 55.0f, 25.0f);
            const float target = kF0 * std::pow (2.0f, sc / 1200.0f);
            std::vector<float> out ((size_t) n, 0.0f);
            for (int p = 0; p + kBlock <= n; p += kBlock)
                for (int off = 0; off < kBlock; off += 128)
                {
                    const int at = p + off;
                    const bool uv = (at % 24000) < 2880;   // 60ms gap / 500ms
                    e.process (vib.data() + at, out.data() + at,
                               std::min (128, kBlock - off),
                               uv ? 0.0f : truef0[(size_t) at], ! uv,
                               target, sc);
                }
            const int latency = e.latencySamples();
            const double outT = kFs / (double) target;
            const auto st = measure (out, latency, outT);
            const int T = (int) std::lround (outT);
            int rough = 0, nearEntry = 0, nearExit = 0, elsewhere = 0;
            for (long a = latency + (long) (0.5 * kFs);
                 a + 2 * T + 4 < (long) out.size() - T; a += T)
                if (ncc (out, a, a + T, T) < 0.90)
                {
                    ++rough;
                    // Which seam edge is this window near, in INPUT time?
                    const long ip = a - latency;          // input position
                    const long inCycle = ip % 24000;
                    // entry seam (voiced->uv) at 0; exit (uv->voiced) at 2880
                    const long dEntry = std::min (inCycle, 24000 - inCycle);
                    const long dExit  = std::labs (inCycle - 2880);
                    if      (dExit  < 3 * T) ++nearExit;
                    else if (dEntry < 3 * T) ++nearEntry;
                    else                     ++elsewhere;
                }
            std::printf ("%5.0f, %.4f, %.4f, %.2f  (entry %d, exit %d, other %d)\n",
                         sc, st.simMedian, st.simP10, rough / (kSeconds - 0.5),
                         nearEntry, nearExit, elsewhere);
        }
    }

    // ---- EXPERIMENT 3b: the SEAM FLOOR — an ideal shifter with the same
    // v/uv pattern. Voiced spans resampled at constant r (cubic, started
    // PHASE-ALIGNED with the dry at each entry), unvoiced spans bit-exact
    // dry, 1.5ms linear crossfades. The pitch STEP wet<->dry at every seam
    // is genuine and the metric charges for it — whatever this scores is
    // the floor for "correct the vowels, keep consonants dry".
    std::printf ("\nexp3b: ideal seam control\n");
    std::printf ("shift_c, sim_median, sim_p10, rough_per_s\n");
    {
        const int n = (int) (kFs * kSeconds);
        std::vector<float> vib ((size_t) n);
        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double f = kF0 * std::pow (2.0,
                (30.0 / 1200.0) * std::sin (2.0 * M_PI * 6.0 * i / kFs));
            double sg = 0.0;
            for (int h = 1; h <= 12; ++h) sg += std::sin (2.0 * M_PI * h * ph) / (double) h;
            vib[(size_t) i] = (float) (0.25 * sg);
            ph += f / kFs;
        }
        auto cubic = [&] (double x) -> float
        {
            const long i1 = (long) x;
            if (i1 < 1 || i1 + 2 >= n) return 0.0f;
            const double fr = x - (double) i1;
            const double xm = vib[(size_t) (i1 - 1)], x0 = vib[(size_t) i1],
                         x1 = vib[(size_t) (i1 + 1)], x2 = vib[(size_t) (i1 + 2)];
            return (float) (x0 + 0.5 * fr * (x1 - xm
                          + fr * (2.0 * xm - 5.0 * x0 + 4.0 * x1 - x2
                          + fr * (3.0 * (x0 - x1) + x2 - xm))));
        };
        for (float sc : { 0.0f, 40.0f, 80.0f })
        {
            const double r = std::pow (2.0, sc / 1200.0);
            std::vector<float> out ((size_t) n, 0.0f);
            const int fadeN = 72;
            for (int i = 0; i < n; ++i)
            {
                const int inCycle = i % 24000;
                const bool uv = inCycle < 2880;
                if (uv) { out[(size_t) i] = vib[(size_t) i]; continue; }
                // voiced: resample from the span's own start, phase-aligned
                const int spanStart = (i / 24000) * 24000 + 2880;
                const double pos = (double) spanStart + (double) (i - spanStart) * r;
                float wet = cubic (pos);
                // fade near both edges (voiced side), 72 samples
                const int fromStart = i - spanStart;
                const int toEnd = (spanStart + 24000 - 2880) - i;
                const int edge = std::min (fromStart, toEnd);
                const float g = edge >= fadeN ? 1.0f
                              : (float) (edge + 1) / (float) (fadeN + 1);
                out[(size_t) i] = vib[(size_t) i]
                                + g * (wet - vib[(size_t) i]);
            }
            const double outT = kFs / (double) (kF0 * std::pow (2.0, sc / 1200.0));
            const auto st = measure (out, 0, outT);
            const int T = (int) std::lround (outT);
            int rough = 0;
            for (long a = (long) (0.5 * kFs);
                 a + 2 * T + 4 < (long) out.size() - T; a += T)
                if (ncc (out, a, a + T, T) < 0.90) ++rough;
            std::printf ("%5.0f, %.4f, %.4f, %.2f\n", sc,
                         st.simMedian, st.simP10, rough / (kSeconds - 0.5));
        }
    }

    // ---- EXPERIMENT 4: MOVING shift (retune dynamics) ------------------
    // Steady vibrato tone, shift ramping between 0 and 80c at 3 Hz —
    // roughly what correction across note transitions does to the ratio.
    std::printf ("\nexp4: moving shift 0..80c @3Hz, vibrato tone, splice path\n");
    {
        const int n = (int) (kFs * kSeconds);
        std::vector<float> vib ((size_t) n);
        std::vector<float> truef0 ((size_t) n);
        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double f = kF0 * std::pow (2.0,
                (30.0 / 1200.0) * std::sin (2.0 * M_PI * 6.0 * i / kFs));
            truef0[(size_t) i] = (float) f;
            double s = 0.0;
            for (int h = 1; h <= 12; ++h) s += std::sin (2.0 * M_PI * h * ph) / (double) h;
            vib[(size_t) i] = (float) (0.25 * s);
            ph += f / kFs;
        }
        PsolaEngine e;
        e.prepare (kFs, kBlock, 55.0f, 25.0f);
        std::vector<float> out ((size_t) n, 0.0f);
        for (int p = 0; p + kBlock <= n; p += kBlock)
            for (int off = 0; off < kBlock; off += 128)
            {
                const int at = p + off;
                const float sc = 40.0f
                    + 40.0f * (float) std::sin (2.0 * M_PI * 3.0 * at / kFs);
                const float target = kF0 * std::pow (2.0f, sc / 1200.0f);
                e.process (vib.data() + at, out.data() + at,
                           std::min (128, kBlock - off),
                           truef0[(size_t) at], true, target, sc);
            }
        const int latency = e.latencySamples();
        const double outT = kFs / (double) (kF0 * std::pow (2.0f, 40.0f / 1200.0f));
        const auto st = measure (out, latency, outT);
        const int T = (int) std::lround (outT);
        int rough = 0;
        for (long a = latency + (long) (0.5 * kFs);
             a + 2 * T + 4 < (long) out.size() - T; a += T)
            if (ncc (out, a, a + T, T) < 0.90) ++rough;
        std::printf ("sim_median %.4f, sim_p10 %.4f, rough_per_s %.2f\n",
                     st.simMedian, st.simP10, rough / (kSeconds - 0.5));
    }
    // ---- EXPERIMENT 4b: the METRIC'S OWN FLOOR -------------------------
    // The same 0..80c @3Hz trajectory through an IDEAL time-varying
    // resampler (cubic interpolation, moving read rate - it cannot
    // glitch). Whatever this scores is what the fixed-period similarity
    // metric charges for GENUINE pitch movement; the field numbers
    // (measured with the same metric) inherit it.
    std::printf ("\nexp4b: ideal-resampler control, same trajectory\n");
    {
        const int n = (int) (kFs * kSeconds);
        std::vector<float> vib ((size_t) n);
        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double f = kF0 * std::pow (2.0,
                (30.0 / 1200.0) * std::sin (2.0 * M_PI * 6.0 * i / kFs));
            double sig = 0.0;
            for (int h = 1; h <= 12; ++h) sig += std::sin (2.0 * M_PI * h * ph) / (double) h;
            vib[(size_t) i] = (float) (0.25 * sig);
            ph += f / kFs;
        }
        std::vector<float> out ((size_t) n, 0.0f);
        double pos = 0.0;
        auto cubic = [&] (double x) -> float
        {
            const long i1 = (long) x;
            if (i1 < 1 || i1 + 2 >= n) return 0.0f;
            const double fr = x - (double) i1;
            const double xm = vib[(size_t) (i1 - 1)], x0 = vib[(size_t) i1],
                         x1 = vib[(size_t) (i1 + 1)], x2 = vib[(size_t) (i1 + 2)];
            return (float) (x0 + 0.5 * fr * (x1 - xm
                          + fr * (2.0 * xm - 5.0 * x0 + 4.0 * x1 - x2
                          + fr * (3.0 * (x0 - x1) + x2 - xm))));
        };
        for (int i = 0; i < n; ++i)
        {
            const float sc = 40.0f
                + 40.0f * (float) std::sin (2.0 * M_PI * 3.0 * i / kFs);
            out[(size_t) i] = cubic (pos);
            pos += std::pow (2.0, sc / 1200.0);
        }
        const double outT = kFs / (double) (kF0 * std::pow (2.0f, 40.0f / 1200.0f));
        const auto st = measure (out, 0, outT);
        const int T = (int) std::lround (outT);
        int rough = 0;
        for (long a = (long) (0.5 * kFs);
             a + 2 * T + 4 < (long) out.size() - T; a += T)
            if (ncc (out, a, a + T, T) < 0.90) ++rough;
        std::printf ("IDEAL: sim_median %.4f, sim_p10 %.4f, rough_per_s %.2f\n",
                     st.simMedian, st.simP10, rough / (kSeconds - 0.5));
    }
    return 0;
}
