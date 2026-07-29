// Standalone test for the shared dynamics core (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source dynamics_core_test.cpp -o dyntest && ./dyntest
//
// What is worth pinning here, and why:
//
//   * The gain computer's STATIC CURVE. Every one of the six Dynamics devices is
//     this curve in a different mode, so a sign error or an off-by-one in the
//     knee is six broken devices, not one.
//   * BALLISTICS timing. "attack 10 ms" has to mean something measurable, or the
//     schema is advertising a number the DSP does not honour.
//   * The GATE's hysteresis and hold — the two things that separate a usable
//     gate from a buzzing one, and neither is visible by ear in a unit test.
//   * The 4-BAND SPLITTER's flatness. This is the one that actually catches
//     bugs: a naive crossover tree looks completely correct in code and is
//     several dB wrong near the crossovers. Measured, not asserted by reading.

#include "EedDynamicsCore.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }

static constexpr double kSr = 48000.0;

// ---------------------------------------------------------------------------
// Steady-state gain reduction for a constant-level input, in dB.
// Runs long enough for the ballistics to settle.
// ---------------------------------------------------------------------------
static float settledGrDb (DynamicsCore& c, float levelDb, double seconds = 2.0)
{
    const float amp = dyn::dbToGain (levelDb);
    const int   n   = (int) (seconds * kSr);
    for (int i = 0; i < n; ++i)
        c.gainForSidechain (amp, amp);
    return c.gainReductionDb();
}

int main()
{
    // -----------------------------------------------------------------------
    std::printf ("== gain computer: COMPRESS, hard knee ==\n");
    {
        GainCurve g;
        g.mode = DynamicsMode::Compress;
        g.thresholdDb = -20.0f; g.ratio = 4.0f; g.kneeDb = 0.0f; g.rangeDb = 0.0f;

        check (near (g.reductionDb (-40.0f), 0.0, 1e-5), "below threshold: no reduction");
        check (near (g.reductionDb (-20.0f), 0.0, 1e-5), "at threshold: no reduction");

        // 20 dB over at 4:1 -> output is 5 dB over -> 15 dB of reduction.
        check (near (g.reductionDb (0.0f), -15.0, 1e-4), "20 dB over at 4:1 = -15 dB");

        // 8 dB over at 4:1 -> 2 dB over -> -6 dB.
        check (near (g.reductionDb (-12.0f), -6.0, 1e-4), "8 dB over at 4:1 = -6 dB");

        g.ratio = 1.0f;
        check (near (g.reductionDb (0.0f), 0.0, 1e-5), "ratio 1:1 is a true bypass");
    }

    std::printf ("== gain computer: COMPRESS, soft knee is continuous ==\n");
    {
        GainCurve g;
        g.mode = DynamicsMode::Compress;
        g.thresholdDb = -20.0f; g.ratio = 4.0f; g.kneeDb = 12.0f;

        // Knee spans -26 .. -14 dB. Outside it the curve must match hard-knee.
        check (near (g.reductionDb (-30.0f), 0.0, 1e-5), "below the knee: untouched");

        GainCurve hard = g; hard.kneeDb = 0.0f;
        check (near (g.reductionDb (-5.0f), hard.reductionDb (-5.0f), 1e-4),
               "above the knee: matches the hard-knee curve");

        // Continuity: no step anywhere across the knee.
        double worstStep = 0.0, prev = g.reductionDb (-34.0f);
        for (double in = -34.0; in <= -6.0; in += 0.05)
        {
            const double v = g.reductionDb ((float) in);
            worstStep = std::max (worstStep, std::fabs (v - prev));
            prev = v;
        }
        check (worstStep < 0.05, "no discontinuity across the knee (worst step "
                                 + std::to_string (worstStep) + " dB)");

        // At the threshold a soft knee is ALREADY reducing — that is the point.
        check (g.reductionDb (-20.0f) < -0.5, "soft knee acts at the threshold");
    }

    std::printf ("== gain computer: LIMIT is a brick wall ==\n");
    {
        GainCurve g;
        g.mode = DynamicsMode::Limit;
        g.thresholdDb = -6.0f; g.kneeDb = 0.0f;

        // Whatever goes in above the ceiling comes out AT the ceiling.
        for (double in : { -6.0, -3.0, 0.0, 6.0, 20.0 })
        {
            const double out = in + g.reductionDb ((float) in);
            if (! near (out, in <= -6.0 ? in : -6.0, 1e-4))
            { check (false, "limit held at " + std::to_string (in)); goto limitDone; }
        }
        check (true, "output pinned to the ceiling for every input above it");
        limitDone:;
    }

    std::printf ("== gain computer: EXPAND pushes DOWN below threshold ==\n");
    {
        GainCurve g;
        g.mode = DynamicsMode::Expand;
        g.thresholdDb = -40.0f; g.ratio = 3.0f; g.kneeDb = 0.0f; g.rangeDb = 0.0f;

        check (near (g.reductionDb (-20.0f), 0.0, 1e-5), "above threshold: untouched");
        // 10 dB under at 3:1 -> (3-1) * 10 = 20 dB down.
        check (near (g.reductionDb (-50.0f), -20.0, 1e-4), "10 dB under at 3:1 = -20 dB");

        g.rangeDb = 12.0f;
        check (near (g.reductionDb (-50.0f), -12.0, 1e-4), "range caps the reduction at 12 dB");
        check (near (g.reductionDb (-90.0f), -12.0, 1e-4), "and stays capped however far under");
    }

    // -----------------------------------------------------------------------
    // The gate's curve is what TransferCurveView PLOTS, while the audio path
    // goes through GateState. Those are two functions that have to agree, and
    // this branch was previously an expander slope on a `ratio` the Gate device
    // never sets — dead code, and silently a lie the moment it was drawn.
    std::printf ("== gain computer: GATE is a STEP, not a steep expander ==\n");
    {
        GainCurve g;
        g.mode = DynamicsMode::Gate;
        g.thresholdDb = -40.0f; g.rangeDb = 30.0f;
        g.ratio = 4.0f;                       // must be IGNORED
        g.kneeDb = 6.0f;                      // must be IGNORED

        check (near (g.reductionDb (-20.0f), 0.0, 1e-5), "well above threshold: open, unity");
        check (near (g.reductionDb (-40.0f), 0.0, 1e-5), "AT the threshold: open");
        check (near (g.reductionDb (-40.1f), -30.0, 1e-4), "just below: straight to the floor");
        check (near (g.reductionDb (-90.0f), -30.0, 1e-4), "and no deeper however far under");

        // The knee must not round the step off, and the ratio must not tilt it.
        g.ratio = 20.0f;
        check (near (g.reductionDb (-45.0f), -30.0, 1e-4), "ratio does not change the step");
        g.kneeDb = 24.0f;
        check (near (g.reductionDb (-38.0f), 0.0, 1e-5), "knee does not soften the step");

        g.rangeDb = 0.0f;
        check (near (g.reductionDb (-50.0f), -80.0, 1e-4),
               "range 0 means the -80 dB floor, not 'no gating'");
    }

    std::printf ("== gate: the DRAWN curve and the AUDIO path share one floor ==\n");
    {
        // DynamicsCore's gate branch and GainCurve::reductionDb are two separate
        // functions the user sees as one device: the meter and the picture come
        // from one, the sound from the other. A floor defined twice is a gate
        // that draws -40 and applies -80.
        for (float range : { 0.0f, 12.0f, 40.0f })
        {
            DynamicsCore c;
            c.prepare (kSr);
            c.setMode (DynamicsMode::Gate);
            c.setDetectorMode (DetectorMode::Peak);
            c.setThresholdDb (-40.0f);
            c.setRangeDb (range);
            c.setHysteresisDb (0.0f);
            c.setAttackMs (0.05); c.setReleaseMs (5.0); c.setHoldMs (0.0);
            c.reset();

            // Well below the threshold, so the gate is closed and settled.
            const float settledDb = settledGrDb (c, -70.0f, 3.0);

            GainCurve g;
            g.mode = DynamicsMode::Gate;
            g.thresholdDb = -40.0f;
            g.rangeDb = range;
            const float drawnDb = g.reductionDb (-70.0f);

            char msg[140];
            std::snprintf (msg, sizeof (msg),
                           "range %.0f: audio settles at %.2f dB, curve draws %.2f dB",
                           range, settledDb, drawnDb);
            check (near (settledDb, drawnDb, 0.05), msg);
        }
    }

    // -----------------------------------------------------------------------
    std::printf ("== detector: peak vs RMS ==\n");
    {
        Detector d;
        d.prepare (kSr);
        d.setMode (DetectorMode::Peak);
        check (near (d.process (0.5f, 0.25f), 0.5, 1e-6), "peak links by MAX across channels");
        check (near (d.process (-0.75f, 0.1f), 0.75, 1e-6), "peak is magnitude, not sign");

        // RMS of a full-scale sine settles at 1/sqrt(2) = -3.01 dB.
        d.setMode (DetectorMode::Rms);
        d.setRmsWindowMs (10.0);
        d.reset();
        float last = 0.0f;
        for (int i = 0; i < (int) (kSr * 1.0); ++i)
        {
            const float s = (float) std::sin (2.0 * dyn::kPi * 1000.0 * i / kSr);
            last = d.process (s, s);
        }
        check (near (last, 0.70710678, 0.02), "RMS of a full-scale sine ~ 0.707");
    }

    std::printf ("== detector: silence is a finite dB, never -inf/NaN ==\n");
    {
        const float db = dyn::gainToDb (0.0f);
        check (std::isfinite (db) && db <= -100.0f, "gainToDb(0) is a finite floor");
        check (dyn::dbToGain (db) == 0.0f, "and round-trips back to true silence");
    }

    // -----------------------------------------------------------------------
    std::printf ("== ballistics: attack reaches ~63%% of the target in one tau ==\n");
    {
        Ballistics b;
        b.prepare (kSr);
        b.setAttackMs (10.0);
        b.setReleaseMs (200.0);
        b.reset();

        const int n = (int) (0.010 * kSr);       // exactly one attack time
        float v = 0.0f;
        for (int i = 0; i < n; ++i) v = b.process (-10.0f);

        check (near (v, -6.32, 0.35), "10 ms attack toward -10 dB reaches ~-6.3 dB "
                                      "(got " + std::to_string (v) + ")");

        // A slower attack must be measurably slower at the same instant.
        Ballistics slow;
        slow.prepare (kSr); slow.setAttackMs (100.0); slow.setReleaseMs (200.0); slow.reset();
        float sv = 0.0f;
        for (int i = 0; i < n; ++i) sv = slow.process (-10.0f);
        check (sv > v + 3.0f, "100 ms attack is far behind the 10 ms one at 10 ms");
    }

    std::printf ("== ballistics: release recovers to unity, exactly ==\n");
    {
        Ballistics b;
        b.prepare (kSr);
        b.setAttackMs (1.0); b.setReleaseMs (50.0); b.reset();

        for (int i = 0; i < (int) (0.05 * kSr); ++i) b.process (-12.0f);
        check (b.currentGainDb() < -10.0f, "reduced while the target asks for it");

        for (int i = 0; i < (int) (1.0 * kSr); ++i) b.process (0.0f);
        check (b.currentGainDb() == 0.0f, "returns to EXACTLY 0 dB, not 0.003 dB short");
    }

    std::printf ("== ballistics: hold freezes the recovery ==\n");
    {
        Ballistics b;
        b.prepare (kSr);
        b.setAttackMs (0.1); b.setReleaseMs (50.0); b.setHoldMs (100.0); b.reset();

        for (int i = 0; i < (int) (0.02 * kSr); ++i) b.process (-40.0f);
        const float held = b.currentGainDb();

        // 50 ms into a 100 ms hold, with a 50 ms release: without hold it would
        // already have recovered most of the way.
        for (int i = 0; i < (int) (0.05 * kSr); ++i) b.process (0.0f);
        check (near (b.currentGainDb(), held, 0.01), "still frozen inside the hold window");

        // Long enough for a 50 ms release to cross 40 dB and hit the snap: the
        // one-pole needs ~11 tau to get from -40 dB inside 0.001 dB of unity.
        for (int i = 0; i < (int) (1.0 * kSr); ++i) b.process (0.0f);
        check (b.currentGainDb() == 0.0f, "and releases once the hold expires");
    }

    // -----------------------------------------------------------------------
    std::printf ("== gate: hysteresis stops the chatter ==\n");
    {
        GateState g;
        g.reset();

        check (! g.update (-50.0f, -40.0f, 6.0f), "starts closed below threshold");
        check (g.update (-39.0f, -40.0f, 6.0f),   "opens at the threshold");

        // Between close (-46) and open (-40) the gate must HOLD its state — that
        // is the entire point, and a single-threshold gate fails right here.
        check (g.update (-43.0f, -40.0f, 6.0f), "stays open inside the hysteresis band");
        check (! g.update (-47.0f, -40.0f, 6.0f), "closes only below threshold-hysteresis");
        check (! g.update (-43.0f, -40.0f, 6.0f), "and stays closed back inside the band");
    }

    std::printf ("== core: compressor end to end ==\n");
    {
        DynamicsCore c;
        c.prepare (kSr);
        c.setMode (DynamicsMode::Compress);
        c.setDetectorMode (DetectorMode::Peak);
        c.setThresholdDb (-20.0f);
        c.setRatio (4.0f);
        c.setKneeDb (0.0f);
        c.setAttackMs (1.0);
        c.setReleaseMs (50.0);
        c.reset();

        check (near (settledGrDb (c, -40.0f), 0.0, 0.05), "quiet input: no reduction");

        c.reset();
        check (near (settledGrDb (c, 0.0f), -15.0, 0.2), "0 dBFS at -20/4:1 settles at -15 dB");

        c.reset();
        c.setRatio (2.0f);
        check (near (settledGrDb (c, 0.0f), -10.0, 0.2), "same input at 2:1 settles at -10 dB");
    }

    std::printf ("== core: unity in, unity out when nothing is triggered ==\n");
    {
        DynamicsCore c;
        c.prepare (kSr);
        c.setMode (DynamicsMode::Compress);
        c.setThresholdDb (0.0f);
        c.setRatio (4.0f);
        c.setKneeDb (0.0f);
        c.setMakeupDb (0.0f);
        c.setMix (1.0f);
        c.reset();

        std::vector<float> l (512, 0.1f), r (512, 0.1f);
        for (int b = 0; b < 20; ++b)
        {
            for (auto& v : l) v = 0.1f;
            for (auto& v : r) v = 0.1f;
            c.process (l.data(), r.data(), 512);
        }
        check (near (l[511], 0.1, 1e-5) && near (r[511], 0.1, 1e-5),
               "a signal under the threshold is passed through untouched");
    }

    std::printf ("== core: mix blends toward dry ==\n");
    {
        DynamicsCore c;
        c.prepare (kSr);
        c.setMode (DynamicsMode::Compress);
        c.setThresholdDb (-40.0f);
        c.setRatio (20.0f);
        c.setKneeDb (0.0f);
        c.setAttackMs (0.5);
        c.setReleaseMs (20.0);
        c.setMix (0.0f);           // fully dry
        c.reset();

        std::vector<float> l (512, 0.0f), r (512, 0.0f);
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < 512; ++i) { l[(size_t) i] = 0.5f; r[(size_t) i] = 0.5f; }
            c.process (l.data(), r.data(), 512);
        }
        check (near (l[511], 0.5, 1e-4), "mix 0 is bit-for-bit dry even while reducing hard");
        check (c.gainReductionDb() < -10.0f, "and the GR meter still reports the action");
    }

    std::printf ("== core: stereo LINK — both channels get the same gain ==\n");
    {
        DynamicsCore c;
        c.prepare (kSr);
        c.setMode (DynamicsMode::Compress);
        c.setThresholdDb (-30.0f);
        c.setRatio (10.0f);
        c.setKneeDb (0.0f);
        c.setAttackMs (1.0);
        c.setReleaseMs (50.0);
        c.reset();

        // Loud left, quiet right. A per-channel detector would attenuate only
        // the left and drag the image sideways.
        std::vector<float> l (512), r (512);
        for (int b = 0; b < 60; ++b)
        {
            for (int i = 0; i < 512; ++i) { l[(size_t) i] = 0.9f; r[(size_t) i] = 0.09f; }
            c.process (l.data(), r.data(), 512);
        }
        const double ratioOut = (double) r[511] / (double) l[511];
        check (near (ratioOut, 0.1, 1e-3),
               "the 20 dB L/R difference survives compression (ratio "
               + std::to_string (ratioOut) + ")");
    }

    std::printf ("== lookahead: delays by exactly the requested samples ==\n");
    {
        LookaheadDelay d;
        d.prepare (kSr, 10.0, 2);
        d.setDelayMs (1.0);
        const int expect = (int) std::lround (0.001 * kSr);
        check (d.delaySamples() == expect,
               "1 ms at 48k = " + std::to_string (expect) + " samples");

        d.reset();
        int impulseOutAt = -1;
        for (int i = 0; i < 500; ++i)
        {
            float frame[2] = { i == 0 ? 1.0f : 0.0f, 0.0f };
            d.process (frame, 2);
            if (frame[0] > 0.5f && impulseOutAt < 0) impulseOutAt = i;
        }
        check (impulseOutAt == expect, "an impulse emerges exactly that many samples late");

        d.setDelayMs (1000.0);   // far past the prepared maximum
        check (d.delaySamples() < (int) (0.011 * kSr),
               "an over-long request is CLAMPED, never reallocated on the audio thread");
    }

    // -----------------------------------------------------------------------
    std::printf ("== 4-band splitter: the four bands SUM BACK to the input ==\n");
    {
        FourBandSplitter s;
        s.prepare (kSr);
        s.setCrossovers (120.0, 800.0, 5000.0);

        // Sine sweep by frequency; at each one, compare the summed amplitude to
        // the input amplitude once the filters have settled.
        double worstErrDb = 0.0;
        double worstFreq  = 0.0;

        for (double f : { 30.0, 60.0, 100.0, 120.0, 150.0, 250.0, 500.0, 700.0,
                          800.0, 900.0, 1500.0, 3000.0, 4500.0, 5000.0, 6000.0,
                          9000.0, 15000.0 })
        {
            s.reset();
            double peakIn = 0.0, peakSum = 0.0;
            const int n = (int) (kSr * 0.5);
            const int settle = (int) (kSr * 0.2);

            for (int i = 0; i < n; ++i)
            {
                const float x = (float) std::sin (2.0 * dyn::kPi * f * i / kSr);
                float bands[4];
                s.process (0, x, bands);
                const double sum = (double) bands[0] + bands[1] + bands[2] + bands[3];

                if (i > settle)
                {
                    peakIn  = std::max (peakIn,  std::fabs ((double) x));
                    peakSum = std::max (peakSum, std::fabs (sum));
                }
            }

            const double errDb = 20.0 * std::log10 (std::max (peakSum, 1e-9)
                                                  / std::max (peakIn, 1e-9));
            if (std::fabs (errDb) > std::fabs (worstErrDb)) { worstErrDb = errDb; worstFreq = f; }
        }

        char msg[160];
        std::snprintf (msg, sizeof (msg),
                       "magnitude-flat across the spectrum (worst %.3f dB at %.0f Hz)",
                       worstErrDb, worstFreq);
        check (std::fabs (worstErrDb) < 0.3, msg);
    }

    std::printf ("== 4-band splitter: bands actually SEPARATE ==\n");
    {
        FourBandSplitter s;
        s.prepare (kSr);
        s.setCrossovers (120.0, 800.0, 5000.0);

        auto bandEnergy = [&s] (double f, int band)
        {
            s.reset();
            double peak = 0.0;
            const int n = (int) (kSr * 0.4);
            for (int i = 0; i < n; ++i)
            {
                const float x = (float) std::sin (2.0 * dyn::kPi * f * i / kSr);
                float bands[4];
                s.process (0, x, bands);
                if (i > (int) (kSr * 0.2)) peak = std::max (peak, std::fabs ((double) bands[band]));
            }
            return peak;
        };

        check (bandEnergy (50.0, 0)   > 0.9, "50 Hz lands in band 0");
        check (bandEnergy (50.0, 3)   < 0.01, "and not in band 3");
        check (bandEnergy (400.0, 1)  > 0.9, "400 Hz lands in band 1");
        check (bandEnergy (2000.0, 2) > 0.9, "2 kHz lands in band 2");
        check (bandEnergy (10000.0, 3)> 0.9, "10 kHz lands in band 3");
        check (bandEnergy (10000.0, 0)< 0.01, "and not in band 0");
    }

    std::printf ("== 4-band splitter: out-of-order crossovers are sorted, not broken ==\n");
    {
        FourBandSplitter s;
        s.prepare (kSr);
        s.setCrossovers (5000.0, 120.0, 800.0);
        check (s.crossover1() < s.crossover2() && s.crossover2() < s.crossover3(),
               "ascending after a scrambled set");
        check (near (s.crossover1(), 120.0, 1e-6)
            && near (s.crossover2(), 800.0, 1e-6)
            && near (s.crossover3(), 5000.0, 1e-6), "and the values are preserved");

        s.setCrossovers (1000.0, 1000.0, 1000.0);
        check (s.crossover1() < s.crossover2() && s.crossover2() < s.crossover3(),
               "three identical frequencies are spread rather than collapsed");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
