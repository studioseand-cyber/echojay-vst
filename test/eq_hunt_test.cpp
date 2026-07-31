// Standalone test for the resonance-hunt detector (P3 of
// SURGICAL_EQ_ENHANCEMENTS.md). No JUCE. Build:
//   g++ -std=c++17 -O2 -I../Source eq_hunt_test.cpp -o hunttest && ./hunttest
//
// The detector's one job: find narrow peaks standing above the LOCAL spectral
// envelope, and ignore everything that is merely loud or merely broadband. So
// the probes are built to punish the failure modes: a resonance inside noise
// (must find it), noise alone (must find nothing), broadband tilt (must not
// mistake the loud end for a resonance), two resonances (must find both, best
// first), and silence (must return zero, not garbage).

#include "EqResonanceHunt.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace echojay;
static constexpr double kPi = 3.14159265358979323846;

static int g_fail = 0;
static void check (bool cond, const std::string& what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (! cond) ++g_fail;
}

// White-ish noise at ampDb plus resonant sines. Deterministic seed.
static std::vector<float> makeSignal (int n, double fs, float noiseAmp,
                                      std::initializer_list<std::pair<double, float>> tones)
{
    std::mt19937 rng (0xC0FFEE);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> x ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        float v = noiseAmp * dist (rng);
        for (const auto& t : tones)
            v += t.second * (float) std::sin (2.0 * kPi * t.first * (double) i / fs);
        x[(size_t) i] = v;
    }
    return x;
}

int main()
{
    const double fs = 48000.0;
    const int    N  = 65536;                 // ~1.4 s at 48 k, the capture the EQ takes
    ResonancePeak peaks[8];

    // -----------------------------------------------------------------------
    std::printf ("== a resonance inside noise is found, at the right place ==\n");
    {
        // 3.7 kHz deliberately not a divisor of fs (see depth-pass test lore).
        auto x = makeSignal (N, fs, 0.05f, { { 3700.0, 0.25f } });

        ResonanceHuntParams p;                // medium defaults
        const int found = findResonances (x.data(), N, fs, p, peaks, 8);

        check (found >= 1, "found something (" + std::to_string (found) + ")");
        if (found >= 1)
        {
            check (std::fabs (peaks[0].freqHz - 3700.0f) < 3700.0f * 0.05f,
                   "strongest peak at ~3.7 kHz (got "
                   + std::to_string (peaks[0].freqHz) + ")");
            check (peaks[0].prominenceDb >= p.marginDb,
                   "prominence beats the margin ("
                   + std::to_string (peaks[0].prominenceDb) + " dB)");
            check (peaks[0].q >= 1.0f && peaks[0].q <= 36.0f,
                   "Q in the sane clamp (" + std::to_string (peaks[0].q) + ")");
            check (peaks[0].levelDb > peaks[0].envelopeDb,
                   "level sits above the envelope it was measured against");
        }
    }

    // -----------------------------------------------------------------------
    std::printf ("== broadband content alone yields NOTHING ==\n");
    {
        auto x = makeSignal (N, fs, 0.3f, {});
        ResonanceHuntParams p;
        const int found = findResonances (x.data(), N, fs, p, peaks, 8);
        check (found == 0, "plain noise: " + std::to_string (found) + " peaks");
    }

    // -----------------------------------------------------------------------
    std::printf ("== spectral TILT is not a resonance ==\n");
    {
        // Noise through a crude one-pole lowpass: lots of level difference
        // across the spectrum, but no narrow structure anywhere.
        std::mt19937 rng (42);
        std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
        std::vector<float> x ((size_t) N);
        float state = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            state += 0.2f * (dist (rng) - state);
            x[(size_t) i] = state * 3.0f;
        }
        ResonanceHuntParams p;
        const int found = findResonances (x.data(), N, fs, p, peaks, 8);
        check (found == 0, "tilted noise: " + std::to_string (found) + " peaks");
    }

    // -----------------------------------------------------------------------
    std::printf ("== two resonances: BOTH found, distinctly ==\n");
    {
        // Ranking is by PROMINENCE over the local envelope (what a notch would
        // act on), not by raw amplitude — so the order between these two is a
        // detector property, and the claim here is only that both exist.
        auto x = makeSignal (N, fs, 0.05f, { { 620.0, 0.15f }, { 4900.0, 0.3f } });
        ResonanceHuntParams p;
        const int found = findResonances (x.data(), N, fs, p, peaks, 8);

        check (found == 2, "exactly two (" + std::to_string (found) + ")");
        if (found == 2)
        {
            const bool a620  = std::fabs (peaks[0].freqHz - 620.0f)  < 620.0f  * 0.05f
                            || std::fabs (peaks[1].freqHz - 620.0f)  < 620.0f  * 0.05f;
            const bool a4900 = std::fabs (peaks[0].freqHz - 4900.0f) < 4900.0f * 0.05f
                            || std::fabs (peaks[1].freqHz - 4900.0f) < 4900.0f * 0.05f;
            check (a620,  "one lands at ~620 Hz");
            check (a4900, "one lands at ~4.9 kHz");
        }
    }

    // -----------------------------------------------------------------------
    std::printf ("== range_hz and max_bands are honoured ==\n");
    {
        auto x = makeSignal (N, fs, 0.05f, { { 620.0, 0.2f }, { 4900.0, 0.3f } });

        ResonanceHuntParams p;
        p.loHz = 2000.0f; p.hiHz = 8000.0f;
        int found = findResonances (x.data(), N, fs, p, peaks, 8);
        check (found == 1 && std::fabs (peaks[0].freqHz - 4900.0f) < 250.0f,
               "range [2k, 8k] excludes the 620 Hz peak");

        ResonanceHuntParams p2;
        p2.maxPeaks = 1;
        found = findResonances (x.data(), N, fs, p2, peaks, 8);
        check (found == 1, "max_bands 1 caps the answer");
    }

    // -----------------------------------------------------------------------
    std::printf ("== sensitivity: a subtle peak needs the HIGH setting ==\n");
    {
        auto x = makeSignal (N, fs, 0.15f, { { 2350.0, 0.06f } });

        ResonanceHuntParams low;   low.marginDb  = resonanceMarginForSensitivity (0);
        ResonanceHuntParams high;  high.marginDb = resonanceMarginForSensitivity (2);

        const int atLow  = findResonances (x.data(), N, fs, low,  peaks, 8);
        const int atHigh = findResonances (x.data(), N, fs, high, peaks, 8);

        check (atLow == 0,  "low sensitivity (6 dB margin) leaves it alone");
        check (atHigh >= 1 && std::fabs (peaks[0].freqHz - 2350.0f) < 2350.0f * 0.06f,
               "high sensitivity (2.5 dB margin) finds it ("
               + std::to_string (atHigh > 0 ? peaks[0].freqHz : 0.0f) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== degenerate inputs are answered, not crashed on ==\n");
    {
        ResonanceHuntParams p;
        std::vector<float> silence ((size_t) N, 0.0f);
        check (findResonances (silence.data(), N, fs, p, peaks, 8) == 0, "silence: 0");
        check (findResonances (nullptr, N, fs, p, peaks, 8) == 0,        "nullptr: 0");
        check (findResonances (silence.data(), 100, fs, p, peaks, 8) == 0,
               "too-short capture: 0");
    }

    std::printf (g_fail == 0 ? "\nALL PASS (0 failures)\n"
                             : "\n%d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
