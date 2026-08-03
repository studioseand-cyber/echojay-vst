// Standalone test for the Harmonic cluster's display-side analysis (no JUCE).
//   g++ -std=c++17 -O2 -I../Source harmonic_analysis_test.cpp ../Source/EedHarmonicAnalysis.cpp -o atest && ./atest
//
// Pitch detection is the kind of code that is confidently wrong by an octave and
// looks fine until someone plays a real note, so every claim here is checked
// against a synthesised signal whose answer is known exactly — including the
// cases that break naive detectors: a missing fundamental, a strong 2nd
// harmonic, DC offset, noise, and silence.

#include "EedHarmonicAnalysis.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay::harmonic;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kSr = 48000.0;

// A tone with an arbitrary harmonic recipe. `amps[k]` is the amplitude of
// harmonic k+1.
static std::vector<float> makeTone (double f0, const std::vector<double>& amps,
                                    int n = 4096, double dc = 0.0, double phase = 0.3)
{
    std::vector<float> x ((std::size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        double s = dc;
        for (std::size_t h = 0; h < amps.size(); ++h)
            s += amps[h] * std::sin (2.0 * kPi * f0 * (double) (h + 1) * (double) i / kSr
                                     + phase * (double) h);
        x[(std::size_t) i] = (float) s;
    }
    return x;
}

// Deterministic pseudo-noise: no <random> so the test cannot drift between
// standard library versions.
static std::vector<float> makeNoise (int n, double amp)
{
    std::vector<float> x ((std::size_t) n);
    unsigned s = 22222u;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1664525u + 1013904223u;
        x[(std::size_t) i] = (float) (amp * (2.0 * ((double) (s >> 8) / 16777216.0) - 1.0));
    }
    return x;
}

static bool within (double a, double b, double tolPercent)
{
    return std::fabs (a - b) <= std::fabs (b) * tolPercent * 0.01;
}

int main()
{
    // -----------------------------------------------------------------------
    std::printf ("== whole-cycle trimming (what makes an unwindowed Goertzel exact) ==\n");
    {
        // 200 Hz at 48 kHz is 240 samples; 4096/240 = 17 whole cycles = 4080.
        check (wholeCycleLength (4096, kSr, 200.0) == 4080, "200 Hz -> 4080 (17 cycles)");
        // 1 kHz is 48 samples; 4096/48 = 85 whole cycles = 4080.
        check (wholeCycleLength (4096, kSr, 1000.0) == 4080, "1 kHz -> 4080 (85 cycles)");
        check (wholeCycleLength (4096, kSr, 5.0) == 0, "a period longer than the frame -> 0");
        check (wholeCycleLength (0, kSr, 200.0) == 0, "no frame -> 0");

        // The point of it: measured over a whole number of cycles the harmonic
        // lands on a bin centre and the fundamental does not leak onto it.
        const auto tone = makeTone (200.0, { 1.0 });        // a PURE sine, no 2nd
        const int trimmed = wholeCycleLength (4096, kSr, 200.0);

        const double h2Trimmed = goertzelMagnitude (tone.data(), trimmed, kSr, 400.0);
        const double h2Ragged  = goertzelMagnitude (tone.data(), 4096,    kSr, 400.0);

        const double dbTrimmed = 20.0 * std::log10 (h2Trimmed / 1.0);
        const double dbRagged  = 20.0 * std::log10 (h2Ragged  / 1.0);
        std::printf ("    2nd harmonic of a PURE sine: trimmed %.1f dB, untrimmed %.1f dB\n",
                     dbTrimmed, dbRagged);
        check (dbTrimmed < -80.0, "trimmed: the absent 2nd reads as absent");
        check (dbTrimmed < dbRagged - 20.0,
               "and is " + std::to_string (dbRagged - dbTrimmed)
               + " dB below what an untrimmed frame invents");
    }

    // -----------------------------------------------------------------------
    std::printf ("== Goertzel magnitude is the component's amplitude ==\n");
    {
        const auto tone = makeTone (250.0, { 0.5 });
        const int n = wholeCycleLength (4096, kSr, 250.0);
        const double m = goertzelMagnitude (tone.data(), n, kSr, 250.0);
        check (within (m, 0.5, 2.0), "amplitude 0.5 reads as " + std::to_string (m));

        const double none = goertzelMagnitude (tone.data(), n, kSr, 3000.0);
        check (none < 1.0e-3, "a frequency that is not there reads as ~0");
        check (goertzelMagnitude (nullptr, n, kSr, 250.0) == 0.0, "null buffer -> 0");
        check (goertzelMagnitude (tone.data(), n, kSr, 40000.0) == 0.0, "above Nyquist -> 0");
    }

    // -----------------------------------------------------------------------
    std::printf ("== fundamental estimation, across the range ==\n");
    {
        for (double f0 : { 55.0, 110.0, 220.0, 440.0, 880.0 })
        {
            const auto tone = makeTone (f0, { 1.0, 0.4, 0.25, 0.1 });
            const auto e = estimateFundamental (tone.data(), (int) tone.size(), kSr);
            check (e.locked && within (e.hz, f0, 1.0),
                   std::to_string ((int) f0) + " Hz -> " + std::to_string (e.hz)
                   + " Hz (clarity " + std::to_string (e.clarity) + ")");
        }
    }

    std::printf ("== accuracy is good enough for the EIGHTH bin, not just the first ==\n");
    {
        // f0 error is multiplied by 8 at the top bar, so 1% is not good enough:
        // at 200 Hz an 8th harmonic sits at 1600 Hz and the bins are ~12 Hz wide.
        const auto tone = makeTone (200.0, { 1.0, 0.3, 0.2 });
        const auto e = estimateFundamental (tone.data(), (int) tone.size(), kSr);
        const double errorAtEighth = std::fabs ((double) e.hz - 200.0) * 8.0;
        std::printf ("    f0 %.3f Hz -> 8th harmonic misplaced by %.2f Hz\n", e.hz, errorAtEighth);
        check (errorAtEighth < 6.0, "8th harmonic lands within half a bin");
    }

    std::printf ("== the octave trap: a 2nd harmonic LOUDER than the fundamental ==\n");
    {
        // A naive "pick the biggest autocorrelation peak" reads this an octave
        // high or low depending on the frame. It is not a corner case: it is what
        // a clarinet, an overdriven guitar and a saturated bass all look like.
        const auto tone = makeTone (150.0, { 0.4, 1.0, 0.5, 0.3 });
        const auto e = estimateFundamental (tone.data(), (int) tone.size(), kSr);
        check (e.locked && within (e.hz, 150.0, 2.0),
               "reads 150 Hz, not 300 (got " + std::to_string (e.hz) + ")");
    }

    std::printf ("== the missing fundamental ==\n");
    {
        // No energy at f0 at all — only its harmonics. The ear hears 100 Hz and
        // so should the detector, because the harmonics ARE spaced 100 Hz apart.
        const auto tone = makeTone (100.0, { 0.0, 1.0, 0.8, 0.6, 0.4 });
        const auto e = estimateFundamental (tone.data(), (int) tone.size(), kSr);
        check (e.locked && within (e.hz, 100.0, 2.0),
               "reads 100 Hz from harmonics alone (got " + std::to_string (e.hz) + ")");
    }

    std::printf ("== a DC offset does not make everything look periodic ==\n");
    {
        const auto tone = makeTone (330.0, { 1.0, 0.3 }, 4096, 0.6);   // +0.6 DC
        const auto e = estimateFundamental (tone.data(), (int) tone.size(), kSr);
        check (e.locked && within (e.hz, 330.0, 2.0),
               "330 Hz survives a large DC offset (got " + std::to_string (e.hz) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== it declines to lock when there is nothing to lock to ==\n");
    {
        const std::vector<float> silence (4096, 0.0f);
        const auto s = estimateFundamental (silence.data(), 4096, kSr);
        check (! s.locked, "silence does not lock");
        check (s.hz == 0.0f, "and reports no frequency at all");

        const auto noise = makeNoise (4096, 0.3);
        const auto ne = estimateFundamental (noise.data(), 4096, kSr);
        std::printf ("    noise: locked=%d clarity=%.3f\n", (int) ne.locked, ne.clarity);
        check (! ne.locked, "broadband noise does not lock");

        const auto quiet = makeTone (220.0, { 1.0e-6 });
        const auto qe = estimateFundamental (quiet.data(), (int) quiet.size(), kSr);
        check (! qe.locked, "a tone below the noise floor does not lock");

        check (! estimateFundamental (nullptr, 4096, kSr).locked, "null buffer does not lock");
        check (! estimateFundamental (silence.data(), 16, kSr).locked, "a tiny frame does not lock");
    }

    // -----------------------------------------------------------------------
    std::printf ("== harmonic magnitudes: a known recipe reads back correctly ==\n");
    {
        // Fundamental 1.0, 2nd at 0.1 (-20 dB), 3rd at 0.01 (-40 dB).
        const auto tone = makeTone (200.0, { 1.0, 0.1, 0.01 });
        const int n = wholeCycleLength (4096, kSr, 200.0);

        float db[6] {};
        harmonicMagnitudesDb (tone.data(), tone.data(), n, kSr, 200.0, db, 6);

        std::printf ("    read back: %.1f %.1f %.1f %.1f %.1f %.1f dB\n",
                     db[0], db[1], db[2], db[3], db[4], db[5]);
        check (std::fabs (db[0] - 0.0f)   < 0.2f, "fundamental is the 0 dB reference");
        check (std::fabs (db[1] + 20.0f)  < 0.5f, "2nd reads -20 dB");
        check (std::fabs (db[2] + 40.0f)  < 0.5f, "3rd reads -40 dB");
        check (db[3] < -55.0f && db[4] < -55.0f, "absent harmonics sit at the floor");
    }

    std::printf ("== cross-referenced magnitudes (the Exciter's reading) ==\n");
    {
        // The Exciter analyses what it GENERATED, which has almost nothing at
        // the fundamental. Self-referenced that would divide by noise; referenced
        // to the INPUT's fundamental it reads as "how much was added".
        const auto input     = makeTone (200.0, { 1.0 });
        const auto generated = makeTone (200.0, { 0.0, 0.05, 0.02 });   // no fundamental
        const int n = wholeCycleLength (4096, kSr, 200.0);

        float db[6] {};
        harmonicMagnitudesDb (generated.data(), input.data(), n, kSr, 200.0, db, 6);

        std::printf ("    generated vs input f0: %.1f %.1f %.1f dB\n", db[0], db[1], db[2]);
        check (db[0] < -55.0f, "nothing generated AT the fundamental, and it shows");
        check (std::fabs (db[1] + 26.0f) < 1.0f, "2nd reads -26 dB against the input's f0");
        check (std::fabs (db[2] + 34.0f) < 1.0f, "3rd reads -34 dB");

        // Self-referenced, the same buffer gives NO reading at all: there is no
        // fundamental in it, so the divide-by-noise guard fires and every bar
        // drops to the floor. That is the safe outcome rather than a wall of
        // full-scale bars, but it is still no reading — which is exactly why the
        // two-buffer form exists.
        float selfDb[6] {};
        harmonicMagnitudesDb (generated.data(), generated.data(), n, kSr, 200.0, selfDb, 6);
        check (selfDb[1] <= -59.0f && selfDb[2] <= -59.0f,
               "self-referencing the generated signal reads nothing at all "
               "(2nd " + std::to_string (selfDb[1]) + " dB), where cross-referencing "
               "reads " + std::to_string (db[1]) + " dB");
    }

    std::printf ("== magnitudes degrade safely ==\n");
    {
        float db[6] {};
        const std::vector<float> silence (2048, 0.0f);

        harmonicMagnitudesDb (silence.data(), silence.data(), 2048, kSr, 200.0, db, 6);
        bool allFloor = true;
        for (float v : db) if (v > -59.0f) allFloor = false;
        check (allFloor, "silence reports the floor, not a shape");

        harmonicMagnitudesDb (nullptr, nullptr, 2048, kSr, 200.0, db, 6);
        check (db[0] <= -60.0f, "null buffers leave the output at the floor");

        // A fundamental high enough that the upper harmonics exceed Nyquist.
        const auto high = makeTone (9000.0, { 1.0 });
        harmonicMagnitudesDb (high.data(), high.data(), 4096, kSr, 9000.0, db, 6);
        check (db[2] <= -60.0f, "harmonics above Nyquist stay at the floor, not aliased");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
