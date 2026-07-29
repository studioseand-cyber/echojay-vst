// Standalone test for GainEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source gain_engine_test.cpp -o gaintest && ./gaintest

#include "EedGainEngine.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;
static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (float a, float b, float tol) { return std::fabs (a - b) <= tol; }

// Run enough samples that the 20 ms smoother has fully settled, then report the
// steady-state gain the engine applies to each channel for a DC input of 1.
static void settled (GainEngine& e, float& lOut, float& rOut, int blocks = 40)
{
    constexpr int kBlock = 512;
    std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i) { l[(size_t) i] = 1.0f; r[(size_t) i] = 1.0f; }
        e.process (l.data(), r.data(), kBlock);
    }
    lOut = l[(size_t) (kBlock - 1)];
    rOut = r[(size_t) (kBlock - 1)];
}

int main()
{
    std::printf ("== dB -> gain ==\n");
    check (near (GainEngine::dbToGain (0.0f),   1.0f,    1e-6f), "0 dB is unity");
    check (near (GainEngine::dbToGain (6.0f),   1.99526f,1e-4f), "+6 dB ~ 2.0x");
    check (near (GainEngine::dbToGain (-6.0f),  0.50119f,1e-4f), "-6 dB ~ 0.5x");
    check (GainEngine::dbToGain (-60.0f) == 0.0f,                "-60 dB floor is TRUE silence");
    check (GainEngine::dbToGain (-999.0f) == 0.0f,               "below floor is silence");

    std::printf ("== dB clamping matches the advertised range ==\n");
    check (GainEngine::clampDb (999.0f)  == GainEngine::kMaxDb, "clamps to +24");
    check (GainEngine::clampDb (-999.0f) == GainEngine::kMinDb, "clamps to -60");

    std::printf ("== pan law: centre is UNITY (a default insert is pass-through) ==\n");
    {
        float gl = 0.0f, gr = 0.0f;
        GainEngine::panGains (0.0f, gl, gr);
        check (near (gl, 1.0f, 1e-5f), "centre left gain == 1.0");
        check (near (gr, 1.0f, 1e-5f), "centre right gain == 1.0");
    }

    std::printf ("== pan law: constant power across the whole sweep ==\n");
    {
        bool allOk = true;
        float worst = 0.0f;
        for (int i = -10; i <= 10; ++i)
        {
            float gl = 0.0f, gr = 0.0f;
            GainEngine::panGains ((float) i / 10.0f, gl, gr);
            // Normalised by the centre-unity scaling: gl^2+gr^2 == 2 throughout.
            const float power = gl * gl + gr * gr;
            worst = std::fmax (worst, std::fabs (power - 2.0f));
            if (! near (power, 2.0f, 1e-4f)) allOk = false;
        }
        check (allOk, "gl^2+gr^2 constant over 21 positions (worst dev "
                      + std::to_string (worst) + ")");
    }

    std::printf ("== pan law: hard pans ==\n");
    {
        float gl = 0.0f, gr = 0.0f;
        GainEngine::panGains (-1.0f, gl, gr);
        check (near (gr, 0.0f, 1e-5f),    "hard left silences the right channel");
        check (near (gl, 1.41421f, 1e-4f),"hard left reaches +3 dB on the left");

        GainEngine::panGains (1.0f, gl, gr);
        check (near (gl, 0.0f, 1e-5f),    "hard right silences the left channel");
        check (near (gr, 1.41421f, 1e-4f),"hard right reaches +3 dB on the right");
    }
    {
        // Out-of-range pan must not wrap around into the opposite side.
        float gl = 0.0f, gr = 0.0f;
        GainEngine::panGains (-5.0f, gl, gr);
        check (near (gr, 0.0f, 1e-5f) && gl > 1.4f, "pan below -1 clamps, does not wrap");
    }

    std::printf ("== defaults are a true pass-through ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        float l = 0.0f, r = 0.0f;
        settled (e, l, r);
        check (near (l, 1.0f, 1e-4f) && near (r, 1.0f, 1e-4f),
               "0 dB + centre leaves DC 1.0 untouched");
    }

    std::printf ("== level reaches its target through the smoother ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setLevelDb (-6.0f);
        float l = 0.0f, r = 0.0f;
        settled (e, l, r);
        check (near (l, 0.50119f, 1e-3f), "settles at -6 dB on the left");
        check (near (r, 0.50119f, 1e-3f), "settles at -6 dB on the right");
    }

    std::printf ("== level + pan compose ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setLevelDb (-6.0f);
        e.setPan (1.0f);                       // hard right
        float l = 0.0f, r = 0.0f;
        settled (e, l, r);
        check (near (l, 0.0f, 1e-4f), "hard right + -6 dB: left is silent");
        check (near (r, 0.50119f * 1.41421f, 1e-3f), "hard right + -6 dB: right is -6 dB +3 dB");
    }

    std::printf ("== a level move RAMPS, it does not step (no click) ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.reset();
        e.setLevelDb (-60.0f);                 // full-scale drop to silence

        constexpr int kBlock = 64;
        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        for (int i = 0; i < kBlock; ++i) { l[(size_t) i] = 1.0f; r[(size_t) i] = 1.0f; }
        e.process (l.data(), r.data(), kBlock);

        // The very first sample must still be near the OLD value: a step would
        // land at 0 immediately and click.
        check (l[0] > 0.9f, "first sample stays near the previous gain");
        check (l[(size_t) (kBlock - 1)] < l[0], "gain is falling across the block");

        float worstJump = 0.0f;
        for (int i = 1; i < kBlock; ++i)
            worstJump = std::fmax (worstJump, std::fabs (l[(size_t) i] - l[(size_t) (i - 1)]));
        check (worstJump < 0.01f, "no sample-to-sample jump above 0.01 (worst "
                                  + std::to_string (worstJump) + ")");
    }

    std::printf ("== reset() snaps, so a restored session does not fade in ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setLevelDb (-6.0f);
        e.reset();
        constexpr int kBlock = 16;
        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        for (int i = 0; i < kBlock; ++i) { l[(size_t) i] = 1.0f; r[(size_t) i] = 1.0f; }
        e.process (l.data(), r.data(), kBlock);
        check (near (l[0], 0.50119f, 1e-3f), "first sample is already at target");
    }

    std::printf ("== mono: pan is ignored rather than silencing the channel ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setPan (1.0f);                       // hard right, but there is no right
        e.reset();
        constexpr int kBlock = 32;
        std::vector<float> l ((size_t) kBlock);
        for (int i = 0; i < kBlock; ++i) l[(size_t) i] = 1.0f;
        e.process (l.data(), nullptr, kBlock);
        check (near (l[0], 1.0f, 1e-4f), "mono at 0 dB passes through at unity");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
