// Standalone test for the four Modulation-cluster engines (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source mod_engines_test.cpp -o modtest && ./modtest
//
// LfoCore has its own test (lfo_core_test.cpp). What this file pins is what each
// FACE does with the LFO's number — and above all the two properties that are
// silent when broken:
//
//   * a device at its defaults, or with depth/mix at 0, is PASS-THROUGH, and
//   * nothing here can run away, however far the feedback is pushed.

#include "EedAutoPanEngine.h"
#include "EedChorusEngine.h"
#include "EedPhaserEngine.h"
#include "EedTremoloEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }

constexpr double kSr    = 48000.0;
constexpr int    kBlock = 512;

// A block of DC 1.0 in both channels.
static void fillDc (std::vector<float>& l, std::vector<float>& r, float v = 1.0f)
{
    for (size_t i = 0; i < l.size(); ++i) { l[i] = v; r[i] = v; }
}

static float peakOf (const std::vector<float>& v)
{
    float p = 0.0f;
    for (float x : v) p = std::max (p, std::fabs (x));
    return p;
}

static bool allFinite (const std::vector<float>& v)
{
    for (float x : v) if (! std::isfinite (x)) return false;
    return true;
}

// ---------------------------------------------------------------------------
static void testTremolo()
{
    std::printf ("== TREMOLO: the gain law hangs DOWN from unity ==\n");
    {
        check (near (TremoloEngine::gainFor (0.0f,  0.0f), 1.0, 1e-6), "depth 0 is unity");
        check (near (TremoloEngine::gainFor (1.0f,  1.0f), 1.0, 1e-6), "depth 100%, peak = unity (never boosts)");
        check (near (TremoloEngine::gainFor (1.0f, -1.0f), 0.0, 1e-6), "depth 100%, trough = silence");
        check (near (TremoloEngine::gainFor (0.5f,  0.0f), 0.75, 1e-6), "depth 50%, mid = 0.75");
        check (near (TremoloEngine::gainFor (0.5f,  0.5f), 1.0, 1e-6), "depth 50%, peak = unity");
        check (near (TremoloEngine::gainFor (0.5f, -0.5f), 0.5, 1e-6), "depth 50%, trough = 0.5");

        bool neverAbove = true;
        for (int d = 0; d <= 10; ++d)
            for (int m = -10; m <= 10; ++m)
                if (TremoloEngine::gainFor ((float) d / 10.0f,
                                            (float) m / 10.0f * (float) d / 10.0f) > 1.0f + 1e-6f)
                    neverAbove = false;
        check (neverAbove, "the gain never exceeds unity anywhere in the range");
    }

    std::printf ("== TREMOLO: depth 0 is bit-identical pass-through ==\n");
    {
        TremoloEngine e;
        e.lfo().setDepthPercent (0.0f);
        e.lfo().setRateHz (5.0f);
        e.setMixPercent (100.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool identical = true;
        for (int b = 0; b < 8; ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (l[(size_t) i] != 1.0f || r[(size_t) i] != 1.0f) identical = false;
        }
        check (identical, "in == out, exactly");
    }

    std::printf ("== TREMOLO: mix 0 is pass-through even at full depth ==\n");
    {
        TremoloEngine e;
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (5.0f);
        e.setMixPercent (0.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool identical = true;
        for (int b = 0; b < 8; ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (l[(size_t) i] != 1.0f) identical = false;
        }
        check (identical, "a fully dry tremolo does nothing");
    }

    std::printf ("== TREMOLO: full depth swings between silence and unity ==\n");
    {
        TremoloEngine e;
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (4.0f);
        e.lfo().setShape (LfoCore::kSine);
        e.setMixPercent (100.0f);
        e.prepare (kSr, kBlock);

        float lo = 2.0f, hi = -2.0f;
        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        for (int b = 0; b < (int) (kSr / kBlock); ++b)   // a full second
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (float x : l) { lo = std::min (lo, x); hi = std::max (hi, x); }
        }
        check (near (hi, 1.0, 0.01), "peaks at unity, does not boost");
        check (lo < 0.01f,           "troughs at silence");
    }

    std::printf ("== TREMOLO: stereo phase 180 makes the channels counter-swing ==\n");
    {
        TremoloEngine e;
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (2.0f);
        e.lfo().setStereoPhaseDeg (180.0f);
        e.setMixPercent (100.0f);
        e.prepare (kSr, kBlock);

        double worstSum = 0.0, bestSep = 0.0;
        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        for (int b = 0; b < (int) (kSr / kBlock); ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                // gain law is 1 - d/2 + m/2 with mirrored m, so the pair always
                // sums to 2 - d = 1.0 at full depth.
                worstSum = std::max (worstSum, (double) std::fabs ((l[(size_t) i] + r[(size_t) i]) - 1.0f));
                bestSep  = std::max (bestSep, (double) std::fabs (l[(size_t) i] - r[(size_t) i]));
            }
        }
        check (worstSum < 0.01, "the two channels' gains stay complementary");
        check (bestSep > 0.9,   "and they genuinely swing apart");
    }
}

// ---------------------------------------------------------------------------
static void testAutoPan()
{
    std::printf ("== AUTO PAN: depth 0 is pass-through (centre is unity) ==\n");
    {
        AutoPanEngine e;
        e.lfo().setDepthPercent (0.0f);
        e.lfo().setRateHz (1.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        float worst = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                worst = std::max (worst, std::fabs (l[(size_t) i] - 1.0f));
                worst = std::max (worst, std::fabs (r[(size_t) i] - 1.0f));
            }
        }
        check (worst < 1e-5f, "both channels come back unchanged");
    }

    std::printf ("== AUTO PAN: in step, the pair holds constant POWER ==\n");
    {
        AutoPanEngine e;
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (3.0f);
        e.lfo().setStereoPhaseDeg (0.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        double worst = 0.0;
        for (int b = 0; b < (int) (kSr / kBlock); ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                const double p = (double) l[(size_t) i] * l[(size_t) i]
                               + (double) r[(size_t) i] * r[(size_t) i];
                worst = std::max (worst, std::fabs (p - 2.0));   // sqrt(2)-normalised
            }
        }
        check (worst < 0.01, "L^2 + R^2 stays constant across the whole sweep");
    }

    std::printf ("== AUTO PAN: full depth reaches both extremes ==\n");
    {
        AutoPanEngine e;
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (4.0f);
        e.lfo().setStereoPhaseDeg (0.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        float loL = 2.0f, hiL = -2.0f;
        for (int b = 0; b < (int) (kSr / kBlock); ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (float x : l) { loL = std::min (loL, x); hiL = std::max (hiL, x); }
        }
        check (loL < 0.02f,            "left reaches silence at hard right");
        check (near (hiL, 1.414, 0.02), "and +3 dB at hard left, holding power");
    }

    std::printf ("== AUTO PAN: mono is left alone, not half-panned ==\n");
    {
        AutoPanEngine e;
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (4.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock);
        bool identical = true;
        for (int b = 0; b < 8; ++b)
        {
            for (auto& x : l) x = 1.0f;
            e.process (l.data(), nullptr, kBlock);
            for (float x : l) if (x != 1.0f) identical = false;
        }
        check (identical, "there is no stereo field to pan into, so nothing happens");
    }
}

// ---------------------------------------------------------------------------
static void testChorus()
{
    std::printf ("== CHORUS: the sweep never outgrows the base delay ==\n");
    {
        check (near (ChorusEngine::sweepMsFor (1.0f),  0.9, 1e-6),  "1 ms base sweeps +/-0.9 ms");
        check (near (ChorusEngine::sweepMsFor (5.0f),  4.5, 1e-6),  "5 ms base sweeps +/-4.5 ms");
        check (near (ChorusEngine::sweepMsFor (20.0f), 8.0, 1e-6),  "20 ms base is capped at +/-8 ms");
        check (near (ChorusEngine::sweepMsFor (50.0f), 8.0, 1e-6),  "50 ms base is capped too");

        bool alwaysPositive = true;
        for (int i = 10; i <= 500; ++i)
        {
            const float base = (float) i / 10.0f;
            if (base - ChorusEngine::sweepMsFor (base) <= 0.0f) alwaysPositive = false;
        }
        check (alwaysPositive, "the delay stays positive at the trough for every base setting");
    }

    std::printf ("== CHORUS: mix 0 is pass-through ==\n");
    {
        ChorusEngine e;
        e.setMixPercent (0.0f);
        e.setFeedbackPercent (90.0f);
        e.lfo().setDepthPercent (100.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool identical = true;
        for (int b = 0; b < 8; ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (l[(size_t) i] != 1.0f || r[(size_t) i] != 1.0f) identical = false;
        }
        check (identical, "a fully dry chorus does nothing");
    }

    std::printf ("== CHORUS: an impulse comes back at the dialled delay ==\n");
    {
        ChorusEngine e;
        e.setDelayMs (10.0f);
        e.setVoices (1);
        e.setFeedbackPercent (0.0f);
        e.setMixPercent (100.0f);
        e.lfo().setDepthPercent (0.0f);       // no sweep: pin the delay exactly
        e.prepare (kSr, kBlock);

        std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
        l[0] = 1.0f; r[0] = 1.0f;
        e.process (l.data(), r.data(), (int) l.size());

        int peakAt = 0;
        float peak = 0.0f;
        for (size_t i = 1; i < l.size(); ++i)
            if (std::fabs (l[i]) > peak) { peak = std::fabs (l[i]); peakAt = (int) i; }

        const int expected = (int) std::lround (10.0 * 0.001 * kSr);   // 480
        check (std::abs (peakAt - expected) <= 1,
               "10 ms at 48 kHz lands at sample " + std::to_string (expected)
               + " (got " + std::to_string (peakAt) + ")");
        check (near (peak, 1.0, 1e-3), "and comes back at full level");
    }

    std::printf ("== CHORUS: voices change the texture, not the level ==\n");
    {
        double rms[5] = {};
        for (int v = 1; v <= ChorusEngine::kMaxVoices; ++v)
        {
            ChorusEngine e;
            e.setVoices (v);
            e.setDelayMs (12.0f);
            e.setFeedbackPercent (0.0f);
            e.setMixPercent (100.0f);
            e.lfo().setDepthPercent (50.0f);
            e.lfo().setRateHz (0.6f);
            e.prepare (kSr, kBlock);

            // A tone, so the voices actually interfere rather than summing DC.
            double sum = 0.0;
            int    n   = 0;
            std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
            double phase = 0.0;
            for (int b = 0; b < (int) (kSr / kBlock); ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    const float s = (float) std::sin (phase);
                    phase += 2.0 * 3.14159265358979 * 220.0 / kSr;
                    l[(size_t) i] = s; r[(size_t) i] = s;
                }
                e.process (l.data(), r.data(), kBlock);
                for (float x : l) { sum += (double) x * x; ++n; }
            }
            rms[v] = std::sqrt (sum / n);
        }

        // Perfect equality is not the claim — interference between voices moves
        // the level around. Staying inside a few dB is.
        bool sane = true;
        for (int v = 2; v <= ChorusEngine::kMaxVoices; ++v)
            if (rms[v] < rms[1] * 0.5 || rms[v] > rms[1] * 2.0) sane = false;
        check (sane, "1..4 voices stay within a few dB of each other");
    }

    std::printf ("== CHORUS: silence in, silence out; and 95%% feedback stays bounded ==\n");
    {
        ChorusEngine e;
        e.setFeedbackPercent (95.0f);
        e.setMixPercent (100.0f);
        e.setVoices (4);
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (5.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
        for (int b = 0; b < 8; ++b)
        {
            for (auto& x : l) x = 0.0f;
            for (auto& x : r) x = 0.0f;
            e.process (l.data(), r.data(), kBlock);
        }
        check (peakOf (l) == 0.0f, "no self-noise");

        // Now hit it and let it ring for ten seconds.
        float worst = 0.0f;
        for (int b = 0; b < (int) (10.0 * kSr / kBlock); ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
                r[(size_t) i] = l[(size_t) i];
            }
            e.process (l.data(), r.data(), kBlock);
            worst = std::max (worst, peakOf (l));
            if (! allFinite (l)) { check (false, "output went non-finite"); return; }
        }
        check (worst < 8.0f, "a decade of 95% feedback stays bounded (peak "
                             + std::to_string (worst) + ")");
    }
}

// ---------------------------------------------------------------------------
static void testPhaser()
{
    std::printf ("== PHASER: the allpass coefficient is stable everywhere ==\n");
    {
        bool stable = true;
        for (float hz = PhaserEngine::kMinCentreHz; hz <= PhaserEngine::kMaxCentreHz; hz += 25.0f)
            for (double sr : { 44100.0, 48000.0, 96000.0 })
                if (std::fabs (PhaserEngine::coeffFor (hz, sr)) >= 1.0f) stable = false;
        check (stable, "|a| < 1 across the whole centre range at 44.1/48/96k");

        // Even a swept corner beyond the band stays inside the pole circle.
        bool sweptStable = true;
        for (float hz : { 100.0f, 800.0f, 8000.0f })
            for (int m = -10; m <= 10; ++m)
            {
                const float f = PhaserEngine::sweptHz (hz, (float) m / 10.0f);
                if (std::fabs (PhaserEngine::coeffFor (f, 48000.0)) >= 1.0f) sweptStable = false;
            }
        check (sweptStable, "and stays stable when the LFO sweeps it off the ends");
    }

    std::printf ("== PHASER: the sweep is logarithmic (2 octaves either way) ==\n");
    {
        check (near (PhaserEngine::sweptHz (800.0f,  0.0f),  800.0, 1e-3), "lfo 0 sits at centre");
        check (near (PhaserEngine::sweptHz (800.0f,  1.0f), 3200.0, 1e-2), "lfo +1 is two octaves up");
        check (near (PhaserEngine::sweptHz (800.0f, -1.0f),  200.0, 1e-2), "lfo -1 is two octaves down");
        // Same RATIO at another centre: that is what "sounds the same width" means.
        check (near (PhaserEngine::sweptHz (200.0f, 1.0f) / 200.0,
                     PhaserEngine::sweptHz (4000.0f, 1.0f) / 4000.0, 1e-6),
               "the sweep is the same width wherever centre is parked");
    }

    std::printf ("== PHASER: mix 0 is pass-through ==\n");
    {
        PhaserEngine e;
        e.setMixPercent (0.0f);
        e.setFeedbackPercent (90.0f);
        e.lfo().setDepthPercent (100.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool identical = true;
        for (int b = 0; b < 8; ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (l[(size_t) i] != 1.0f || r[(size_t) i] != 1.0f) identical = false;
        }
        check (identical, "a fully dry phaser does nothing");
    }

    std::printf ("== PHASER: unity at DC (an allpass passes DC through untouched) ==\n");
    {
        PhaserEngine e;
        e.setMixPercent (100.0f);
        e.setFeedbackPercent (0.0f);
        e.setStages (6);
        e.setCentreHz (800.0f);
        e.lfo().setDepthPercent (0.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        for (int b = 0; b < 40; ++b)                 // let the cascade settle
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
        }
        check (near (l[(size_t) kBlock - 1], 1.0, 1e-3), "DC comes out at unity");
    }

    std::printf ("== PHASER: an odd cascade notches Nyquist (this IS the effect) ==\n");
    {
        PhaserEngine e;
        e.setMixPercent (100.0f);
        e.setFeedbackPercent (0.0f);
        e.setStages (3);                    // odd: each stage inverts at Nyquist
        e.setCentreHz (800.0f);
        e.lfo().setDepthPercent (0.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        float tail = 1.0f;
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (i % 2 == 0) ? 1.0f : -1.0f;   // Nyquist
                r[(size_t) i] = l[(size_t) i];
            }
            e.process (l.data(), r.data(), kBlock);
            tail = std::fabs (l[(size_t) kBlock - 1]);
        }
        check (tail < 0.01f, "Nyquist cancels against the dry path");
    }

    std::printf ("== PHASER: silence in, silence out; and 95%% feedback stays bounded ==\n");
    {
        PhaserEngine e;
        e.setFeedbackPercent (95.0f);
        e.setMixPercent (100.0f);
        e.setStages (PhaserEngine::kMaxStages);
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (5.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
        for (int b = 0; b < 8; ++b)
        {
            for (auto& x : l) x = 0.0f;
            for (auto& x : r) x = 0.0f;
            e.process (l.data(), r.data(), kBlock);
        }
        check (peakOf (l) == 0.0f, "no self-noise");

        float worst = 0.0f;
        for (int b = 0; b < (int) (10.0 * kSr / kBlock); ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
                r[(size_t) i] = l[(size_t) i];
            }
            e.process (l.data(), r.data(), kBlock);
            worst = std::max (worst, peakOf (l));
            if (! allFinite (l)) { check (false, "output went non-finite"); return; }
        }
        check (worst < 8.0f, "a decade of 95% feedback stays bounded (peak "
                             + std::to_string (worst) + ")");
    }

    std::printf ("== PHASER: every stage count runs clean ==\n");
    {
        bool ok = true;
        for (int s = PhaserEngine::kMinStages; s <= PhaserEngine::kMaxStages; ++s)
        {
            PhaserEngine e;
            e.setStages (s);
            e.setMixPercent (100.0f);
            e.setFeedbackPercent (60.0f);
            e.lfo().setDepthPercent (100.0f);
            e.lfo().setRateHz (2.0f);
            e.prepare (kSr, kBlock);

            std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
            for (int b = 0; b < 40; ++b)
            {
                double phase = 0.0;
                for (int i = 0; i < kBlock; ++i)
                {
                    l[(size_t) i] = (float) std::sin (phase);
                    r[(size_t) i] = l[(size_t) i];
                    phase += 2.0 * 3.14159265358979 * 440.0 / kSr;
                }
                e.process (l.data(), r.data(), kBlock);
                if (! allFinite (l) || peakOf (l) > 8.0f) ok = false;
            }
        }
        check (ok, "2..12 stages: finite and bounded");
    }
}

int main()
{
    testTremolo();
    testAutoPan();
    testChorus();
    testPhaser();

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
