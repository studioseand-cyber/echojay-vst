// Standalone numerical test for EqEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source eq_engine_test.cpp ../Source/EqEngine.cpp -o eqtest
// Validates that the analytic magnitude response (what the UI draws) matches the
// actual measured filter response, and checks the surgical-critical behaviours.

#include "EqEngine.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <random>
#include <string>

using namespace echojay;
static constexpr double kPi = 3.14159265358979323846;

static int g_fail = 0;
static void check (bool cond, const std::string& what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (! cond) ++g_fail;
}

// Measure steady-state gain (dB) of the engine at a single frequency by
// running a continuous sine through it and comparing output/input mean-square.
static double measureGainDb (EqEngine& eq, double freq, double fs)
{
    const int    block   = 256;
    const double dphase  = 2.0 * kPi * freq / fs;
    const int    warmup  = (int) (fs * 0.4);   // let smoothing + transient settle
    const int    measure = (int) (fs * 0.4);
    double phase = 0.0, sumIn = 0.0, sumOut = 0.0;
    std::vector<float> buf (block), in (block);
    int done = 0;
    const int total = warmup + measure;
    while (done < total)
    {
        const int n = std::min (block, total - done);
        for (int i = 0; i < n; ++i) { buf[i] = (float) std::sin (phase); in[i] = buf[i]; phase += dphase; }
        float* ch[1] = { buf.data() };
        eq.process (ch, 1, n);
        if (done >= warmup)
            for (int i = 0; i < n; ++i) { sumIn += (double) in[i] * in[i]; sumOut += (double) buf[i] * buf[i]; }
        done += n;
    }
    return 10.0 * std::log10 (sumOut / std::max (sumIn, 1e-20));
}

static double analyticDb (EqEngine& eq, double freq)
{
    float f = (float) freq, m = 0.0f;
    eq.getMagnitudeResponse (&f, &m, 1);
    return (double) m;
}

static void oneBand (EqEngine& eq, const BandSpec& b)
{
    BandSpec bands[EqEngine::kMaxBands];
    bands[0] = b;
    eq.setBands (bands, EqEngine::kMaxBands);
    eq.reset();
}

int main()
{
    const double fs = 48000.0;
    EqEngine eq;
    eq.prepare (fs, 256, 1);

    std::printf ("== analytic vs measured (should agree within ~0.25 dB) ==\n");
    struct Case { const char* name; BandSpec b; std::vector<double> freqs; };
    std::vector<Case> cases = {
        { "bell +6 @1k Q1",     { true, BandType::Bell,      1000, 6.0,  1.0f, 12 }, {250, 500, 1000, 2000, 4000} },
        { "bell -6 @1k Q2",     { true, BandType::Bell,      1000, -6.0, 2.0f, 12 }, {500, 1000, 2000} },
        { "lowshelf +6 @200",   { true, BandType::LowShelf,  200,  6.0,  0.707f,12}, {40, 200, 1000, 8000} },
        { "highshelf -6 @5k",   { true, BandType::HighShelf, 5000, -6.0, 0.707f,12}, {500, 5000, 15000} },
        { "highpass 100 24dB",  { true, BandType::HighPass,  100,  0.0,  0.707f,24}, {100, 400, 2000} },
        { "lowpass 8k 12dB",    { true, BandType::LowPass,   8000, 0.0,  0.707f,12}, {1000, 8000} },
    };
    for (auto& c : cases)
    {
        oneBand (eq, c.b);
        for (double f : c.freqs)
        {
            const double a = analyticDb (eq, f), m = measureGainDb (eq, f, fs);
            check (std::abs (a - m) < 0.25,
                   std::string (c.name) + " @ " + std::to_string ((int) f) + "Hz: analytic="
                   + std::to_string (a) + " measured=" + std::to_string (m));
        }
    }

    std::printf ("\n== bell centre gain equals requested gain ==\n");
    for (double g : { 3.0, 6.0, 12.0, -3.0, -6.0, -12.0 })
    {
        oneBand (eq, { true, BandType::Bell, 1000, (float) g, 3.0f, 12 });
        const double m = measureGainDb (eq, 1000, fs);
        check (std::abs (m - g) < 0.15, "bell centre " + std::to_string (g) + " dB → measured " + std::to_string (m));
    }

    std::printf ("\n== notch produces a deep null at centre ==\n");
    {
        oneBand (eq, { true, BandType::Notch, 1000, 0.0f, 8.0f, 12 });
        const double m = measureGainDb (eq, 1000, fs);
        check (m < -30.0, "notch @1k depth = " + std::to_string (m) + " dB (want < -30)");
        const double off = measureGainDb (eq, 250, fs);
        check (std::abs (off) < 1.0, "notch far from centre is ~flat: " + std::to_string (off) + " dB");
    }

    std::printf ("\n== HP/LP are -3 dB at cutoff and steepen with slope ==\n");
    {
        oneBand (eq, { true, BandType::HighPass, 500, 0.0f, 0.707f, 12 });
        check (std::abs (measureGainDb (eq, 500, fs) + 3.0) < 0.4, "HP12 -3dB @ cutoff");
        oneBand (eq, { true, BandType::LowPass, 500, 0.0f, 0.707f, 12 });
        check (std::abs (measureGainDb (eq, 500, fs) + 3.0) < 0.4, "LP12 -3dB @ cutoff");

        // one octave below a highpass cutoff: 24 dB/oct should attenuate ~2x the 12 dB/oct
        oneBand (eq, { true, BandType::HighPass, 1000, 0.0f, 0.707f, 12 });
        const double a12 = measureGainDb (eq, 500, fs);
        oneBand (eq, { true, BandType::HighPass, 1000, 0.0f, 0.707f, 24 });
        const double a24 = measureGainDb (eq, 500, fs);
        check (a24 < a12 - 8.0, "HP24 steeper than HP12 one octave down (" + std::to_string (a12)
               + " vs " + std::to_string (a24) + ")");
    }

    std::printf ("\n== shelves hit the right asymptotes ==\n");
    {
        oneBand (eq, { true, BandType::LowShelf, 300, 6.0f, 0.707f, 12 });
        check (std::abs (measureGainDb (eq, 30, fs) - 6.0) < 0.3, "lowshelf +6 → +6 at LF");
        check (std::abs (measureGainDb (eq, 12000, fs)) < 0.3,   "lowshelf +6 → 0 at HF");
        oneBand (eq, { true, BandType::HighShelf, 3000, -8.0f, 0.707f, 12 });
        check (std::abs (measureGainDb (eq, 18000, fs) + 8.0) < 0.4, "highshelf -8 → -8 at HF");
        check (std::abs (measureGainDb (eq, 60, fs)) < 0.3,          "highshelf -8 → 0 at LF");
    }

    std::printf ("\n== stability at extreme Q (no NaN/Inf, bounded) ==\n");
    {
        oneBand (eq, { true, BandType::Bell, 3000, 12.0f, 80.0f, 12 });
        std::mt19937 rng (1234);
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        std::vector<float> buf (256);
        bool ok = true; float peak = 0.0f;
        for (int blk = 0; blk < 4000; ++blk)
        {
            for (auto& x : buf) x = d (rng);
            float* ch[1] = { buf.data() };
            eq.process (ch, 1, 256);
            for (float x : buf) { if (! std::isfinite (x)) ok = false; peak = std::max (peak, std::fabs (x)); }
        }
        check (ok, "high-Q bell stays finite over 1M samples of noise");
        check (peak < 100.0f, "high-Q bell output bounded (peak=" + std::to_string (peak) + ")");
    }

    std::printf ("\n== multi-band stacks (two moves compose) ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        bands[0] = { true, BandType::Bell, 1000, 6.0f, 1.0f, 12 };
        bands[1] = { true, BandType::Bell, 1000, 4.0f, 1.0f, 12 }; // same spot, should add
        eq.setBands (bands, EqEngine::kMaxBands); eq.reset();
        const double m = measureGainDb (eq, 1000, fs);
        check (std::abs (m - 10.0) < 0.3, "two +6/+4 bells at 1k sum to ~+10 dB (" + std::to_string (m) + ")");
    }

    std::printf ("\n== bypass and solo ==\n");
    {
        BandSpec bands[EqEngine::kMaxBands];
        bands[0] = { true, BandType::Bell, 1000, 12.0f, 1.0f, 12 };
        bands[1] = { true, BandType::Bell, 4000, 12.0f, 1.0f, 12 };
        eq.setBands (bands, EqEngine::kMaxBands); eq.reset();
        eq.setBypassed (true);
        check (std::abs (measureGainDb (eq, 1000, fs)) < 0.01, "bypass → unity");
        eq.setBypassed (false);
        eq.setSoloBand (1); eq.reset();
        check (std::abs (measureGainDb (eq, 1000, fs)) < 1.0, "solo band 1 → band 0 (1k) inactive");
        check (measureGainDb (eq, 4000, fs) > 6.0, "solo band 1 → band 1 (4k) audible");
        eq.setSoloBand (-1);
    }

    std::printf ("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
