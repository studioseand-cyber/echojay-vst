// Standalone test for DelayEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source delay_engine_test.cpp -o delayenginetest && ./delayenginetest
//
// The core is already pinned by delay_core_test.cpp, so this file is about the
// DEVICE's promises rather than the primitives':
//
//   * the echo lands at the time the knob says, in ms and in sync,
//   * mix = 0 is bit-identical pass-through (an insert at defaults must be
//     invisible, or the device is not safe to leave in a chain),
//   * feedback decays, always, including at the maximum the schema advertises,
//   * ping-pong actually bounces (first echo one side, second the other),
//   * the loop filter darkens repeats without touching the first one,
//   * nothing here can produce NaN or run away, at any combination of settings.

#include "EedDelayEngine.h"

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

// Run `n` samples through the engine in realistic blocks, with an impulse at
// sample 0 on both channels unless `silent`.
struct Run
{
    std::vector<float> l, r;
};

static Run impulseRun (DelayEngine& e, int n, bool stereo = true, float amp = 1.0f)
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

// Peak magnitude and its index inside [from, to).
static void peakIn (const std::vector<float>& v, int from, int to,
                    double& peak, int& at)
{
    peak = 0.0; at = from;
    from = std::max (0, from);
    to   = std::min ((int) v.size(), to);
    for (int i = from; i < to; ++i)
        if (std::fabs ((double) v[(size_t) i]) > peak)
        { peak = std::fabs ((double) v[(size_t) i]); at = i; }
}

static double energyIn (const std::vector<float>& v, int from, int to)
{
    double e = 0.0;
    from = std::max (0, from);
    to   = std::min ((int) v.size(), to);
    for (int i = from; i < to; ++i) e += (double) v[(size_t) i] * (double) v[(size_t) i];
    return e;
}

static bool anyNonFinite (const std::vector<float>& v)
{
    for (float x : v) if (! std::isfinite (x)) return true;
    return false;
}

// A device configured the way a test wants it, prepared and snapped.
static void configure (DelayEngine& e, float timeMs, float fbPct, float mixPct,
                       float hp = 20.0f, float lp = 20000.0f)
{
    e.setSync (false);
    e.setTimeMs (timeMs);
    e.setFeedbackPct (fbPct);
    e.setMixPct (mixPct);
    e.setHighpassHz (hp);
    e.setLowpassHz (lp);
    e.setPingPong (false);
    e.setStereoOffsetPct (0.0f);
    e.setModDepthMs (0.0f);
    e.setModRateHz (0.4f);
    e.prepare (kSr, 256);
}

int main()
{
    std::printf ("== the echo lands where the TIME knob says ==\n");
    {
        DelayEngine e;
        configure (e, 100.0f, 0.0f, 100.0f);          // 100 ms, no feedback, all wet
        auto out = impulseRun (e, 20000);

        double p = 0.0; int at = 0;
        peakIn (out.l, 1, 20000, p, at);

        // 100 ms at 48 kHz = 4800 samples. One extra sample is the read-before-
        // write convention (the loop reads before this sample's input is in the
        // line) and is inherent, not an error.
        check (std::abs (at - 4801) <= 2, "100 ms echo at sample "
                                          + std::to_string (at) + " (want ~4801)");
        check (near (p, 1.0, 1e-3), "the first echo is unattenuated (peak "
                                    + std::to_string (p) + ")");
    }

    std::printf ("== a different time moves it proportionally ==\n");
    {
        DelayEngine e;
        configure (e, 250.0f, 0.0f, 100.0f);
        auto out = impulseRun (e, 40000);

        double p = 0.0; int at = 0;
        peakIn (out.l, 1, 40000, p, at);
        check (std::abs (at - 12001) <= 2, "250 ms echo at sample " + std::to_string (at));
    }

    std::printf ("== mix = 0 is BIT-IDENTICAL pass-through ==\n");
    {
        DelayEngine e;
        configure (e, 300.0f, 80.0f, 0.0f);

        std::vector<float> l ((size_t) 8000), r ((size_t) 8000), refL, refR;
        for (int i = 0; i < 8000; ++i)
        {
            l[(size_t) i] = (float) std::sin (i * 0.01) * 0.7f;
            r[(size_t) i] = (float) std::cos (i * 0.013) * 0.6f;
        }
        refL = l; refR = r;

        for (int i = 0; i < 8000; i += 256)
        {
            const int m = std::min (256, 8000 - i);
            e.process (&l[(size_t) i], &r[(size_t) i], m);
        }

        bool identical = true;
        for (int i = 0; i < 8000; ++i)
            if (l[(size_t) i] != refL[(size_t) i] || r[(size_t) i] != refR[(size_t) i])
                identical = false;

        check (identical, "a fully dry delay changes nothing at all");
    }

    std::printf ("== mix = 100 leaves no dry signal at t = 0 ==\n");
    {
        DelayEngine e;
        configure (e, 100.0f, 0.0f, 100.0f);
        auto out = impulseRun (e, 8000);
        check (near (out.l[0], 0.0, 1e-6), "the input impulse itself is gone");
    }

    std::printf ("== feedback: repeats appear, and every one is quieter ==\n");
    {
        DelayEngine e;
        configure (e, 50.0f, 70.0f, 100.0f);          // 2400-sample spacing
        auto out = impulseRun (e, 40000);

        double prev = 1.0e9;
        bool   descending = true;
        int    echoes = 0;

        for (int k = 1; k <= 8; ++k)
        {
            double p = 0.0; int at = 0;
            peakIn (out.l, k * 2400 - 200, k * 2400 + 400, p, at);
            if (p > 1.0e-4) ++echoes;
            if (p > prev) descending = false;
            prev = p;
        }
        check (echoes >= 6, "at least 6 repeats are audible (" + std::to_string (echoes) + ")");
        check (descending, "each repeat is quieter than the one before it");
    }

    std::printf ("== more feedback means a longer tail ==\n");
    {
        auto tailEnergy = [] (float fb)
        {
            DelayEngine e;
            configure (e, 50.0f, fb, 100.0f);
            auto out = impulseRun (e, 60000);
            return energyIn (out.l, 30000, 60000);     // late tail only
        };

        const double low  = tailEnergy (20.0f);
        const double high = tailEnergy (85.0f);
        check (high > low * 10.0, "85% feedback holds far more late energy than 20%");
    }

    std::printf ("== full feedback is a long tail, NOT a divergence ==\n");
    {
        // The advertised maximum, fed continuously with a loud signal for 10
        // seconds. softClip in the loop is what has to hold here.
        DelayEngine e;
        configure (e, 120.0f, 100.0f, 100.0f);

        std::vector<float> l ((size_t) 4800), r ((size_t) 4800);
        double worst = 0.0;
        bool   finite = true;

        for (int blk = 0; blk < 100; ++blk)          // 100 x 0.1 s = 10 s
        {
            for (int i = 0; i < 4800; ++i)
            {
                const float x = (float) std::sin ((blk * 4800 + i) * 0.02) * 0.9f;
                l[(size_t) i] = x;
                r[(size_t) i] = x;
            }
            e.process (l.data(), r.data(), 4800);

            for (float v : l)
            {
                if (! std::isfinite (v)) finite = false;
                worst = std::max (worst, (double) std::fabs (v));
            }
        }
        check (finite, "no NaN or inf after 10 s at maximum feedback");
        check (worst < 8.0, "output stays bounded (worst |sample| "
                            + std::to_string (worst) + ")");
    }

    std::printf ("== full feedback decays once the input stops ==\n");
    {
        DelayEngine e;
        configure (e, 120.0f, 100.0f, 100.0f);
        auto out = impulseRun (e, (int) kSr * 20, true, 1.0f);   // 20 s

        const double early = energyIn (out.l, 0,      (int) kSr * 2);
        const double late  = energyIn (out.l, (int) kSr * 18, (int) kSr * 20);
        check (late < early, "the tail is quieter at 19 s than at 1 s ("
                             + std::to_string (late) + " vs " + std::to_string (early) + ")");
        check (! anyNonFinite (out.l), "still finite over 20 s");
    }

    std::printf ("== tempo sync: the note length is the delay time ==\n");
    {
        publishHostTempo (120.0);
        check (near (DelayEngine::syncedMs (9, 120.0), 500.0, 1e-6), "1/4 at 120 BPM is 500 ms");
        check (near (DelayEngine::syncedMs (6, 120.0), 250.0, 1e-6), "1/8 at 120 BPM is 250 ms");
        check (near (DelayEngine::syncedMs (3, 120.0), 125.0, 1e-6), "1/16 at 120 BPM is 125 ms");
        check (near (DelayEngine::syncedMs (8, 120.0), 375.0, 1e-6), "dotted 1/8 is 375 ms");
        check (near (DelayEngine::syncedMs (4, 120.0), 500.0 / 3.0, 1e-6), "1/8 triplet is a third of a beat");
        check (near (DelayEngine::syncedMs (9, 140.0), 60000.0 / 140.0, 1e-6), "1/4 tracks the tempo");

        // Divisions must rise with the index, or "make it longer" has no meaning.
        bool rising = true;
        for (int i = 1; i < DelayEngine::kNumDivisions; ++i)
            if (! (DelayEngine::divisionBeats (i) > DelayEngine::divisionBeats (i - 1)))
                rising = false;
        check (rising, "the division table is ordered shortest to longest");

        check (DelayEngine::clampDivision (-5) == 0, "a negative index clamps to the shortest");
        check (DelayEngine::clampDivision (999) == DelayEngine::kNumDivisions - 1,
               "an out-of-range index clamps to the longest");
    }

    std::printf ("== tempo sync drives the actual audio path ==\n");
    {
        publishHostTempo (120.0);
        DelayEngine e;
        configure (e, 999.0f, 0.0f, 100.0f);     // free-running time deliberately wrong
        e.setSync (true);
        e.setDivision (3);                        // 1/16 at 120 BPM = 125 ms = 6000 samples
        e.prepare (kSr, 256);

        auto out = impulseRun (e, 20000);
        double p = 0.0; int at = 0;
        peakIn (out.l, 1, 20000, p, at);
        check (std::abs (at - 6001) <= 2, "synced echo at sample " + std::to_string (at)
                                          + " (want ~6001), not the free-running time");
    }

    std::printf ("== no host tempo still gives a usable time ==\n");
    {
        gHostTempoBpm.store (0.0);                // a host that reports nothing
        check (near (hostTempoBpm(), 120.0, 1e-9), "falls back to 120 BPM");
        gHostTempoBpm.store (1.0e9);
        check (near (hostTempoBpm(), 120.0, 1e-9), "an absurd tempo also falls back");
        publishHostTempo (0.0);
        check (near (hostTempoBpm(), 120.0, 1e-9), "publishing nonsense is refused");
        publishHostTempo (174.0);
        check (near (hostTempoBpm(), 174.0, 1e-9), "a real tempo is accepted");
        publishHostTempo (120.0);
    }

    std::printf ("== stereo offset moves the right side only ==\n");
    {
        DelayEngine e;
        configure (e, 100.0f, 0.0f, 100.0f);
        e.setStereoOffsetPct (50.0f);            // right = 150 ms
        e.prepare (kSr, 256);

        auto out = impulseRun (e, 30000);
        double pl = 0.0, pr = 0.0; int al = 0, ar = 0;
        peakIn (out.l, 1, 30000, pl, al);
        peakIn (out.r, 1, 30000, pr, ar);

        check (std::abs (al - 4801) <= 2, "left is still at 100 ms (" + std::to_string (al) + ")");
        check (std::abs (ar - 7201) <= 2, "right is at 150 ms (" + std::to_string (ar) + ")");
    }

    std::printf ("== ping-pong bounces: first echo left, second right ==\n");
    {
        DelayEngine e;
        configure (e, 50.0f, 75.0f, 100.0f);
        e.setPingPong (true);
        e.prepare (kSr, 256);

        auto out = impulseRun (e, 30000);

        double l1 = 0.0, r1 = 0.0, l2 = 0.0, r2 = 0.0; int at = 0;
        peakIn (out.l, 2200, 2700, l1, at);
        peakIn (out.r, 2200, 2700, r1, at);
        peakIn (out.l, 4600, 5100, l2, at);
        peakIn (out.r, 4600, 5100, r2, at);

        check (l1 > 0.05 && r1 < l1 * 0.05, "the first echo is on the LEFT only");
        check (r2 > 0.05 && l2 < r2 * 0.05, "the second echo is on the RIGHT only");
    }

    std::printf ("== ping-pong OFF keeps each side on its own channel ==\n");
    {
        DelayEngine e;
        configure (e, 50.0f, 50.0f, 100.0f);
        std::vector<float> l ((size_t) 20000, 0.0f), r ((size_t) 20000, 0.0f);
        l[0] = 1.0f;                              // LEFT ONLY
        for (int i = 0; i < 20000; i += 256)
            e.process (&l[(size_t) i], &r[(size_t) i], std::min (256, 20000 - i));

        double pl = 0.0, pr = 0.0; int at = 0;
        peakIn (l, 1, 20000, pl, at);
        peakIn (r, 1, 20000, pr, at);
        check (pl > 0.5 && pr < 1e-6, "a left-only input stays left when ping-pong is off");
    }

    std::printf ("== the loop filter darkens REPEATS, not the first echo ==\n");
    {
        // Same feedback, very different low-pass. The first echo is a tap taken
        // BEFORE the filter, so it must be identical either way; the second has
        // been through the filter once, so it must not be.
        auto firstAndSecond = [] (float lp, double& first, double& second)
        {
            DelayEngine e;
            configure (e, 50.0f, 70.0f, 100.0f, 20.0f, lp);
            auto out = impulseRun (e, 20000);
            int at = 0;
            peakIn (out.l, 2200, 2700, first,  at);
            peakIn (out.l, 4600, 5100, second, at);
        };

        double brightFirst = 0.0, brightSecond = 0.0, darkFirst = 0.0, darkSecond = 0.0;
        firstAndSecond (18000.0f, brightFirst, brightSecond);
        firstAndSecond (400.0f,   darkFirst,   darkSecond);

        check (near (brightFirst, darkFirst, 1e-4), "the first echo is identical either way");
        check (darkSecond < brightSecond * 0.5, "a 400 Hz loop low-pass kills the second echo's peak");
    }

    std::printf ("== the high-pass stops sub energy piling up over repeats ==\n");
    {
        auto dcBuildup = [] (float hp)
        {
            DelayEngine e;
            configure (e, 30.0f, 95.0f, 100.0f, hp, 20000.0f);
            std::vector<float> l ((size_t) 48000, 0.6f), r ((size_t) 48000, 0.6f);  // DC
            for (int i = 0; i < 48000; i += 256)
                e.process (&l[(size_t) i], &r[(size_t) i], std::min (256, 48000 - i));
            return std::fabs ((double) l[47999]);
        };

        check (dcBuildup (400.0f) < dcBuildup (20.0f),
               "a 400 Hz loop high-pass accumulates less DC than a 20 Hz one");
    }

    std::printf ("== modulation actually modulates ==\n");
    {
        auto run = [] (float depthMs)
        {
            DelayEngine e;
            configure (e, 100.0f, 40.0f, 100.0f);
            e.setModDepthMs (depthMs);
            e.setModRateHz (2.0f);
            e.prepare (kSr, 256);
            return impulseRun (e, 60000);
        };

        auto flat = run (0.0f);
        auto wob  = run (8.0f);

        double diff = 0.0;
        for (size_t i = 0; i < flat.l.size(); ++i)
            diff = std::max (diff, std::fabs ((double) flat.l[i] - (double) wob.l[i]));

        check (diff > 0.01, "8 ms of modulation changes the output");
        check (! anyNonFinite (wob.l) && ! anyNonFinite (wob.r), "modulated output is finite");

        // Depth 0 must be EXACTLY still — a modulated read at zero depth that
        // still wobbles would detune every unmodulated delay in the plugin.
        auto flat2 = run (0.0f);
        bool same = true;
        for (size_t i = 0; i < flat.l.size(); ++i)
            if (flat.l[i] != flat2.l[i]) same = false;
        check (same, "zero depth is deterministic and still");
    }

    std::printf ("== mono in, mono out ==\n");
    {
        DelayEngine e;
        configure (e, 80.0f, 50.0f, 100.0f);
        e.setPingPong (true);                    // meaningless in mono: must be ignored
        e.setStereoOffsetPct (50.0f);
        e.prepare (kSr, 256);

        auto out = impulseRun (e, 20000, false);
        double p = 0.0; int at = 0;
        peakIn (out.l, 1, 20000, p, at);
        check (std::abs (at - 3841) <= 2, "mono echo at 80 ms (" + std::to_string (at) + ")");
        check (! anyNonFinite (out.l), "mono output is finite");
    }

    std::printf ("== a time change glides to the new time ==\n");
    {
        DelayEngine e;
        configure (e, 100.0f, 0.0f, 100.0f);

        // Move the knob, let the 60 ms smoother settle, THEN measure.
        e.setTimeMs (200.0f);
        std::vector<float> l ((size_t) 48000, 0.0f), r ((size_t) 48000, 0.0f);
        for (int i = 0; i < 48000; i += 256)
            e.process (&l[(size_t) i], &r[(size_t) i], std::min (256, 48000 - i));

        auto out = impulseRun (e, 30000);
        double p = 0.0; int at = 0;
        peakIn (out.l, 1, 30000, p, at);
        check (std::abs (at - 9601) <= 4, "settles on the new 200 ms time ("
                                          + std::to_string (at) + ")");
    }

    std::printf ("== every advertised extreme is survivable ==\n");
    {
        const float times[]  = { DelayEngine::kMinTimeMs, 1.0f, 4000.0f };
        const float fbs[]    = { 0.0f, 100.0f };
        const float mixes[]  = { 0.0f, 100.0f };
        const float hps[]    = { DelayEngine::kMinHpHz, DelayEngine::kMaxHpHz };
        const float lps[]    = { DelayEngine::kMinLpHz, DelayEngine::kMaxLpHz };

        bool ok = true;
        for (float t : times) for (float f : fbs) for (float m : mixes)
        for (float hp : hps) for (float lp : lps)
        {
            DelayEngine e;
            configure (e, t, f, m, hp, lp);
            e.setModDepthMs (DelayEngine::kMaxModDepthMs);
            e.setModRateHz (DelayEngine::kMaxModRateHz);
            e.setStereoOffsetPct (DelayEngine::kMaxOffsetPct);
            e.setPingPong (true);
            e.prepare (kSr, 256);

            auto out = impulseRun (e, 96000, true, 1.0f);
            if (anyNonFinite (out.l) || anyNonFinite (out.r)) ok = false;
        }
        check (ok, "no setting combination produces NaN or inf");
    }

    std::printf ("== parameters clamp to the advertised range ==\n");
    {
        DelayEngine e;
        e.prepare (kSr, 256);

        e.setTimeMs (1.0e6f);       check (e.getTimeMs() == DelayEngine::kMaxTimeMs, "time clamps high");
        e.setTimeMs (-5.0f);        check (e.getTimeMs() == DelayEngine::kMinTimeMs, "time clamps low");
        e.setFeedbackPct (500.0f);  check (e.getFeedbackPct() == DelayEngine::kMaxFeedbackPct, "feedback clamps");
        e.setMixPct (-10.0f);       check (e.getMixPct() == DelayEngine::kMinMixPct, "mix clamps");
        e.setStereoOffsetPct (99.0f);
        check (e.getStereoOffsetPct() == DelayEngine::kMaxOffsetPct, "offset clamps");
        e.setDivision (99);         check (e.getDivision() == DelayEngine::kNumDivisions - 1, "division clamps");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
