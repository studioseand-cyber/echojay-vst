/*
    eq_gain_test.cpp — the device-global output trim and auto-gain (Phase 1 of
    SURGICAL_EQ_ENHANCEMENTS.md).

    What is actually under test
    ---------------------------
    Auto-gain claims to cancel the LOUDNESS change a set of bands causes, so the
    assertions have to measure loudness, not the gain at one convenient
    frequency. Loudness here means "what pink noise sees": equal energy per
    octave. So the probe is a MULTITONE on a log-spaced grid — for a linear
    system, out/in power over such a grid is exactly the pink-weighted delta.

    The probe grid is deliberately NOT the grid the engine integrates over
    (1/6 octave, quarter-step offset, vs the engine's 1/12 octave). If the
    makeup only matched on its own sample points the test would prove nothing;
    matching on an independent grid is what makes it a real loudness match.

    JUCE-free, like the other EQ tests.

    Build/run:
      g++ -std=c++17 -O2 -I../Source eq_gain_test.cpp ../Source/EqEngine.cpp \
          -o gaintest && ./gaintest
*/

#include "EqEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using echojay::BandSpec;
using echojay::BandType;
using echojay::EqEngine;

static constexpr double kPi = 3.14159265358979323846;

static int g_fail = 0;
static void check (bool cond, const std::string& what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (! cond) ++g_fail;
}

// ---------------------------------------------------------------------------
// The pink-weighted probe: equal-amplitude tones, 1/6 octave apart, offset a
// quarter step off the engine's own grid so the two never sample the same
// points. Phases are spread deterministically to keep the crest factor sane —
// the engine is linear, so the level itself does not matter, but a 61-tone
// in-phase impulse is a needlessly nasty transient to warm up through.
struct Multitone
{
    std::vector<double> freqs, phase, dphase;

    Multitone (double fs)
    {
        const double fLo = 20.0 * std::pow (2.0, 1.0 / 24.0);   // quarter-step offset
        const double fHi = std::min (20000.0, fs * 0.45);
        for (double f = fLo; f <= fHi; f *= std::pow (2.0, 1.0 / 6.0))
            freqs.push_back (f);

        phase.resize (freqs.size());
        dphase.resize (freqs.size());
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            // Schroeder-ish quadratic phase spread: deterministic, low crest.
            phase[i]  = kPi * (double) (i * i) / (double) freqs.size();
            dphase[i] = 2.0 * kPi * freqs[i] / fs;
        }
    }

    void fill (float* dest, int n)
    {
        for (int s = 0; s < n; ++s)
        {
            double acc = 0.0;
            for (size_t i = 0; i < freqs.size(); ++i)
            {
                acc += std::sin (phase[i]);
                phase[i] += dphase[i];
            }
            dest[s] = (float) (acc * 0.02);
        }
    }
};

// Pink-weighted level change through the engine, in dB. Warms up first so the
// block-rate smoothing on both the bands and the output stage has settled.
static double measurePinkDeltaDb (EqEngine& eq, double fs)
{
    const int block   = 256;
    const int warmup  = (int) (fs * 0.5);
    const int measure = (int) (fs * 0.5);

    Multitone tone (fs);
    std::vector<float> buf ((size_t) block), in ((size_t) block);

    double sumIn = 0.0, sumOut = 0.0;
    int done = 0;
    const int total = warmup + measure;

    while (done < total)
    {
        const int n = std::min (block, total - done);
        tone.fill (buf.data(), n);
        for (int i = 0; i < n; ++i) in[(size_t) i] = buf[(size_t) i];

        float* ch[1] = { buf.data() };
        eq.process (ch, 1, n);

        if (done >= warmup)
            for (int i = 0; i < n; ++i)
            {
                sumIn  += (double) in[(size_t) i]  * in[(size_t) i];
                sumOut += (double) buf[(size_t) i] * buf[(size_t) i];
            }
        done += n;
    }
    return 10.0 * std::log10 (sumOut / std::max (sumIn, 1e-20));
}

static void oneBand (EqEngine& eq, const BandSpec& b)
{
    BandSpec bands[EqEngine::kMaxBands];
    bands[0] = b;
    eq.setBands (bands, EqEngine::kMaxBands);
    eq.reset();
}

static BandSpec bell (float hz, float gainDb, float q)
{
    BandSpec b;
    b.enabled = true; b.type = BandType::Bell;
    b.freqHz = hz; b.gainDb = gainDb; b.q = q;
    return b;
}

// ---------------------------------------------------------------------------
int main()
{
    const double fs = 48000.0;

    // -- output trim ---------------------------------------------------------
    std::printf ("\n== output trim is a clean, exact final gain ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        eq.setOutputDb (-6.0f);
        const double d = measurePinkDeltaDb (eq, fs);
        check (std::fabs (d - (-6.0)) < 0.1,
               "no bands, output_db -6 → -6.00 dB out (" + std::to_string (d) + ")");

        eq.setOutputDb (6.0f);
        const double u = measurePinkDeltaDb (eq, fs);
        check (std::fabs (u - 6.0) < 0.1,
               "output_db +6 → +6.00 dB out (" + std::to_string (u) + ")");
    }
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        eq.setOutputDb (100.0f);
        check (std::fabs (eq.getOutputDb() - 24.0f) < 1e-6f, "output_db clamps to +24");
        eq.setOutputDb (-100.0f);
        check (std::fabs (eq.getOutputDb() - (-24.0f)) < 1e-6f, "output_db clamps to -24");
    }
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        eq.setOutputDb (12.0f);
        eq.setBypassed (true);
        const double d = measurePinkDeltaDb (eq, fs);
        check (std::fabs (d) < 0.01, "bypass beats the output trim (" + std::to_string (d) + ")");
    }

    // -- auto-gain: the headline case from the spec ---------------------------
    std::printf ("\n== auto-gain cancels a +6 dB bell's loudness delta ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        oneBand (eq, bell (1000.0f, 6.0f, 1.0f));

        const double off = measurePinkDeltaDb (eq, fs);
        eq.setAutoGain (true);
        const double on = measurePinkDeltaDb (eq, fs);

        check (off > 0.3, "the +6 dB bell does raise level with auto-gain off ("
                          + std::to_string (off) + " dB)");
        check (std::fabs (on) < 1.0, "auto-gain on → within 1 dB of unity ("
                                     + std::to_string (on) + " dB)");
        check (std::fabs (on) < std::fabs (off), "auto-gain strictly improves the match");
    }

    // A wide bell moves far more energy than a surgical one, so this is where a
    // makeup that merely "sums the band gains" or does nothing at all would be
    // exposed: off is several dB, on must still land inside 1 dB.
    std::printf ("\n== auto-gain holds for a WIDE +6 dB bell (the hard case) ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        oneBand (eq, bell (1000.0f, 6.0f, 0.3f));

        const double off = measurePinkDeltaDb (eq, fs);
        eq.setAutoGain (true);
        const double on = measurePinkDeltaDb (eq, fs);

        check (off > 2.0, "wide +6 dB bell is a big level change uncompensated ("
                          + std::to_string (off) + " dB)");
        check (std::fabs (on) < 1.0, "auto-gain on → within 1 dB of unity ("
                                     + std::to_string (on) + " dB)");
    }

    std::printf ("\n== auto-gain works downward too (a cut gets made up) ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        oneBand (eq, bell (500.0f, -8.0f, 0.5f));

        const double off = measurePinkDeltaDb (eq, fs);
        eq.setAutoGain (true);
        const double on = measurePinkDeltaDb (eq, fs);

        check (off < -1.0, "wide -8 dB cut loses level uncompensated ("
                           + std::to_string (off) + " dB)");
        check (std::fabs (on) < 1.0, "auto-gain on → within 1 dB of unity ("
                                     + std::to_string (on) + " dB)");
    }

    // -- the two stack ---------------------------------------------------------
    std::printf ("\n== output_db sums linearly on top of the makeup ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        oneBand (eq, bell (1000.0f, 6.0f, 0.3f));
        eq.setAutoGain (true);

        const double base = measurePinkDeltaDb (eq, fs);
        eq.setOutputDb (-3.0f);
        const double trimmed = measurePinkDeltaDb (eq, fs);
        eq.setOutputDb (9.0f);
        const double lifted = measurePinkDeltaDb (eq, fs);

        check (std::fabs ((trimmed - base) - (-3.0)) < 0.1,
               "auto-gain + output_db -3 → exactly 3 dB below the makeup alone ("
               + std::to_string (trimmed - base) + ")");
        check (std::fabs ((lifted - base) - 9.0) < 0.1,
               "auto-gain + output_db +9 → exactly 9 dB above ("
               + std::to_string (lifted - base) + ")");
    }

    // -- what the UI reads off ------------------------------------------------
    std::printf ("\n== autoGainDbApplied() reports what was actually done ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        oneBand (eq, bell (1000.0f, 6.0f, 0.3f));

        const double off = measurePinkDeltaDb (eq, fs);
        check (std::fabs (eq.autoGainDbApplied()) < 1e-6f,
               "reports 0 while auto-gain is off");

        eq.setAutoGain (true);
        measurePinkDeltaDb (eq, fs);
        const double applied = eq.autoGainDbApplied();
        check (std::fabs (applied - (-off)) < 0.35,
               "reports the inverse of the measured delta (" + std::to_string (applied)
               + " vs " + std::to_string (-off) + ")");
        check (std::fabs (applied - eq.autoGainDbTarget()) < 0.01,
               "settles onto its target");
    }

    // A dynamic band's action is program-dependent and momentary; folding it
    // into a static makeup would make the output breathe with the detector.
    std::printf ("\n== dynamic bands are excluded from the makeup ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        BandSpec b = bell (1000.0f, 6.0f, 0.3f);
        b.dynamic = true; b.thresholdDb = -20.0f; b.rangeDb = -6.0f;
        oneBand (eq, b);
        check (std::fabs (eq.autoGainDbTarget()) < 1e-6f,
               "a dynamic bell contributes no static makeup");
    }

    std::printf ("\n== a band set with nothing enabled is exactly unity ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);
        eq.setAutoGain (true);
        const double d = measurePinkDeltaDb (eq, fs);
        check (std::fabs (d) < 0.01, "no bands → no makeup (" + std::to_string (d) + ")");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
