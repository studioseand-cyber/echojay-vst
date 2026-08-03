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

// ---------------------------------------------------------------------------
// The depth pass (DEVICE_DEPTH_PLAN.md, Modulation): each mode's audible claim,
// measured. The neutral modes need no test of their own — every suite above
// runs at the defaults and passes unchanged, which IS the neutrality proof at
// this layer; the registry test re-proves it end to end through processBlock.
// ---------------------------------------------------------------------------
static void testTremoloModes()
{
    std::printf ("== TREMOLO/optical: the photocell rounds a square's edges ==\n");
    {
        // Same square, same zero LFO smoothing; the only difference is the
        // circuit. sine mode steps hard, optical glides.
        auto worstStep = [] (TremoloMode mode)
        {
            TremoloEngine e;
            e.setMode (mode);
            e.lfo().setDepthPercent (100.0f);
            e.lfo().setRateHz (4.0f);
            e.lfo().setShape (LfoCore::kSquare);
            e.lfo().setSmoothingMs (0.0f);
            e.setMixPercent (100.0f);
            e.prepare (kSr, kBlock);

            std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
            float worst = 0.0f, prev = 1.0f;
            for (int b = 0; b < (int) (kSr / kBlock); ++b)
            {
                fillDc (l, r);
                e.process (l.data(), r.data(), kBlock);
                for (float x : l)
                {
                    worst = std::max (worst, std::fabs (x - prev));
                    prev = x;
                }
            }
            return worst;
        };

        const float hard = worstStep (TremoloMode::Sine);
        const float soft = worstStep (TremoloMode::Optical);
        check (hard > 0.5f,  "sine circuit: an unsmoothed square steps hard");
        check (soft < 0.05f, "optical circuit: the same square glides (worst step "
                             + std::to_string (soft) + ")");
    }

    std::printf ("== TREMOLO/bias: lows and highs wobble in OPPOSITE phase ==\n");
    {
        // Feed a low tone and a high tone through the harmonic circuit
        // separately and track their envelopes over one LFO cycle: when the
        // lows are at their loudest the highs must be at their quietest.
        auto envelope = [] (double toneHz, std::vector<float>& env)
        {
            TremoloEngine e;
            e.setMode (TremoloMode::Bias);
            e.lfo().setDepthPercent (100.0f);
            e.lfo().setRateHz (2.0f);
            e.lfo().setShape (LfoCore::kSine);
            e.setMixPercent (100.0f);
            e.prepare (kSr, kBlock);

            std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
            double phase = 0.0;
            const int blocks = (int) (kSr / kBlock);        // one second, 2 cycles
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    l[(size_t) i] = (float) std::sin (phase);
                    r[(size_t) i] = l[(size_t) i];
                    phase += 2.0 * 3.14159265358979 * toneHz / kSr;
                }
                e.process (l.data(), r.data(), kBlock);
                env.push_back (peakOf (l));                  // block peak = envelope
            }
        };

        std::vector<float> lowEnv, highEnv;
        envelope (100.0, lowEnv);
        envelope (6000.0, highEnv);

        // Skip the first quarter second while the crossover settles, then
        // correlate the two envelopes: anti-phase means strongly negative.
        double sumL = 0, sumH = 0;
        const size_t from = lowEnv.size() / 4;
        for (size_t i = from; i < lowEnv.size(); ++i) { sumL += lowEnv[i]; sumH += highEnv[i]; }
        const double meanL = sumL / (double) (lowEnv.size() - from);
        const double meanH = sumH / (double) (lowEnv.size() - from);

        double corr = 0, nL = 0, nH = 0;
        for (size_t i = from; i < lowEnv.size(); ++i)
        {
            const double a = lowEnv[i] - meanL, b = highEnv[i] - meanH;
            corr += a * b; nL += a * a; nH += b * b;
        }
        corr /= std::sqrt (nL * nH) + 1e-12;

        check (corr < -0.8, "low and high envelopes are anti-correlated (r = "
                            + std::to_string (corr) + ")");

        // And both bands genuinely wobble — anti-phase stillness would pass
        // the correlation test with noise.
        float lo = 2.0f, hi = -2.0f;
        for (size_t i = from; i < lowEnv.size(); ++i)
        { lo = std::min (lo, lowEnv[i]); hi = std::max (hi, lowEnv[i]); }
        check (hi - lo > 0.5f, "the low band's envelope really swings");
    }

    std::printf ("== TREMOLO/bias: depth 0 stays transparent through the crossover ==\n");
    {
        TremoloEngine e;
        e.setMode (TremoloMode::Bias);
        e.lfo().setDepthPercent (0.0f);
        e.lfo().setRateHz (5.0f);
        e.setMixPercent (100.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        float worst = 0.0f;
        double phase = 0.0;
        for (int b = 0; b < 16; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (float) std::sin (phase);
                r[(size_t) i] = l[(size_t) i];
                phase += 2.0 * 3.14159265358979 * 440.0 / kSr;
            }
            std::vector<float> ref (l);
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                worst = std::max (worst, std::fabs (l[(size_t) i] - ref[(size_t) i]));
        }
        check (worst < 1e-5f, "split-then-sum reconstructs the input (worst "
                              + std::to_string (worst) + ")");
    }
}

static void testAutoPanModes()
{
    std::printf ("== AUTO PAN/linear: the law is a straight crossfade, centre at unity ==\n");
    {
        float gl = 0, gr = 0;
        AutoPanEngine::panGainsLinear (0.0f, gl, gr);
        check (near (gl, 1.0, 1e-6) && near (gr, 1.0, 1e-6), "centre leaves both at unity");
        AutoPanEngine::panGainsLinear (1.0f, gl, gr);
        check (near (gl, 0.0, 1e-6) && near (gr, 1.0, 1e-6), "hard right silences the left");
        AutoPanEngine::panGainsLinear (-1.0f, gl, gr);
        check (near (gl, 1.0, 1e-6) && near (gr, 0.0, 1e-6), "hard left silences the right");
        AutoPanEngine::panGainsLinear (0.5f, gl, gr);
        check (near (gl, 0.5, 1e-6), "and the fade between them is linear");
    }

    std::printf ("== AUTO PAN/width: scales how far the field extends ==\n");
    {
        // Full depth, width 50: the left channel's peak gain should reach the
        // constant-power value for pan -0.5, not for pan -1.
        AutoPanEngine e;
        e.setWidthPercent (50.0f);
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (4.0f);
        e.prepare (kSr, kBlock);

        float glHalf = 0, grHalf = 0;
        GainEngine::panGains (-0.5f, glHalf, grHalf);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        float hiL = -2.0f;
        for (int b = 0; b < (int) (kSr / kBlock); ++b)
        {
            fillDc (l, r);
            e.process (l.data(), r.data(), kBlock);
            for (float x : l) hiL = std::max (hiL, x);
        }
        check (near (hiL, glHalf, 0.02), "width 50 at full depth peaks like a half-field pan ("
                                         + std::to_string (hiL) + " vs " + std::to_string (glHalf) + ")");
    }

    std::printf ("== AUTO PAN/binaural: the far ear hears it LATER, not just quieter ==\n");
    {
        // Park the pan hard right with a square LFO (first half-cycle is +1),
        // send one impulse to both channels, and find each channel's peak:
        // the left ear's copy must land later than the right ear's by roughly
        // the full inter-aural delay.
        AutoPanEngine e;
        e.setMode (AutoPanMode::Binaural);
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (0.01f);                  // one cycle per 100 s: parked
        e.lfo().setShape (LfoCore::kSquare);
        e.lfo().setSmoothingMs (0.0f);
        e.lfo().setStereoPhaseDeg (0.0f);
        e.prepare (kSr, kBlock);

        // Let the depth smoother settle at +1 before the impulse goes in.
        std::vector<float> l ((size_t) kBlock, 0.0f), r ((size_t) kBlock, 0.0f);
        for (int b = 0; b < 20; ++b)
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            e.process (l.data(), r.data(), kBlock);
        }

        std::vector<float> il (4096, 0.0f), ir (4096, 0.0f);
        il[0] = 1.0f; ir[0] = 1.0f;
        e.process (il.data(), ir.data(), (int) il.size());

        auto peakAt = [] (const std::vector<float>& v)
        {
            int at = 0; float p = 0.0f;
            for (size_t i = 0; i < v.size(); ++i)
                if (std::fabs (v[i]) > p) { p = std::fabs (v[i]); at = (int) i; }
            return at;
        };

        const int lagSamples = peakAt (il) - peakAt (ir);
        const int expected   = (int) std::lround (AutoPanEngine::kMaxItdMs * 0.001 * kSr);
        check (std::abs (lagSamples - expected) <= 2,
               "panned hard right, the left channel lags by ~" + std::to_string (expected)
               + " samples (got " + std::to_string (lagSamples) + ")");
    }
}

static void testChorusModes()
{
    std::printf ("== CHORUS/spread 0: both channels come out identical ==\n");
    {
        ChorusEngine e;
        e.setSpreadPercent (0.0f);
        e.setMixPercent (100.0f);
        e.lfo().setDepthPercent (60.0f);
        e.lfo().setRateHz (1.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool identical = true;
        double phase = 0.0;
        for (int b = 0; b < 16; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (float) std::sin (phase);
                r[(size_t) i] = l[(size_t) i];
                phase += 2.0 * 3.14159265358979 * 330.0 / kSr;
            }
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (l[(size_t) i] != r[(size_t) i]) identical = false;
        }
        check (identical, "a spread of 0 is a mono modulator");
    }

    std::printf ("== CHORUS/dimension: nothing sweeps — the comb is FROZEN ==\n");
    {
        // Two impulses far apart must come back at the SAME delay. In classic
        // mode the LFO would have moved the read position between them.
        auto tapSpread = [] (ChorusMode mode)
        {
            ChorusEngine e;
            e.setMode (mode);
            e.setDelayMs (15.0f);
            e.setVoices (1);
            e.setFeedbackPercent (0.0f);
            e.setMixPercent (100.0f);
            e.lfo().setDepthPercent (100.0f);
            e.lfo().setRateHz (2.0f);
            e.prepare (kSr, kBlock);

            auto findEcho = [&e] ()
            {
                std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
                l[0] = 1.0f; r[0] = 1.0f;
                e.process (l.data(), r.data(), (int) l.size());
                int at = 0; float p = 0.0f;
                for (size_t i = 32; i < l.size(); ++i)      // skip the dry spike
                    if (std::fabs (l[i]) > p) { p = std::fabs (l[i]); at = (int) i; }
                return at;
            };

            const int first = findEcho();
            const int second = findEcho();                  // ~0.17 s later
            return std::abs (second - first);
        };

        check (tapSpread (ChorusMode::Dimension) <= 1,
               "dimension: the echo never moves");
        check (tapSpread (ChorusMode::Classic) > 4,
               "classic (same settings): the sweep moves it - the control works");
    }

    std::printf ("== CHORUS/dimension: left and right are DIFFERENT combs ==\n");
    {
        ChorusEngine e;
        e.setMode (ChorusMode::Dimension);
        e.setDelayMs (10.0f);
        e.setVoices (3);
        e.setFeedbackPercent (0.0f);
        e.setMixPercent (100.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
        l[0] = 1.0f; r[0] = 1.0f;
        e.process (l.data(), r.data(), (int) l.size());

        float diff = 0.0f;
        for (size_t i = 64; i < l.size(); ++i)
            diff = std::max (diff, std::fabs (l[i] - r[i]));
        check (diff > 0.1f, "the mirrored stagger decorrelates the channels (max diff "
                            + std::to_string (diff) + ")");
    }

    std::printf ("== CHORUS/ensemble: more voices than the dial, still bounded ==\n");
    {
        ChorusEngine e;
        e.setMode (ChorusMode::Ensemble);
        e.setVoices (4);                                    // + 2 from the mode
        e.setDelayMs (12.0f);
        e.setFeedbackPercent (60.0f);
        e.setMixPercent (100.0f);
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (3.0f);
        e.prepare (kSr, kBlock);

        check (e.effectiveVoices() == 6, "4 dialled + 2 from the mode = 6 running");

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool ok = true;
        double phase = 0.0;
        for (int b = 0; b < (int) (2.0 * kSr / kBlock); ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (float) std::sin (phase);
                r[(size_t) i] = l[(size_t) i];
                phase += 2.0 * 3.14159265358979 * 220.0 / kSr;
            }
            e.process (l.data(), r.data(), kBlock);
            if (! allFinite (l) || peakOf (l) > 8.0f) ok = false;
        }
        check (ok, "two seconds at full tilt: finite and bounded");
    }

    std::printf ("== CHORUS/tone: the tilt darkens or brightens the WET only ==\n");
    {
        // A bright test signal (impulse train) through full-wet chorus at
        // tone -6 vs +6: the dark setting must carry less high-frequency
        // energy. Measured as first-difference energy, a cheap HF proxy.
        auto hfEnergy = [] (float toneDb)
        {
            ChorusEngine e;
            e.setToneDb (toneDb);
            e.setMixPercent (100.0f);
            e.setDelayMs (10.0f);
            e.lfo().setDepthPercent (20.0f);
            e.lfo().setRateHz (1.0f);
            e.prepare (kSr, kBlock);

            std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
            double hf = 0.0;
            for (int b = 0; b < 32; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    l[(size_t) i] = (i % 64 == 0) ? 1.0f : 0.0f;
                    r[(size_t) i] = l[(size_t) i];
                }
                e.process (l.data(), r.data(), kBlock);
                for (int i = 1; i < kBlock; ++i)
                {
                    const double d = (double) l[(size_t) i] - l[(size_t) i - 1];
                    hf += d * d;
                }
            }
            return hf;
        };

        const double dark = hfEnergy (-6.0f);
        const double bright = hfEnergy (6.0f);
        check (dark < bright * 0.7,
               "tone -6 carries measurably less HF than +6 ("
               + std::to_string (dark) + " vs " + std::to_string (bright) + ")");
    }
}

static void testPhaserModes()
{
    std::printf ("== PHASER/vintage: capped stages, darker wet, still stable ==\n");
    {
        PhaserEngine e;
        e.setMode (PhaserMode::Vintage);
        e.setStages (12);
        check (e.effectiveStages() == 4, "12 dialled stages run as 4 in vintage");

        // Same bright signal through modern and vintage at identical dials:
        // vintage's wet low-pass must lose HF energy.
        auto hfEnergy = [] (PhaserMode mode)
        {
            PhaserEngine p;
            p.setMode (mode);
            p.setStages (6);
            p.setMixPercent (100.0f);
            p.setFeedbackPercent (40.0f);
            p.lfo().setDepthPercent (70.0f);
            p.lfo().setRateHz (1.0f);
            p.prepare (kSr, kBlock);

            std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
            double hf = 0.0;
            for (int b = 0; b < 32; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    l[(size_t) i] = (i % 48 == 0) ? 1.0f : 0.0f;
                    r[(size_t) i] = l[(size_t) i];
                }
                p.process (l.data(), r.data(), kBlock);
                for (int i = 1; i < kBlock; ++i)
                {
                    const double d = (double) l[(size_t) i] - l[(size_t) i - 1];
                    hf += d * d;
                }
            }
            return hf;
        };

        check (hfEnergy (PhaserMode::Vintage) < hfEnergy (PhaserMode::Modern) * 0.85,
               "vintage is measurably darker than modern at the same dials");

        // The hotter feedback stays bounded even from the dial's ceiling.
        PhaserEngine hot;
        hot.setMode (PhaserMode::Vintage);
        hot.setFeedbackPercent (95.0f);
        hot.setMixPercent (100.0f);
        hot.lfo().setDepthPercent (100.0f);
        hot.lfo().setRateHz (5.0f);
        hot.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool ok = true;
        for (int b = 0; b < (int) (2.0 * kSr / kBlock); ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
                r[(size_t) i] = l[(size_t) i];
            }
            hot.process (l.data(), r.data(), kBlock);
            if (! allFinite (l) || peakOf (l) > 8.0f) ok = false;
        }
        check (ok, "95% dialled feedback times the vintage push stays bounded");
    }

    std::printf ("== PHASER/stereo_spread 0: both channels sweep as one ==\n");
    {
        PhaserEngine e;
        e.setStereoSpreadDeg (0.0f);
        e.setMixPercent (100.0f);
        e.setFeedbackPercent (40.0f);
        e.lfo().setDepthPercent (100.0f);
        e.lfo().setRateHz (2.0f);
        e.prepare (kSr, kBlock);

        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        bool identical = true;
        double phase = 0.0;
        for (int b = 0; b < 32; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                l[(size_t) i] = (float) std::sin (phase);
                r[(size_t) i] = l[(size_t) i];
                phase += 2.0 * 3.14159265358979 * 440.0 / kSr;
            }
            e.process (l.data(), r.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (l[(size_t) i] != r[(size_t) i]) identical = false;
        }
        check (identical, "spread 0 collapses the channel offset");
    }

    std::printf ("== PHASER/stereo: the right channel's notches sit elsewhere ==\n");
    {
        // Same input to both channels; in stereo mode the right channel's
        // lifted centre must make L and R differ MORE than modern does at the
        // same spread.
        auto channelDiff = [] (PhaserMode mode)
        {
            PhaserEngine p;
            p.setMode (mode);
            p.setStereoSpreadDeg (0.0f);       // isolate the centre lift
            p.setMixPercent (100.0f);
            p.setFeedbackPercent (30.0f);
            p.lfo().setDepthPercent (50.0f);
            p.lfo().setRateHz (1.0f);
            p.prepare (kSr, kBlock);

            std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
            double diff = 0.0;
            double phase = 0.0;
            for (int b = 0; b < 32; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    l[(size_t) i] = (float) std::sin (phase);
                    r[(size_t) i] = l[(size_t) i];
                    phase += 2.0 * 3.14159265358979 * 880.0 / kSr;
                }
                p.process (l.data(), r.data(), kBlock);
                for (int i = 0; i < kBlock; ++i)
                    diff += std::fabs ((double) l[(size_t) i] - r[(size_t) i]);
            }
            return diff;
        };

        const double modern = channelDiff (PhaserMode::Modern);
        const double stereo = channelDiff (PhaserMode::Stereo);
        check (modern < 1e-6, "modern at spread 0 keeps the channels identical");
        check (stereo > 1.0,  "stereo mode separates them even at spread 0");
    }
}

int main()
{
    testTremolo();
    testAutoPan();
    testChorus();
    testPhaser();
    testTremoloModes();
    testAutoPanModes();
    testChorusModes();
    testPhaserModes();

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
