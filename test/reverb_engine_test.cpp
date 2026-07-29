// Standalone test for ReverbEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source reverb_engine_test.cpp -o reverbtest && ./reverbtest
//
// A reverb is the one device where "it sounds fine" is not evidence: the failure
// modes are slow (a tail that never quite dies), settings-dependent (a decay
// that diverges only at 20 s and maximum size) and easy to miss by ear (a
// network that is really eight parallel delays). So the checks here are the
// structural ones:
//
//   * the Hadamard matrix is genuinely ORTHOGONAL, which is the entire
//     stability argument for the network,
//   * the per-line decay gains follow the RT60 law, and shorter lines get
//     HIGHER per-pass gains (equal gains is what thins a tail out early),
//   * the measured tail actually tracks the decay knob,
//   * predelay means silence before it, not just "later",
//   * mix = 0 is bit-identical dry, width = 0 is mono,
//   * and nothing diverges at ANY corner of the advertised parameter space,
//     checked over 30 seconds rather than a block.

#include "EedReverbEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }

constexpr double kSr = 48000.0;

struct Run { std::vector<float> l, r; };

static Run impulseRun (ReverbEngine& e, int n, bool stereo = true, float amp = 1.0f)
{
    Run out;
    out.l.assign ((size_t) n, 0.0f);
    out.r.assign ((size_t) n, 0.0f);
    out.l[0] = amp;
    if (stereo) out.r[0] = amp;

    constexpr int kBlock = 256;
    for (int i = 0; i < n; i += kBlock)
    {
        const int m = std::min (kBlock, n - i);
        e.process (&out.l[(size_t) i], stereo ? &out.r[(size_t) i] : nullptr, m);
    }
    return out;
}

static double rmsIn (const std::vector<float>& v, int from, int to)
{
    from = std::max (0, from);
    to   = std::min ((int) v.size(), to);
    if (to <= from) return 0.0;

    double e = 0.0;
    for (int i = from; i < to; ++i) e += (double) v[(size_t) i] * (double) v[(size_t) i];
    return std::sqrt (e / (double) (to - from));
}

static double peakIn (const std::vector<float>& v, int from, int to)
{
    from = std::max (0, from);
    to   = std::min ((int) v.size(), to);
    double p = 0.0;
    for (int i = from; i < to; ++i) p = std::max (p, std::fabs ((double) v[(size_t) i]));
    return p;
}

static bool anyNonFinite (const std::vector<float>& v)
{
    for (float x : v) if (! std::isfinite (x)) return true;
    return false;
}

// The default-ish reverb a test starts from, prepared and snapped.
static void configure (ReverbEngine& e, float sizePct, float decayS, float mixPct,
                       float predelayMs = 0.0f, float dampingPct = 0.0f)
{
    e.setSizePct (sizePct);
    e.setDecaySeconds (decayS);
    e.setMixPct (mixPct);
    e.setPredelayMs (predelayMs);
    e.setDampingPct (dampingPct);
    e.setLowCutHz (20.0f);
    e.setWidthPct (100.0f);
    e.setEarlyLatePct (100.0f);      // late only, unless a test says otherwise
    e.setModDepthPct (0.0f);         // deterministic unless a test says otherwise
    e.prepare (kSr, 256);
}

int main()
{
    std::printf ("== Hadamard: orthogonal, which IS the stability argument ==\n");
    {
        // Energy preserved for arbitrary vectors. If this is not exactly true,
        // the network's "max gain < 1 implies stable" reasoning does not hold.
        bool ok = true;
        double worst = 0.0;

        for (int trial = 0; trial < 64; ++trial)
        {
            float v[8];
            double before = 0.0;
            for (int i = 0; i < 8; ++i)
            {
                v[i] = (float) std::sin (trial * 1.7 + i * 2.3);
                before += (double) v[i] * (double) v[i];
            }

            ReverbEngine::hadamard8 (v);

            double after = 0.0;
            for (int i = 0; i < 8; ++i) after += (double) v[i] * (double) v[i];

            worst = std::max (worst, std::fabs (after - before));
            if (std::fabs (after - before) > 1e-4) ok = false;
        }
        check (ok, "energy is preserved exactly (worst drift "
                   + std::to_string (worst) + ")");
    }

    std::printf ("== Hadamard: really mixes (every line reaches every line) ==\n");
    {
        // One line hot, the rest silent. After the matrix all eight must carry
        // signal, or the "network" is really parallel delays.
        float v[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
        ReverbEngine::hadamard8 (v);

        bool allHot = true;
        for (int i = 0; i < 8; ++i) if (std::fabs (v[i]) < 0.3f) allHot = false;
        check (allHot, "a single hot line lands in all eight");
    }

    std::printf ("== Hadamard: involutive (its own inverse when orthogonal) ==\n");
    {
        float v[8] = { 0.3f, -0.7f, 1.1f, 0.0f, -0.2f, 0.9f, -1.3f, 0.4f };
        float ref[8];
        for (int i = 0; i < 8; ++i) ref[i] = v[i];

        ReverbEngine::hadamard8 (v);
        ReverbEngine::hadamard8 (v);

        bool same = true;
        for (int i = 0; i < 8; ++i) if (! near (v[i], ref[i], 1e-4)) same = false;
        check (same, "applying it twice is the identity");
    }

    std::printf ("== decay gain follows the RT60 law ==\n");
    {
        // A line of length L at gain g repeats every L seconds, so after T60 it
        // has been through T60/L times: g^(T60/L) must be 10^-3 (-60 dB).
        bool ok = true;
        for (double L : { 0.01, 0.03, 0.07 })
            for (double t60 : { 0.5, 2.0, 8.0 })
            {
                const double g    = ReverbEngine::decayGain (L, t60);
                const double after = std::pow (g, t60 / L);
                if (! near (after, 0.001, 1e-4)) ok = false;
            }
        check (ok, "g^(T60/L) lands on -60 dB for every length and time");

        // The relationship that keeps the tail dense all the way down: a short
        // line recirculates more often, so it needs a HIGHER per-pass gain to
        // take the same wall-clock time to decay. Equal gains across lines is
        // what makes a naive FDN thin out partway through its own tail.
        check (ReverbEngine::decayGain (0.01, 2.0) > ReverbEngine::decayGain (0.07, 2.0),
               "a SHORTER line gets a HIGHER per-pass gain");

        check (ReverbEngine::decayGain (0.03, 1000.0) <= 0.9995f,
               "gain is capped strictly below 1 even at an absurd decay");
        check (ReverbEngine::decayGain (0.03, 0.0) == 0.0f, "a zero decay is silence, not a NaN");
    }

    std::printf ("== damping and size mappings are monotonic and bounded ==\n");
    {
        check (near (ReverbEngine::dampingCutoffHz (0.0), 18000.0, 1.0), "0% damping is open");
        check (near (ReverbEngine::dampingCutoffHz (100.0), 800.0, 1.0), "100% damping is 800 Hz");
        check (ReverbEngine::dampingCutoffHz (50.0) < 18000.0
               && ReverbEngine::dampingCutoffHz (50.0) > 800.0, "50% is in between");

        check (near (ReverbEngine::sizeFactor (100.0), 1.0, 1e-9), "size 100 uses the full lines");
        check (near (ReverbEngine::sizeFactor (0.0), 0.25, 1e-9),
               "size 0 floors at a quarter, not at zero");
        check (ReverbEngine::sizeFactor (50.0) > ReverbEngine::sizeFactor (10.0), "monotonic");
    }

    std::printf ("== mix = 0 is BIT-IDENTICAL pass-through ==\n");
    {
        ReverbEngine e;
        configure (e, 80.0f, 6.0f, 0.0f, 50.0f, 50.0f);

        std::vector<float> l ((size_t) 8000), r ((size_t) 8000), refL, refR;
        for (int i = 0; i < 8000; ++i)
        {
            l[(size_t) i] = (float) std::sin (i * 0.01) * 0.7f;
            r[(size_t) i] = (float) std::cos (i * 0.017) * 0.6f;
        }
        refL = l; refR = r;

        for (int i = 0; i < 8000; i += 256)
            e.process (&l[(size_t) i], &r[(size_t) i], std::min (256, 8000 - i));

        bool identical = true;
        for (int i = 0; i < 8000; ++i)
            if (l[(size_t) i] != refL[(size_t) i] || r[(size_t) i] != refR[(size_t) i])
                identical = false;

        check (identical, "a fully dry reverb changes nothing at all");
    }

    std::printf ("== a tail is produced, and it decays ==\n");
    {
        ReverbEngine e;
        configure (e, 70.0f, 3.0f, 100.0f);
        auto out = impulseRun (e, (int) kSr * 8);

        const double early = rmsIn (out.l, 2400,  12000);   // 50 ms .. 250 ms
        const double mid   = rmsIn (out.l, 48000, 72000);   // 1 s .. 1.5 s
        const double late  = rmsIn (out.l, 336000, 384000); // 7 s .. 8 s

        check (early > 1e-4, "there IS a tail (early rms " + std::to_string (early) + ")");
        check (mid < early,  "it is quieter at 1 s than at 100 ms");
        check (late < mid * 0.2, "and far quieter again by 7 s");
        check (! anyNonFinite (out.l) && ! anyNonFinite (out.r), "finite throughout");
    }

    std::printf ("== the tail is DENSE, not a train of discrete echoes ==\n");
    {
        // The point of the matrix. Count how much of a 100 ms window past the
        // onset is meaningfully non-silent: a parallel-comb design leaves most
        // of it empty, an FDN fills it.
        ReverbEngine e;
        configure (e, 70.0f, 3.0f, 100.0f);
        auto out = impulseRun (e, 48000);

        const double p = peakIn (out.l, 9600, 14400);
        int busy = 0;
        for (int i = 9600; i < 14400; ++i)
            if (std::fabs ((double) out.l[(size_t) i]) > p * 0.02) ++busy;

        const double fill = (double) busy / 4800.0;
        check (fill > 0.8, "the 200-300 ms window is " + std::to_string (fill * 100.0)
                           + "% filled");
    }

    std::printf ("== the decay knob actually sets the decay ==\n");
    {
        auto tailAt = [] (float decayS, int fromS, int toS)
        {
            ReverbEngine e;
            configure (e, 70.0f, decayS, 100.0f);
            auto out = impulseRun (e, (int) kSr * toS + 1000);
            return rmsIn (out.l, (int) kSr * fromS, (int) kSr * toS);
        };

        const double shortTail = tailAt (0.5f, 2, 3);
        const double longTail  = tailAt (8.0f, 2, 3);
        check (longTail > shortTail * 20.0,
               "at 2-3 s an 8 s decay is far louder than a 0.5 s one ("
               + std::to_string (longTail) + " vs " + std::to_string (shortTail) + ")");

        // And the short one really is over, rather than merely quieter.
        ReverbEngine e;
        configure (e, 70.0f, 0.5f, 100.0f);
        auto out = impulseRun (e, (int) kSr * 5);
        const double onset = rmsIn (out.l, 2400, 12000);
        const double after = rmsIn (out.l, (int) kSr * 4, (int) kSr * 5);
        check (after < onset * 1e-3, "a 0.5 s decay is 60+ dB down by 4 s");
    }

    std::printf ("== measured RT60 is in the right neighbourhood ==\n");
    {
        // Not exact: the damping filter sits inside the loop and shortens what
        // it filters, and the early/late blend colours the onset. With damping
        // off, the measured -60 dB point should land within a factor of two of
        // what was asked, which is what separates "the knob works" from "the
        // knob is decorative".
        auto measureRt60 = [] (float decayS)
        {
            ReverbEngine e;
            configure (e, 80.0f, decayS, 100.0f);
            auto out = impulseRun (e, (int) (kSr * (decayS * 3.0 + 1.0)));

            const double ref = rmsIn (out.l, 4800, 9600);           // 100-200 ms
            if (ref <= 0.0) return -1.0;

            const int win = (int) (kSr * 0.1);
            for (int start = 9600; start + win < (int) out.l.size(); start += win)
            {
                if (rmsIn (out.l, start, start + win) < ref * 0.001)
                    return (double) start / kSr;
            }
            return -1.0;
        };

        const double m2 = measureRt60 (2.0f);
        const double m6 = measureRt60 (6.0f);

        check (m2 > 0.8 && m2 < 4.0, "a 2 s decay measures " + std::to_string (m2) + " s");
        check (m6 > 2.5 && m6 < 12.0, "a 6 s decay measures " + std::to_string (m6) + " s");
        check (m6 > m2, "and longer is longer");
    }

    std::printf ("== predelay is SILENCE before it, not just a later peak ==\n");
    {
        ReverbEngine e;
        configure (e, 70.0f, 3.0f, 100.0f, 100.0f);      // 100 ms = 4800 samples
        auto out = impulseRun (e, 48000);

        const double before = peakIn (out.l, 0, 4700);
        const double after  = peakIn (out.l, 4800, 9600);

        check (before < 1e-5, "nothing at all before 100 ms (peak "
                              + std::to_string (before) + ")");
        check (after > 1e-3, "and the reverb starts right after it");
    }

    std::printf ("== zero predelay starts as early as the network can ==\n");
    {
        // "Immediately" is not the same as "at sample 0". With no predelay the
        // first thing out is the earliest reflection tap; the late tail cannot
        // arrive before the shortest FDN line has gone round once, which at size
        // 70 is ~21 ms. So the check is that predelay SHIFTS the onset, and that
        // the un-predelayed onset is the tap time rather than anything later.
        auto onsetSamples = [] (float predelayMs, float elPct)
        {
            ReverbEngine e;
            configure (e, 70.0f, 3.0f, 100.0f, predelayMs);
            e.setEarlyLatePct (elPct);
            e.prepare (kSr, 256);

            auto out = impulseRun (e, 48000);
            for (int i = 0; i < (int) out.l.size(); ++i)
                if (std::fabs ((double) out.l[i]) > 1e-4) return i;
            return -1;
        };

        const int erOnset   = onsetSamples (0.0f, 50.0f);      // early + late
        const int lateOnset = onsetSamples (0.0f, 100.0f);     // late only
        const int shifted   = onsetSamples (100.0f, 50.0f);

        // First early tap is 4.7 ms scaled by size 70 (factor 0.775) = 3.6 ms.
        check (erOnset > 0 && erOnset < (int) (kSr * 0.006),
               "the first reflection arrives at " + std::to_string (erOnset * 1000.0 / kSr)
               + " ms");
        check (lateOnset > (int) (kSr * 0.015),
               "the late tail cannot beat its shortest line ("
               + std::to_string (lateOnset * 1000.0 / kSr) + " ms)");
        check (shifted > erOnset + (int) (kSr * 0.09),
               "100 ms of predelay pushes the onset out by 100 ms");
    }

    std::printf ("== early reflections and the late tail are separable ==\n");
    {
        // early_late = 0 is early only: a handful of discrete taps that are OVER
        // quickly. 100 is late only: no discrete onset, but a tail that lasts.
        auto lateEnergy = [] (float elPct)
        {
            ReverbEngine e;
            configure (e, 70.0f, 4.0f, 100.0f);
            e.setEarlyLatePct (elPct);
            e.prepare (kSr, 256);
            auto out = impulseRun (e, (int) kSr * 3);
            return rmsIn (out.l, (int) kSr * 2, (int) kSr * 3);
        };

        const double earlyOnly = lateEnergy (0.0f);
        const double lateOnly  = lateEnergy (100.0f);
        check (earlyOnly < lateOnly * 0.01,
               "early-only has essentially no 2-3 s tail (" + std::to_string (earlyOnly) + ")");

        // Early-only still makes SOUND — it is reflections, not a mute.
        ReverbEngine e;
        configure (e, 70.0f, 4.0f, 100.0f);
        e.setEarlyLatePct (0.0f);
        e.prepare (kSr, 256);
        auto out = impulseRun (e, 24000);
        check (peakIn (out.l, 0, 6000) > 0.05, "early-only still produces reflections");
    }

    std::printf ("== width: 100 is stereo, 0 is mono ==\n");
    {
        ReverbEngine e;
        configure (e, 70.0f, 3.0f, 100.0f);
        auto wide = impulseRun (e, 48000);

        double diff = 0.0;
        for (size_t i = 0; i < wide.l.size(); ++i)
            diff = std::max (diff, std::fabs ((double) wide.l[i] - (double) wide.r[i]));
        check (diff > 0.01, "at width 100 the two channels differ");

        ReverbEngine m;
        configure (m, 70.0f, 3.0f, 100.0f);
        m.setWidthPct (0.0f);
        m.prepare (kSr, 256);
        auto mono = impulseRun (m, 48000);

        double monoDiff = 0.0;
        for (size_t i = 0; i < mono.l.size(); ++i)
            monoDiff = std::max (monoDiff, std::fabs ((double) mono.l[i] - (double) mono.r[i]));
        check (monoDiff < 1e-6, "at width 0 they are identical (mono-safe)");
    }

    std::printf ("== damping darkens the tail ==\n");
    {
        auto hfEnergy = [] (float dampPct)
        {
            ReverbEngine e;
            configure (e, 70.0f, 4.0f, 100.0f, 0.0f, dampPct);
            auto out = impulseRun (e, (int) kSr * 2);

            // Crude HF measure: first-difference energy over the tail. A darker
            // signal moves less between adjacent samples.
            double hf = 0.0;
            for (int i = 24000; i < 96000; ++i)
            {
                const double d = (double) out.l[(size_t) i] - (double) out.l[(size_t) (i - 1)];
                hf += d * d;
            }
            return hf;
        };

        check (hfEnergy (90.0f) < hfEnergy (0.0f) * 0.5,
               "90% damping leaves much less high-frequency energy in the tail");
    }

    std::printf ("== the low cut keeps the tail out of the sub ==\n");
    {
        auto dcDrift = [] (float lowCut)
        {
            ReverbEngine e;
            configure (e, 80.0f, 10.0f, 100.0f);
            e.setLowCutHz (lowCut);
            e.prepare (kSr, 256);

            std::vector<float> l ((size_t) 48000, 0.5f), r ((size_t) 48000, 0.5f);  // DC
            for (int i = 0; i < 48000; i += 256)
                e.process (&l[(size_t) i], &r[(size_t) i], std::min (256, 48000 - i));

            double sum = 0.0;
            for (int i = 24000; i < 48000; ++i) sum += (double) l[(size_t) i];
            return std::fabs (sum / 24000.0);
        };

        check (dcDrift (800.0f) < dcDrift (20.0f),
               "an 800 Hz low cut accumulates less DC than a 20 Hz one");
    }

    std::printf ("== modulation smears the network's modes ==\n");
    {
        auto run = [] (float modPct)
        {
            ReverbEngine e;
            configure (e, 70.0f, 6.0f, 100.0f);
            e.setModDepthPct (modPct);
            e.prepare (kSr, 256);
            return impulseRun (e, (int) kSr * 3);
        };

        auto still = run (0.0f);
        auto moved = run (100.0f);

        double diff = 0.0;
        for (size_t i = 0; i < still.l.size(); ++i)
            diff = std::max (diff, std::fabs ((double) still.l[i] - (double) moved.l[i]));
        check (diff > 1e-4, "modulation changes the tail");
        check (! anyNonFinite (moved.l), "a modulated tail stays finite");

        // Zero depth must be exactly repeatable, or every "unmodulated" reverb
        // is quietly drifting.
        auto still2 = run (0.0f);
        bool same = true;
        for (size_t i = 0; i < still.l.size(); ++i) if (still.l[i] != still2.l[i]) same = false;
        check (same, "zero depth is deterministic");
    }

    std::printf ("== an impulse at full wet never clips ==\n");
    {
        bool ok = true;
        double worst = 0.0;

        for (float size : { 0.0f, 50.0f, 100.0f })
            for (float dec : { 0.5f, 4.0f, 20.0f })
            {
                ReverbEngine e;
                configure (e, size, dec, 100.0f);
                e.setEarlyLatePct (50.0f);      // both halves contributing
                e.prepare (kSr, 256);

                auto out = impulseRun (e, (int) kSr * 2);
                const double p = std::max (peakIn (out.l, 0, (int) out.l.size()),
                                           peakIn (out.r, 0, (int) out.r.size()));
                worst = std::max (worst, p);
                if (p > 1.0) ok = false;
            }
        check (ok, "worst impulse peak across size/decay is "
                   + std::to_string (worst) + " (must stay under 1.0)");
    }

    std::printf ("== wet level is in the same league as dry ==\n");
    {
        // Not a precise target, a sanity bound: a reverb whose wet is 30 dB off
        // the dry makes the mix knob useless long before it sounds wrong.
        ReverbEngine e;
        configure (e, 70.0f, 2.5f, 100.0f);
        e.setEarlyLatePct (70.0f);
        e.prepare (kSr, 256);

        std::vector<float> l ((size_t) kSr * 3), r ((size_t) kSr * 3);
        double dryEnergy = 0.0;
        for (size_t i = 0; i < l.size(); ++i)
        {
            const float x = (float) std::sin ((double) i * 0.05) * 0.4f;
            l[i] = x; r[i] = x;
            dryEnergy += (double) x * (double) x;
        }
        const double dryRms = std::sqrt (dryEnergy / (double) l.size());

        for (size_t i = 0; i < l.size(); i += 256)
            e.process (&l[i], &r[i], (int) std::min ((size_t) 256, l.size() - i));

        const double wetRms = rmsIn (l, (int) kSr, (int) kSr * 3);
        const double ratioDb = 20.0 * std::log10 (std::max (1e-12, wetRms / dryRms));
        check (ratioDb > -12.0 && ratioDb < 6.0,
               "wet is " + std::to_string (ratioDb) + " dB relative to dry");
    }

    std::printf ("== NOTHING diverges, at any corner of the parameter space ==\n");
    {
        // 30 seconds of loud continuous input at each corner. This is the check
        // that a reverb has to pass and cannot be talked out of.
        const float sizes[]  = { 0.0f, 100.0f };
        const float decays[] = { 0.1f, 20.0f };
        const float damps[]  = { 0.0f, 100.0f };
        const float cuts[]   = { 20.0f, 1000.0f };
        const float mods[]   = { 0.0f, 100.0f };

        bool   ok    = true;
        double worst = 0.0;
        std::string worstAt;

        for (float sz : sizes) for (float dc : decays) for (float dp : damps)
        for (float lc : cuts) for (float md : mods)
        {
            ReverbEngine e;
            configure (e, sz, dc, 100.0f, 200.0f, dp);
            e.setLowCutHz (lc);
            e.setModDepthPct (md);
            e.setEarlyLatePct (50.0f);
            e.prepare (kSr, 512);

            std::vector<float> l ((size_t) 4800), r ((size_t) 4800);
            double localWorst = 0.0;

            for (int blk = 0; blk < 300; ++blk)         // 300 x 0.1 s = 30 s
            {
                for (int i = 0; i < 4800; ++i)
                {
                    const float x = (float) std::sin ((blk * 4800 + i) * 0.031) * 0.95f;
                    l[(size_t) i] = x;
                    r[(size_t) i] = x;
                }
                e.process (l.data(), r.data(), 4800);

                for (float v : l)
                {
                    if (! std::isfinite (v)) ok = false;
                    localWorst = std::max (localWorst, (double) std::fabs (v));
                }
            }

            if (localWorst > worst)
            {
                worst = localWorst;
                worstAt = "size " + std::to_string ((int) sz) + ", decay "
                        + std::to_string (dc) + " s, damp " + std::to_string ((int) dp);
            }
            // Bounded, not quiet: 30 s of continuous full-scale input into a
            // 20-second reverb genuinely accumulates, and a real room does the
            // same. What must not happen is unbounded growth, so the limit is
            // set well above the honest build-up and well below anything that
            // could be called divergence.
            if (localWorst > 8.0) ok = false;
        }
        check (ok, "30 s of loud input at every corner stays finite and bounded "
                   "(worst " + std::to_string (worst) + " at " + worstAt + ")");
    }

    std::printf ("== the tail dies after the input stops, even at max decay ==\n");
    {
        ReverbEngine e;
        configure (e, 100.0f, 20.0f, 100.0f);
        auto out = impulseRun (e, (int) kSr * 90, true, 1.0f);   // 90 s

        const double at5   = rmsIn (out.l, (int) kSr * 5,  (int) kSr * 6);
        const double at85  = rmsIn (out.l, (int) kSr * 85, (int) kSr * 86);
        check (at85 < at5 * 1e-3, "20 s decay is 60+ dB down by 85 s ("
                                  + std::to_string (at85) + " vs " + std::to_string (at5) + ")");
    }

    std::printf ("== mono in, mono out ==\n");
    {
        ReverbEngine e;
        configure (e, 70.0f, 3.0f, 100.0f);
        auto out = impulseRun (e, 48000, false);
        check (peakIn (out.l, 0, 48000) > 1e-3, "a mono reverb still produces a tail");
        check (! anyNonFinite (out.l), "mono output is finite");
    }

    std::printf ("== parameters clamp to the advertised range ==\n");
    {
        ReverbEngine e;
        e.prepare (kSr, 256);

        e.setSizePct (900.0f);       check (e.getSizePct() == ReverbEngine::kMaxSizePct, "size clamps");
        e.setDecaySeconds (-1.0f);   check (e.getDecaySeconds() == ReverbEngine::kMinDecaySec, "decay clamps");
        e.setPredelayMs (1.0e6f);    check (e.getPredelayMs() == ReverbEngine::kMaxPredelayMs, "predelay clamps");
        e.setDampingPct (-4.0f);     check (e.getDampingPct() == ReverbEngine::kMinDampingPct, "damping clamps");
        e.setLowCutHz (99999.0f);    check (e.getLowCutHz() == ReverbEngine::kMaxLowCutHz, "low cut clamps");
        e.setWidthPct (400.0f);      check (e.getWidthPct() == ReverbEngine::kMaxWidthPct, "width clamps");
        e.setMixPct (-1.0f);         check (e.getMixPct() == ReverbEngine::kMinMixPct, "mix clamps");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
