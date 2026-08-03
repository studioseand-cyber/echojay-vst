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

    // =======================================================================
    // THE UTILITY DEPTH PASS (DEVICE_DEPTH_PLAN.md, Utility): mid/side mode,
    // mono sum, per-channel polarity.
    // =======================================================================

    // Settle with DISTINCT channel values, so mid and side are both non-zero
    // and the M/S -> L/R mapping is actually exercised.
    auto settledLR = [] (GainEngine& e, float inL, float inR, float& lOut, float& rOut)
    {
        constexpr int kBlock = 512;
        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < kBlock; ++i) { l[(size_t) i] = inL; r[(size_t) i] = inR; }
            e.process (l.data(), r.data(), kBlock);
        }
        lOut = l[(size_t) (kBlock - 1)];
        rOut = r[(size_t) (kBlock - 1)];
    };

    std::printf ("== mid/side gain maps to L/R exactly as the algebra says ==\n");
    {
        // in (0.8, 0.2): m = 0.5, s = 0.3. mid -6 dB, side +6 dB:
        //   L = 0.5*0.50119 + 0.3*1.99526 = 0.84917
        //   R = 0.5*0.50119 - 0.3*1.99526 = -0.34798
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setMode (GainMode::MidSide);
        e.setMidDb (-6.0f);
        e.setSideDb (6.0f);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.8f, 0.2f, l, r);
        check (near (l, 0.84917f, 1e-3f),  "L = gM*m + gS*s (got " + std::to_string (l) + ")");
        check (near (r, -0.34798f, 1e-3f), "R = gM*m - gS*s (got " + std::to_string (r) + ")");
    }
    {
        // A pure MID input (1, 1): side gain must not matter at all.
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setMode (GainMode::MidSide);
        e.setMidDb (-6.0f);
        e.setSideDb (6.0f);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 1.0f, 1.0f, l, r);
        check (near (l, 0.50119f, 1e-3f) && near (r, 0.50119f, 1e-3f),
               "mid -6 lands on a centred source; side +6 cannot touch it");
    }
    {
        // A pure SIDE input (1, -1): mid gain must not matter at all.
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setMode (GainMode::MidSide);
        e.setMidDb (-60.0f);
        e.setSideDb (-6.0f);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 1.0f, -1.0f, l, r);
        check (near (l, 0.50119f, 1e-3f) && near (r, -0.50119f, 1e-3f),
               "side -6 lands on a pure side source; mid -60 cannot touch it");
    }

    std::printf ("== side at the -60 floor IS mono; stereo mode ignores M/S ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setMode (GainMode::MidSide);
        e.setSideDb (-60.0f);                  // the floor is true silence
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.8f, 0.2f, l, r);
        check (near (l, 0.5f, 1e-3f) && near (r, 0.5f, 1e-3f),
               "side_db -60 collapses (0.8, 0.2) to its mid (0.5, 0.5)");
    }
    {
        // The mode is the gate: in stereo mode the M/S knobs are inert, which
        // is what keeps an old session identical when this pass ships.
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setMode (GainMode::Stereo);
        e.setMidDb (-60.0f);
        e.setSideDb (-60.0f);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.8f, 0.2f, l, r);
        check (near (l, 0.8f, 1e-4f) && near (r, 0.2f, 1e-4f),
               "stereo mode passes through with M/S dialled to the floor");
    }

    std::printf ("== mono sums to mono, click-free ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setMono (true);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.8f, 0.2f, l, r);
        check (near (l, 0.5f, 1e-4f) && near (r, 0.5f, 1e-4f),
               "mono makes both channels the mid (0.5, 0.5)");

        settledLR (e, 1.0f, -1.0f, l, r);
        check (near (l, 0.0f, 1e-4f) && near (r, 0.0f, 1e-4f),
               "a pure side signal sums to silence");
    }
    {
        // The collapse is a FADE of the side, not a switch: engage mono
        // mid-stream and the very first sample must still be near the stereo
        // value.
        GainEngine e;
        e.prepare (48000.0, 512);
        e.reset();
        e.setMono (true);
        constexpr int kBlock = 64;
        std::vector<float> l ((size_t) kBlock, 1.0f), r ((size_t) kBlock, -1.0f);
        e.process (l.data(), r.data(), kBlock);
        check (l[0] > 0.9f, "first sample is still (nearly) the stereo signal");
        check (std::fabs (l[(size_t) (kBlock - 1)]) < std::fabs (l[0]),
               "and the side is falling across the block");
    }

    std::printf ("== per-channel polarity flips, smoothly ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setPhaseLeft (true);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.8f, 0.2f, l, r);
        check (near (l, -0.8f, 1e-4f) && near (r, 0.2f, 1e-4f),
               "phase_left flips ONLY the left channel");
    }
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setPhaseRight (true);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.8f, 0.2f, l, r);
        check (near (l, 0.8f, 1e-4f) && near (r, -0.2f, 1e-4f),
               "phase_right flips ONLY the right channel");
    }
    {
        // Polarity runs BEFORE the mono sum, so flip-one-side-and-sum — the
        // classic phase check — actually cancels.
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setPhaseLeft (true);
        e.setMono (true);
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.7f, 0.7f, l, r);
        check (near (l, 0.0f, 1e-4f) && near (r, 0.0f, 1e-4f),
               "flip left + mono cancels identical channels to silence");
    }
    {
        // A flip lands through the smoother: no sample-to-sample jump larger
        // than the same bound the level ramp keeps.
        GainEngine e;
        e.prepare (48000.0, 512);
        e.reset();
        e.setPhaseLeft (true);
        constexpr int kBlock = 64;
        std::vector<float> l ((size_t) kBlock, 1.0f), r ((size_t) kBlock, 1.0f);
        e.process (l.data(), r.data(), kBlock);
        float worstJump = 0.0f;
        for (int i = 1; i < kBlock; ++i)
            worstJump = std::fmax (worstJump, std::fabs (l[(size_t) i] - l[(size_t) (i - 1)]));
        check (l[0] > 0.9f, "first sample is still (nearly) unflipped");
        check (worstJump < 0.01f, "the flip is a fade through zero (worst jump "
                                  + std::to_string (worstJump) + ")");
    }

    std::printf ("== the depth stage composes with level and pan ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setMode (GainMode::MidSide);
        e.setSideDb (-60.0f);                  // collapse to mid ...
        e.setLevelDb (-6.0f);                  // ... then trim ...
        e.setPan (1.0f);                       // ... then place hard right
        float l = 0.0f, r = 0.0f;
        settledLR (e, 0.8f, 0.2f, l, r);
        check (near (l, 0.0f, 1e-3f), "hard right still empties the left");
        check (near (r, 0.5f * 0.50119f * 1.41421f, 1e-3f),
               "mid 0.5 x -6 dB x +3 dB pan lands on the right (got "
               + std::to_string (r) + ")");
    }

    std::printf ("== depth clamps match the advertised ranges ==\n");
    {
        GainEngine e;
        e.setMidDb (999.0f);   check (e.getMidDb()  == GainEngine::kMaxMsDb, "mid_db clamps to +6");
        e.setSideDb (-999.0f); check (e.getSideDb() == GainEngine::kMinDb,   "side_db clamps to -60");
    }

    std::printf ("== mono buffer: polarity and level apply, the rest is inert ==\n");
    {
        GainEngine e;
        e.prepare (48000.0, 512);
        e.setPhaseLeft (true);
        e.setMode (GainMode::MidSide);
        e.setMidDb (-60.0f);                   // must be IGNORED without stereo
        e.setMono (true);
        e.reset();
        constexpr int kBlock = 32;
        std::vector<float> l ((size_t) kBlock, 1.0f);
        e.process (l.data(), nullptr, kBlock);
        check (near (l[0], -1.0f, 1e-4f),
               "one wire: the flip applies, the M/S stage does not");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
