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
//   * The DWELL HISTOGRAM the transfer curve glows along. Every failure here is
//     a picture that LIES — silence lighting the floor of every plot, two levels
//     collapsing into their average, energy that never fades — and none of them
//     is visible in the editor, where the only available test is "the glow looks
//     about right".

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

// ---------------------------------------------------------------------------
// The dwell histogram, read the way an editor reads it.
// ---------------------------------------------------------------------------
static void feedLevel (DynamicsCore& c, float amp, double seconds)
{
    const int n = (int) (seconds * kSr);
    for (int i = 0; i < n; ++i)
        c.gainForSidechain (amp, amp);
}

static bool readDwell (const DynamicsCore& c, float* bins)
{
    return c.dwellHistogram().read (bins);
}

static float dwellTotal (const DynamicsCore& c)
{
    float bins[dyn::kDwellBins];
    if (! readDwell (c, bins)) return -1.0f;

    float sum = 0.0f;
    for (float v : bins) sum += v;
    return sum;
}

// The dB the histogram says the signal spent most of its time at.
static float hottestDwellDb (const DynamicsCore& c)
{
    float bins[dyn::kDwellBins];
    if (! readDwell (c, bins)) return dyn::kSilenceDb;

    int best = 0;
    for (int i = 1; i < dyn::kDwellBins; ++i)
        if (bins[i] > bins[best]) best = i;

    return dyn::dwellBinCentreDb (best);
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

    std::printf ("== lookahead: ZERO delay is a true pass-through ==\n");
    {
        // The bug this pins: reading the ring before writing it made a delay of 0
        // come out as a delay of the whole BUFFER, while the device reported zero
        // latency to the host. One track a few milliseconds late, only when its
        // lookahead was dialled to nothing, and nothing anywhere to see.
        LookaheadDelay d;
        d.prepare (kSr, 10.0, 2);
        d.setDelayMs (0.0);
        check (d.delaySamples() == 0, "0 ms is 0 samples");

        int impulseOutAt = -1;
        for (int i = 0; i < 2000 && impulseOutAt < 0; ++i)
        {
            float frame[2] = { i == 0 ? 1.0f : 0.0f, 0.0f };
            d.process (frame, 2);
            if (frame[0] > 0.5f) impulseOutAt = i;
        }
        check (impulseOutAt == 0, "and the impulse comes straight back out (sample "
                                  + std::to_string (impulseOutAt) + ")");
    }

    std::printf ("== lookahead in the core: the detector runs AHEAD of the audio ==\n");
    {
        // What lookahead buys, measured: a transient's front is already reduced by
        // the time it reaches the output, so the overshoot past the threshold is
        // smaller than with the same attack and no lookahead.
        auto overshoot = [] (double lookMs)
        {
            DynamicsCore c;
            c.setMaxLookaheadMs (10.0);
            c.prepare (kSr);
            c.setMode (DynamicsMode::Limit);
            c.setDetectorMode (DetectorMode::Peak);
            c.setThresholdDb (-12.0f);
            c.setKneeDb (0.0f);
            c.setAttackMs (2.0);
            c.setReleaseMs (50.0);
            c.setLookaheadMs (lookMs);
            c.reset();

            std::vector<float> l (2048, 0.0f), r (2048, 0.0f);

            // Silence, then a sudden full-scale burst: the worst case for an
            // attack, and the reason lookahead exists.
            for (int i = 1024; i < 2048; ++i) l[(size_t) i] = r[(size_t) i] = 1.0f;

            c.process (l.data(), r.data(), 2048);

            float peak = 0.0f;
            for (float v : l) peak = std::max (peak, std::fabs (v));
            return dyn::gainToDb (peak);
        };

        const float noLook   = overshoot (0.0);
        const float withLook = overshoot (5.0);

        check (noLook > -8.0f,
               "with no lookahead the burst overshoots the -12 dB ceiling badly ("
               + std::to_string (noLook) + " dBFS)");
        check (withLook < noLook - 3.0f,
               "5 ms of lookahead cuts that overshoot right down ("
               + std::to_string (withLook) + " dBFS)");

        // And the latency is reported, in the units the host needs.
        DynamicsCore c;
        c.setMaxLookaheadMs (10.0);
        c.prepare (kSr);
        c.setLookaheadMs (5.0);
        check (c.lookaheadSamples() == (int) std::lround (0.005 * kSr),
               "5 ms at 48k is " + std::to_string (c.lookaheadSamples()) + " samples");
        c.setLookaheadMs (0.0);
        check (c.lookaheadSamples() == 0, "and 0 ms is 0 samples of reported latency");
    }

    // -----------------------------------------------------------------------
    // THE CHARACTER MODES. Each one is a claim about how the device behaves, and
    // the claim is in the advertisement the model reads — "punch is a very fast
    // attack", "glue is slow and gentle". A mode whose timing does not match its
    // description is worse than no mode at all: the model dials it confidently
    // and gets something else.
    // -----------------------------------------------------------------------
    std::printf ("== character: each mode reshapes the dialled times as advertised ==\n");
    {
        auto timesFor = [] (CharacterMode m)
        {
            DynamicsCore c;
            c.prepare (kSr);
            c.setCharacter (m);
            c.setAttackMs (10.0);
            c.setReleaseMs (100.0);
            return std::pair<double, double> { c.effectiveAttackMs(), c.effectiveReleaseMs() };
        };

        const auto clean  = timesFor (CharacterMode::Clean);
        const auto glue   = timesFor (CharacterMode::Glue);
        const auto punch  = timesFor (CharacterMode::Punch);
        const auto smooth = timesFor (CharacterMode::Smooth);

        check (near (clean.first, 10.0, 1e-9) && near (clean.second, 100.0, 1e-9),
               "clean runs the dialled times UNCHANGED - it is the reference");

        // The ordering IS the specification: punch is the fastest attack of the
        // four, smooth the slowest, and glue sits between clean and smooth.
        check (punch.first < clean.first,  "punch attacks faster than clean");
        check (clean.first < glue.first,   "glue attacks slower than clean");
        check (glue.first  < smooth.first, "smooth is the slowest attack of the four");

        check (punch.second < clean.second, "punch releases faster than clean");
        check (glue.second  > clean.second, "glue releases slower than clean");
        check (glue.second  > 2.0 * clean.second,
               "and much slower - a bus compressor lets go over a phrase");

        check (smooth.second > clean.second, "smooth releases slower than clean too");
        check (smooth.second < glue.second,
               "but its slowness is programme-dependent rather than dialled long");

        // The dialled values must survive the mode entirely, or a mode change is
        // a destructive edit of the user's knobs.
        DynamicsCore c;
        c.prepare (kSr);
        c.setAttackMs (7.5); c.setReleaseMs (250.0); c.setKneeDb (3.0);
        c.setCharacter (CharacterMode::Punch);
        check (near (c.getAttackMs(), 7.5, 1e-9)
            && near (c.getReleaseMs(), 250.0, 1e-9)
            && near (c.getKneeDb(), 3.0, 1e-6),
               "the DIALLED values round-trip whatever the mode is");
        c.setCharacter (CharacterMode::Clean);
        check (near (c.effectiveAttackMs(), 7.5, 1e-9),
               "and going back to clean restores the dialled attack exactly");
    }

    std::printf ("== character: the timing difference is MEASURABLE, not just stored ==\n");
    {
        // The scales could be plumbed into a getter and never reach the envelope
        // follower. So this measures the gain reduction the core actually applies
        // 2 ms into a step, which is the thing a listener hears.
        auto grAfter = [] (CharacterMode m, double seconds)
        {
            DynamicsCore c;
            c.prepare (kSr);
            c.setCharacter (m);
            c.setMode (DynamicsMode::Compress);
            c.setDetectorMode (DetectorMode::Peak);
            c.setThresholdDb (-20.0f);
            c.setRatio (4.0f);
            c.setKneeDb (0.0f);
            c.setAttackMs (10.0);
            c.setReleaseMs (100.0);
            c.reset();

            const float amp = dyn::dbToGain (0.0f);
            const int   n   = (int) (seconds * kSr);
            for (int i = 0; i < n; ++i) c.gainForSidechain (amp, amp);
            return c.gainReductionDb();
        };

        const float punchGr = grAfter (CharacterMode::Punch,  0.002);
        const float cleanGr = grAfter (CharacterMode::Clean,  0.002);
        const float smoothGr = grAfter (CharacterMode::Smooth, 0.002);

        check (punchGr < cleanGr - 1.0f,
               "2 ms in, punch has already pulled further down (" + std::to_string (punchGr)
               + " vs " + std::to_string (cleanGr) + " dB)");
        check (smoothGr > cleanGr + 0.5f,
               "and smooth is still behind clean (" + std::to_string (smoothGr) + " dB)");

        // Given time, every mode arrives at the same static curve: character is
        // the JOURNEY, not the destination. A mode that changed the settled
        // reduction would be silently changing the ratio.
        check (near (grAfter (CharacterMode::Punch, 2.0), -15.0, 0.3)
            && near (grAfter (CharacterMode::Glue,  2.0), -15.0, 0.3),
               "all modes settle on the SAME -15 dB the curve asks for");
    }

    std::printf ("== character: the knee follows the mode ==\n");
    {
        DynamicsCore c;
        c.prepare (kSr);
        c.setKneeDb (6.0f);

        c.setCharacter (CharacterMode::Clean);
        check (near (c.effectiveKneeDb(), 6.0, 1e-6), "clean runs the dialled knee");

        c.setCharacter (CharacterMode::Punch);
        check (c.effectiveKneeDb() < 3.0f, "punch hardens the corner");

        c.setCharacter (CharacterMode::Glue);
        check (c.effectiveKneeDb() > 6.0f, "glue softens it");

        c.setCharacter (CharacterMode::Smooth);
        check (c.effectiveKneeDb() > 12.0f, "and smooth softens it most");

        // A hard 0 dB knee in glue must still be gentle, which is why the spec
        // carries an ADD and not only a scale.
        c.setKneeDb (0.0f);
        check (c.effectiveKneeDb() > 0.0f,
               "a 0 dB knee in glue is still soft (add, not just scale)");
        c.setCharacter (CharacterMode::Clean);
        check (near (c.effectiveKneeDb(), 0.0, 1e-6), "clean leaves a hard knee hard");
    }

    std::printf ("== character: the colour is nonlinear, and only while working ==\n");
    {
        // Nonlinearity measured as "does the same shaper have the same gain at two
        // levels" — which is exactly what generating harmonics means, and needs no
        // FFT to state.
        auto gainAt = [] (float x, float blend)
        { return characterShape (x, blend) / x; };

        check (near (gainAt (0.9f, 0.0f), gainAt (0.2f, 0.0f), 1e-6),
               "blend 0 is perfectly linear (clean is identity at every level)");

        check (gainAt (0.9f, 0.2f) < gainAt (0.2f, 0.2f) - 0.01f,
               "with drive in, a loud sample is attenuated more than a quiet one "
               "- that difference IS the harmonic content");

        check (near (characterShape (0.01f, 0.2f), 0.01, 1e-4),
               "and small signals pass at unity, so a mode is not a level change");

        // The device-level contract: idle means identity, whatever the mode.
        DynamicsCore c;
        c.prepare (kSr);
        c.setCharacter (CharacterMode::Punch);
        c.setMode (DynamicsMode::Compress);
        c.setThresholdDb (0.0f);
        c.setRatio (4.0f);
        c.setKneeDb (0.0f);
        c.reset();

        std::vector<float> l (512, 0.1f), r (512, 0.1f);
        for (int b = 0; b < 20; ++b)
        {
            for (int i = 0; i < 512; ++i) { l[(size_t) i] = 0.1f; r[(size_t) i] = 0.1f; }
            c.process (l.data(), r.data(), 512);
        }
        check (near (l[511], 0.1, 1e-5),
               "punch under its threshold is bit-transparent - no reduction, no colour");

        // And that it DOES colour once it is reducing.
        c.setThresholdDb (-30.0f);
        c.setRatio (10.0f);
        c.setAttackMs (0.5); c.setReleaseMs (20.0);
        c.reset();
        for (int b = 0; b < 60; ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = (float) std::sin (2.0 * dyn::kPi * 100.0 * (b * 512 + i) / kSr);
                l[(size_t) i] = 0.8f * v; r[(size_t) i] = 0.8f * v;
            }
            c.process (l.data(), r.data(), 512);
        }
        check (c.characterBlend() > 0.05f,
               "punch pulling hard has its colour fully in (blend "
               + std::to_string (c.characterBlend()) + ")");

        DynamicsCore clean;
        clean.prepare (kSr);
        clean.setCharacter (CharacterMode::Clean);
        clean.setMode (DynamicsMode::Compress);
        clean.setThresholdDb (-30.0f);
        clean.setRatio (10.0f);
        clean.reset();
        for (int i = 0; i < (int) kSr; ++i) clean.gainForSidechain (0.8f, 0.8f);
        check (clean.characterBlend() == 0.0f,
               "and clean has none however hard it is pulling");
    }

    std::printf ("== auto release: recovery slows down as the reduction deepens ==\n");
    {
        // Time for the reduction to come back within 1 dB of unity, from a step.
        auto recoveryMs = [] (bool autoRel, float inDb)
        {
            DynamicsCore c;
            c.prepare (kSr);
            c.setMode (DynamicsMode::Compress);
            c.setDetectorMode (DetectorMode::Peak);
            c.setThresholdDb (-30.0f);
            c.setRatio (10.0f);
            c.setKneeDb (0.0f);
            c.setAttackMs (1.0);
            c.setReleaseMs (100.0);
            c.setAutoRelease (autoRel);
            c.reset();

            const float amp = dyn::dbToGain (inDb);
            for (int i = 0; i < (int) (0.5 * kSr); ++i) c.gainForSidechain (amp, amp);

            int n = 0;
            const int limit = (int) (10.0 * kSr);
            while (c.gainReductionDb() < -1.0f && n < limit)
            { c.gainForSidechain (0.0f, 0.0f); ++n; }

            return 1000.0 * (double) n / kSr;
        };

        const double offDeep = recoveryMs (false, 0.0f);
        const double onDeep  = recoveryMs (true,  0.0f);
        const double onShallow = recoveryMs (true, -27.0f);

        check (onDeep > offDeep * 1.5,
               "auto release recovers far slower from a deep reduction ("
               + std::to_string (onDeep) + " vs " + std::to_string (offDeep) + " ms)");
        check (onShallow < onDeep,
               "and faster from a shallow one - that dependence is the whole point ("
               + std::to_string (onShallow) + " ms)");

        // smooth is programme-dependent BY DEFINITION, so it must not need the
        // switch — and turning the switch off must not take it away.
        DynamicsCore s;
        s.prepare (kSr);
        s.setCharacter (CharacterMode::Smooth);
        check (s.isProgramRelease(), "smooth is programme-dependent without auto_release");
        s.setAutoRelease (false);
        check (s.isProgramRelease(), "and stays so when auto_release is switched off");

        DynamicsCore v;
        v.prepare (kSr);
        v.setCharacter (CharacterMode::Clean);
        check (! v.isProgramRelease(), "clean is not, unless asked");
        v.setAutoRelease (true);
        check (v.isProgramRelease(), "and is when asked");
    }

    // -----------------------------------------------------------------------
    // THE SIDECHAIN FILTER. The failure this catches is the one that matters:
    // a filter wired into the AUDIO instead of the detector. That version
    // measures correctly, compresses correctly, and quietly high-passes the
    // track — which sounds like a mix decision rather than like a bug.
    // -----------------------------------------------------------------------
    std::printf ("== sidechain HPF: the detector goes deaf to the bass ==\n");
    {
        auto grForTone = [] (double hz, double hpfHz)
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
            c.setSidechainHpfHz (hpfHz);
            c.reset();

            float worst = 0.0f;
            const int n = (int) (1.0 * kSr);
            for (int i = 0; i < n; ++i)
            {
                const float x = (float) std::sin (2.0 * dyn::kPi * hz * i / kSr);
                c.gainForSidechain (x, x);
                if (i > (int) (0.5 * kSr)) worst = std::min (worst, c.gainReductionDb());
            }
            return worst;
        };

        const float bassOpen = grForTone (50.0, 0.0);
        const float bassHpf  = grForTone (50.0, 250.0);
        const float midHpf   = grForTone (2000.0, 250.0);

        check (bassOpen < -10.0f,
               "with the sidechain open, a 50 Hz tone at 0 dBFS is compressed hard ("
               + std::to_string (bassOpen) + " dB)");
        check (bassHpf > bassOpen + 8.0f,
               "high-passing the sidechain at 250 Hz drops that reduction sharply ("
               + std::to_string (bassHpf) + " dB)");
        check (near (midHpf, bassOpen, 1.0),
               "while a 2 kHz tone is compressed exactly as before ("
               + std::to_string (midHpf) + " dB)");

        // 0 IS OFF, as advertised - not a 0 Hz filter and not a clamped 10 Hz one.
        check (near (grForTone (50.0, 0.0), grForTone (50.0, 5.0), 0.01),
               "0 and any value under 20 Hz both mean OFF");
    }

    std::printf ("== sidechain LPF: triggering can ignore the top end ==\n");
    {
        auto grForTone = [] (double hz, double lpfHz)
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
            c.setSidechainLpfHz (lpfHz);
            c.reset();

            float worst = 0.0f;
            const int n = (int) (1.0 * kSr);
            for (int i = 0; i < n; ++i)
            {
                const float x = (float) std::sin (2.0 * dyn::kPi * hz * i / kSr);
                c.gainForSidechain (x, x);
                if (i > (int) (0.5 * kSr)) worst = std::min (worst, c.gainReductionDb());
            }
            return worst;
        };

        const float openTop = grForTone (8000.0, 20000.0);
        const float lpfTop  = grForTone (8000.0, 200.0);
        const float lpfLow  = grForTone (100.0,  200.0);

        check (openTop < -10.0f, "wide open, an 8 kHz tone triggers fully");
        check (lpfTop > openTop + 8.0f,
               "a 200 Hz sidechain low-pass makes the detector nearly deaf to it ("
               + std::to_string (lpfTop) + " dB)");
        check (lpfLow < -8.0f, "while a 100 Hz tone still triggers through it ("
               + std::to_string (lpfLow) + " dB)");
    }

    std::printf ("== the sidechain filter NEVER touches the audio ==\n");
    {
        // A 50 Hz tone, an aggressive sidechain high-pass, and a threshold the
        // signal cannot reach once the detector is filtered. The output must come
        // back untouched: no reduction AND no filtering.
        DynamicsCore c;
        c.prepare (kSr);
        c.setMode (DynamicsMode::Compress);
        c.setThresholdDb (-6.0f);
        c.setRatio (10.0f);
        c.setKneeDb (0.0f);
        c.setSidechainHpfHz (500.0);
        c.reset();

        std::vector<float> l (512), r (512), want (512);
        double worst = 0.0;
        for (int b = 0; b < 20; ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.5f * (float) std::sin (2.0 * dyn::kPi * 50.0
                                                         * (b * 512 + i) / kSr);
                l[(size_t) i] = r[(size_t) i] = want[(size_t) i] = v;
            }
            c.process (l.data(), r.data(), 512);
            for (int i = 0; i < 512; ++i)
                worst = std::max (worst, std::fabs ((double) l[(size_t) i]
                                                  - want[(size_t) i]));
        }
        check (worst < 1.0e-6,
               "the 50 Hz output is bit-identical to its input (worst dev "
               + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== detector: peak and RMS disagree on a TRANSIENT ==\n");
    {
        // A short spike on a quiet bed. Peak sees the spike's full height; RMS
        // averages it away, which is the entire difference between the two and
        // the reason both are published.
        auto grForSpike = [] (DetectorMode m)
        {
            DynamicsCore c;
            c.prepare (kSr);
            c.setDetectorMode (m);
            c.setRmsWindowMs (10.0);
            c.setMode (DynamicsMode::Compress);
            c.setThresholdDb (-20.0f);
            c.setRatio (4.0f);
            c.setKneeDb (0.0f);
            c.setAttackMs (0.1);
            c.setReleaseMs (100.0);
            c.reset();

            float worst = 0.0f;
            const int n = (int) (0.2 * kSr);
            const int spikeStart = (int) (0.1 * kSr);
            const int spikeLen   = (int) (0.0005 * kSr);      // 0.5 ms

            for (int i = 0; i < n; ++i)
            {
                const bool  inSpike = i >= spikeStart && i < spikeStart + spikeLen;
                const float x = inSpike ? 1.0f : dyn::dbToGain (-40.0f);
                c.gainForSidechain (x, x);
                worst = std::min (worst, c.gainReductionDb());
            }
            return worst;
        };

        const float peakGr = grForSpike (DetectorMode::Peak);
        const float rmsGr  = grForSpike (DetectorMode::Rms);

        check (peakGr < -8.0f, "peak catches the 0.5 ms spike ("
               + std::to_string (peakGr) + " dB)");
        check (rmsGr > peakGr + 4.0f,
               "RMS averages most of it away ("+ std::to_string (rmsGr) + " dB) - a "
               "compressor rides loudness, a limiter chases transients");
    }

    std::printf ("== detector: TRUE PEAK sees between the samples ==\n");
    {
        // A tone at a quarter of the sample rate, phased so no sample lands on a
        // crest: the sample peak reads cos(45 deg) = -3 dB, and the real waveform
        // reaches full scale between them. This is exactly the case that makes a
        // sample-peak limiter hand a clipped file to an encoder.
        auto peakOf = [] (bool truePeak)
        {
            Detector d;
            d.prepare (kSr);
            d.setMode (DetectorMode::Peak);
            d.setTruePeak (truePeak);
            d.reset();

            float worst = 0.0f;
            for (int i = 0; i < 2000; ++i)
            {
                const float x = (float) std::sin (2.0 * dyn::kPi * 0.25 * i
                                                  + dyn::kPi * 0.25);
                if (i > 8) worst = std::max (worst, d.process (x, x));
                else d.process (x, x);
            }
            return worst;
        };

        const float sample = peakOf (false);
        const float truePk = peakOf (true);

        check (near (sample, 0.7071, 0.01),
               "sample peak reads 0.707, 3 dB low (" + std::to_string (sample) + ")");
        check (truePk > sample + 0.15f,
               "true peak recovers most of the missing 3 dB ("
               + std::to_string (truePk) + ")");
        check (truePk <= 1.02f, "and does not overshoot full scale");
    }

    // -----------------------------------------------------------------------
    std::printf ("== stereo link: 100%% is one gain, 0%% is two ==\n");
    {
        // Loud left, quiet right, hard compression. Linked, the L/R ratio comes
        // out unchanged; unlinked, the loud side is pulled down and the ratio
        // closes up — which IS the image moving, and is the point of the control.
        auto ratioOut = [] (float link)
        {
            DynamicsCore c;
            c.prepare (kSr);
            c.setMode (DynamicsMode::Compress);
            c.setThresholdDb (-30.0f);
            c.setRatio (10.0f);
            c.setKneeDb (0.0f);
            c.setAttackMs (1.0);
            c.setReleaseMs (50.0);
            c.setStereoLink (link);
            c.reset();

            std::vector<float> l (512), r (512);
            for (int b = 0; b < 60; ++b)
            {
                for (int i = 0; i < 512; ++i) { l[(size_t) i] = 0.9f; r[(size_t) i] = 0.09f; }
                c.process (l.data(), r.data(), 512);
            }
            return (double) r[511] / (double) l[511];
        };

        check (near (ratioOut (1.0f), 0.1, 1e-3),
               "fully linked: the 20 dB difference survives (" + std::to_string (ratioOut (1.0f)) + ")");
        check (ratioOut (0.0f) > 0.3,
               "fully unlinked: the quiet side is left alone and the pair closes up ("
               + std::to_string (ratioOut (0.0f)) + ")");
        const double half = ratioOut (0.5f);
        check (half > 0.1 + 1e-3 && half < ratioOut (0.0f),
               "and 50% sits between the two (" + std::to_string (half) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== duck: the gate's step, the other way up ==\n");
    {
        GainCurve g;
        g.mode = DynamicsMode::Duck;
        g.thresholdDb = -30.0f; g.rangeDb = 12.0f;

        check (near (g.reductionDb (-50.0f), 0.0, 1e-5), "below threshold: untouched");
        check (near (g.reductionDb (-30.0f), -12.0, 1e-4), "AT the threshold: ducked");
        check (near (g.reductionDb (0.0f), -12.0, 1e-4), "and no deeper however far over");

        // End to end, and against the gate: the same settings must produce
        // OPPOSITE reductions, or the mode is not doing anything.
        auto settled = [] (DynamicsMode m, float levelDb)
        {
            DynamicsCore c;
            c.prepare (kSr);
            c.setMode (m);
            c.setDetectorMode (DetectorMode::Peak);
            c.setThresholdDb (-30.0f);
            c.setRangeDb (12.0f);
            c.setHysteresisDb (0.0f);
            c.setAttackMs (0.5); c.setReleaseMs (20.0); c.setHoldMs (0.0);
            c.reset();
            return settledGrDb (c, levelDb, 1.0);
        };

        check (near (settled (DynamicsMode::Duck, -10.0f), -12.0, 0.2),
               "a loud signal DUCKS by the range");
        check (near (settled (DynamicsMode::Duck, -50.0f), 0.0, 0.05),
               "and a quiet one passes");
        check (near (settled (DynamicsMode::Gate, -10.0f), 0.0, 0.05),
               "the gate does the opposite with the same numbers: loud passes");
        check (near (settled (DynamicsMode::Gate, -50.0f), -12.0, 0.2),
               "and quiet is attenuated");
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

    // -----------------------------------------------------------------------
    // The DWELL HISTOGRAM. This is what the transfer curve's glow is drawn
    // from, and every failure below is a picture that lies rather than a
    // picture that is ugly — which is why it is measured here and not left to
    // the editor, where "the glow looks about right" is the only available test.
    // -----------------------------------------------------------------------
    std::printf ("== dwell histogram: the axis ==\n");
    {
        check (dyn::dwellBinFor (dyn::kSilenceDb) < 0,
               "digital silence belongs in NO bin");
        check (dyn::dwellBinFor (dyn::kDwellFloorDb - 0.01f) < 0,
               "and so does anything below the floor");
        check (dyn::dwellBinFor (dyn::kDwellFloorDb) == 0, "the floor is bin 0");
        check (dyn::dwellBinFor (dyn::kDwellTopDb) == dyn::kDwellBins - 1,
               "the top is the last bin");
        check (dyn::dwellBinFor (dyn::kDwellTopDb + 40.0f) == dyn::kDwellBins - 1,
               "and over the top CLAMPS rather than dropping - a limiter's input "
               "lives above 0 dBFS and belongs at the top of the picture");

        // Round-tripping matters because the view interpolates its own dB axis
        // out of these centres; a half-bin offset here is a glow that sits a
        // fraction of a dB off the level it claims to be showing.
        for (int b = 0; b < dyn::kDwellBins; ++b)
            if (dyn::dwellBinFor (dyn::dwellBinCentreDb (b)) != b)
            {
                check (false, "bin centre " + std::to_string (b) + " round-trips");
                break;
            }
        check (true, "every bin centre maps back to its own bin");
    }

    std::printf ("== dwell histogram: it lands where the signal is ==\n");
    {
        DynamicsCore c;
        c.prepare (kSr);
        c.setMode (DynamicsMode::Compress);
        c.setThresholdDb (-20.0f);

        check (dwellTotal (c) == 0.0f, "a prepared core has published nothing yet");

        // Two seconds of a steady -20 dBFS, which is 375 whole flush intervals:
        // no partial publish to reason about.
        feedLevel (c, dyn::dbToGain (-20.0f), 2.0);

        const float hot = hottestDwellDb (c);
        check (near (hot, -20.0, 1.5),
               "a steady -20 dBFS signal is hottest at -20 dB (got "
               + std::to_string (hot) + ")");
        check (dwellTotal (c) > 0.0f, "and there is energy in the histogram");
    }

    std::printf ("== dwell histogram: silence does not light the floor ==\n");
    {
        // The bug this catches has a shape: clamping an out-of-range level into
        // bin 0 instead of dropping it puts a permanent bright spot at the
        // bottom of every plot in the rack, on a signal that is not there.
        DynamicsCore c;
        c.prepare (kSr);

        feedLevel (c, 0.0f, 1.0);
        check (dwellTotal (c) == 0.0f, "a second of digital silence accumulates nothing");
    }

    std::printf ("== dwell histogram: old energy fades ==\n");
    {
        DynamicsCore c;
        c.prepare (kSr);

        feedLevel (c, dyn::dbToGain (-12.0f), 2.0);
        const float loud = dwellTotal (c);
        check (loud > 0.0f, "energy accumulated while the signal played");

        // Three seconds is six time constants. Without the decay this total
        // would simply stay where it was and the glow would be a permanent
        // record of everything the plugin had ever heard.
        feedLevel (c, 0.0f, 3.0);
        const float quiet = dwellTotal (c);
        check (quiet < loud * 0.02f,
               "and three seconds of silence faded it to under 2% of that");

        c.reset();
        check (dwellTotal (c) == 0.0f, "reset() publishes an empty histogram");
    }

    std::printf ("== dwell histogram: two levels, two bright bands ==\n");
    {
        // The whole point of a histogram rather than a level: a signal that
        // spends its time at two levels has to READ as two, not as the average
        // of them, which is all a single float could ever say.
        DynamicsCore c;
        c.prepare (kSr);

        // Alternating 20 ms bursts, long enough for the detector to settle on
        // each. The decay is deliberately outrun by keeping both alive.
        for (int rep = 0; rep < 25; ++rep)
        {
            feedLevel (c, dyn::dbToGain (-30.0f), 0.02);
            feedLevel (c, dyn::dbToGain (-6.0f),  0.02);
        }

        float bins[dyn::kDwellBins];
        check (readDwell (c, bins), "the histogram reads");

        auto binNear = [&bins] (float db)
        {
            const int b = dyn::dwellBinFor (db);
            float best = 0.0f;
            for (int i = b - 2; i <= b + 2; ++i)
                if (i >= 0 && i < dyn::kDwellBins) best = std::max (best, bins[i]);
            return best;
        };

        // The midpoint is where a single averaged level would have put
        // everything, and it is where nothing should be.
        check (binNear (-6.0f)  > 0.0f, "the loud level is lit");
        check (binNear (-30.0f) > 0.0f, "the quiet level is lit");
        check (binNear (-18.0f) < 0.25f * std::min (binNear (-6.0f), binNear (-30.0f)),
               "and the average of the two is NOT - a histogram says both, a "
               "single level could only ever say neither");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
