// Standalone test for StereoEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source stereo_engine_test.cpp -o stereotest && ./stereotest
//
// The headline property is the last section: whatever the knobs are set to, the
// mono sum of the output equals the mono sum of the input. That is not a
// tolerance the DSP happens to hit — it is structural (every stage rewrites the
// SIDE only), and this test is what stops a later edit from quietly breaking it.

#include "EedStereoEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (float a, float b, float tol) { return std::fabs (a - b) <= tol; }

static constexpr double kSR = 48000.0;

// A deterministic pseudo-random source: no <random> spread across libstdc++/
// libc++ versions, so the numbers this test reports are the same everywhere.
struct Noise
{
    unsigned s = 22222u;
    float next()
    {
        s = s * 1664525u + 1013904223u;
        return (float) ((double) (s >> 8) / 8388608.0 - 1.0);   // -1 .. +1
    }
};

// Run a block-based pass and hand back both channels. Long enough by default
// that the 20 ms smoothers have fully settled before the caller looks.
static void run (StereoEngine& e,
                 std::vector<float>& l, std::vector<float>& r,
                 int blockSize = 256)
{
    const int n = (int) l.size();
    for (int i = 0; i < n; i += blockSize)
    {
        const int todo = std::min (blockSize, n - i);
        e.process (l.data() + i, r.data() + i, todo);
    }
}

// A steady sine in both channels, with an optional per-channel scale, so a
// "settled" measurement can be taken from the tail.
static void fillTone (std::vector<float>& l, std::vector<float>& r,
                      float hz, float ampL, float ampR)
{
    for (std::size_t i = 0; i < l.size(); ++i)
    {
        const float ph = 6.28318530718f * hz * (float) i / (float) kSR;
        const float v = std::sin (ph);
        l[i] = v * ampL;
        r[i] = v * ampR;
    }
}

// Peak absolute value over the last quarter of a buffer: past every smoother
// ramp and past the delay line's fill.
static float tailPeak (const std::vector<float>& v)
{
    float p = 0.0f;
    for (std::size_t i = v.size() * 3 / 4; i < v.size(); ++i)
        p = std::fmax (p, std::fabs (v[i]));
    return p;
}

int main()
{
    // -----------------------------------------------------------------------
    std::printf ("== M/S round trip is EXACTLY unity ==\n");
    {
        bool ok = true;
        Noise n;
        for (int i = 0; i < 1000; ++i)
        {
            const float l = n.next(), r = n.next();
            float m, s, lo, ro;
            StereoEngine::encode (l, r, m, s);
            StereoEngine::decode (m, s, lo, ro);
            if (! near (lo, l, 1e-6f) || ! near (ro, r, 1e-6f)) ok = false;
        }
        check (ok, "encode -> decode returns the input over 1000 random pairs");
    }

    // -----------------------------------------------------------------------
    std::printf ("== rotation convention: + degrees moves the image RIGHT ==\n");
    {
        float m = 1.0f, s = 0.0f;                   // a centred source
        StereoEngine::rotate (45.0f, m, s);
        float l, r;
        StereoEngine::decode (m, s, l, r);
        check (near (l, 0.0f, 1e-5f),     "+45 empties the LEFT channel");
        check (near (r, 1.41421f, 1e-4f), "+45 puts a centred source hard RIGHT");

        m = 1.0f; s = 0.0f;
        StereoEngine::rotate (-45.0f, m, s);
        StereoEngine::decode (m, s, l, r);
        check (near (r, 0.0f, 1e-5f),     "-45 empties the RIGHT channel");
        check (near (l, 1.41421f, 1e-4f), "-45 puts a centred source hard LEFT");
    }

    std::printf ("== rotation preserves M^2 + S^2 (it tilts, it does not squash) ==\n");
    {
        bool ok = true;
        Noise n;
        for (int i = 0; i < 200; ++i)
        {
            float m = n.next(), s = n.next();
            const float before = m * m + s * s;
            StereoEngine::rotate ((float) (i % 91) - 45.0f, m, s);
            if (! near (m * m + s * s, before, 1e-4f)) ok = false;
        }
        check (ok, "power invariant across the whole -45..+45 sweep");
    }

    // -----------------------------------------------------------------------
    std::printf ("== defaults are an EXACT bypass (a default insert is inaudible) ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);

        Noise n;
        std::vector<float> l (4096), r (4096), l0, r0;
        for (std::size_t i = 0; i < l.size(); ++i) { l[i] = n.next(); r[i] = n.next(); }
        l0 = l; r0 = r;

        run (e, l, r);

        float worst = 0.0f;
        for (std::size_t i = 0; i < l.size(); ++i)
        {
            worst = std::fmax (worst, std::fabs (l[i] - l0[i]));
            worst = std::fmax (worst, std::fabs (r[i] - r0[i]));
        }
        check (worst <= 1e-6f, "width 100 / haas 0 / mono 0 / mix 100 / trim 0 is "
                               "bit-transparent (worst dev " + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== width 0 collapses to mono; width 200 doubles the side ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setWidthPercent (0.0f);
        e.reset();

        std::vector<float> l (4096), r (4096);
        fillTone (l, r, 500.0f, 1.0f, -1.0f);        // pure side: L = -R
        run (e, l, r);

        bool sameLR = true;
        for (std::size_t i = l.size() / 2; i < l.size(); ++i)
            if (! near (l[i], r[i], 1e-5f)) sameLR = false;
        check (sameLR, "width 0: L == R everywhere");
        check (tailPeak (l) <= 1e-5f, "width 0 on a pure side signal is silence "
                                      "(peak " + std::to_string (tailPeak (l)) + ")");
    }
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setWidthPercent (200.0f);
        e.reset();

        std::vector<float> l (4096), r (4096);
        fillTone (l, r, 500.0f, 1.0f, -1.0f);        // side amplitude 1.0
        run (e, l, r);
        check (near (tailPeak (l), 2.0f, 0.01f),
               "width 200 doubles a pure side signal (peak "
               + std::to_string (tailPeak (l)) + ")");
    }
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setWidthPercent (0.0f);
        e.reset();

        std::vector<float> l (4096), r (4096);
        fillTone (l, r, 500.0f, 1.0f, 1.0f);         // pure mid
        run (e, l, r);
        check (near (tailPeak (l), 1.0f, 0.01f),
               "width does NOT touch a centred source (peak "
               + std::to_string (tailPeak (l)) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== bass mono: mono below the corner, stereo above it ==\n");
    {
        // 40 Hz of pure side, with the mono-maker at 200 Hz: it should be gone.
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setBassMonoHz (200.0f);
        e.reset();

        std::vector<float> l (32768), r (32768);
        fillTone (l, r, 40.0f, 1.0f, -1.0f);
        run (e, l, r);

        // 12 dB/oct: two octaves and a bit below the corner should be ~-28 dB.
        const float lowSide = tailPeak (l);
        check (lowSide < 0.05f, "40 Hz side content is gone at a 200 Hz corner "
                                "(peak " + std::to_string (lowSide) + ")");

        bool sameLR = true;
        for (std::size_t i = l.size() * 3 / 4; i < l.size(); ++i)
            if (! near (l[i], r[i], 0.1f)) sameLR = false;
        check (sameLR, "below the corner the two channels have converged (mono)");
    }
    {
        // 5 kHz of pure side, same corner: it must survive untouched.
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setBassMonoHz (200.0f);
        e.reset();

        std::vector<float> l (8192), r (8192);
        fillTone (l, r, 5000.0f, 1.0f, -1.0f);
        run (e, l, r);
        // Not exactly 1.0: a subtractive one-pole highpass settles at about
        // (1 - a/2) well above its corner, so two stages cost ~0.25 dB of side
        // level up here. That is inaudible, and it is the price of a filter that
        // is EXACT when switched off, which matters far more.
        check (near (tailPeak (l), 1.0f, 0.04f),
               "5 kHz side content passes at full level (peak "
               + std::to_string (tailPeak (l)) + ")");
    }
    {
        // 0 Hz means OFF, not "a filter at 0 Hz".
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setBassMonoHz (0.0f);
        e.reset();

        std::vector<float> l (4096), r (4096);
        fillTone (l, r, 30.0f, 1.0f, -1.0f);
        run (e, l, r);
        check (near (tailPeak (l), 1.0f, 0.01f), "0 Hz leaves 30 Hz side content alone");
    }

    // -----------------------------------------------------------------------
    std::printf ("== haas: 0 ms is an exact bypass, and it widens a MONO source ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setHaasMs (0.0f);
        e.reset();

        Noise n;
        std::vector<float> l (4096), r (4096), l0, r0;
        for (std::size_t i = 0; i < l.size(); ++i) { l[i] = n.next(); r[i] = n.next(); }
        l0 = l; r0 = r;
        run (e, l, r);

        float worst = 0.0f;
        for (std::size_t i = 0; i < l.size(); ++i)
            worst = std::fmax (worst, std::fabs (l[i] - l0[i]));
        check (worst <= 1e-6f, "0 ms changes nothing at all (worst dev "
                               + std::to_string (worst) + ")");
    }
    {
        // The reason the Haas stage injects delayed MID rather than only delaying
        // the side: a mono source has no side to delay, and a stereoizer that
        // does nothing to mono material is not a stereoizer.
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setHaasMs (15.0f);
        e.reset();

        std::vector<float> l (16384), r (16384);
        fillTone (l, r, 300.0f, 1.0f, 1.0f);         // identical channels: mono in
        run (e, l, r);

        float maxDiff = 0.0f;
        for (std::size_t i = l.size() * 3 / 4; i < l.size(); ++i)
            maxDiff = std::fmax (maxDiff, std::fabs (l[i] - r[i]));
        check (maxDiff > 0.5f, "a MONO input comes out with real side content "
                               "(L-R peak " + std::to_string (maxDiff) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== trim is the only stage that changes level ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setTrimDb (-6.0f);
        e.reset();

        std::vector<float> l (8192), r (8192);
        fillTone (l, r, 500.0f, 1.0f, 1.0f);
        run (e, l, r);
        check (near (tailPeak (l), 0.50119f, 0.005f),
               "-6 dB trim ~ 0.5x (peak " + std::to_string (tailPeak (l)) + ")");
    }
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setTrimDb (0.0f);
        e.reset();
        std::vector<float> l (8192), r (8192);
        fillTone (l, r, 500.0f, 1.0f, 1.0f);
        run (e, l, r);
        check (near (tailPeak (l), 1.0f, 1e-3f), "0 dB trim is unity");
    }

    // -----------------------------------------------------------------------
    std::printf ("== mix 0 is the dry signal, whatever the other knobs say ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setWidthPercent (200.0f);
        e.setHaasMs (20.0f);
        e.setBassMonoHz (300.0f);
        e.setMixPercent (0.0f);
        e.reset();

        Noise n;
        std::vector<float> l (4096), r (4096), l0, r0;
        for (std::size_t i = 0; i < l.size(); ++i) { l[i] = n.next(); r[i] = n.next(); }
        l0 = l; r0 = r;
        run (e, l, r);

        float worst = 0.0f;
        for (std::size_t i = 0; i < l.size(); ++i)
            worst = std::fmax (worst, std::fabs (l[i] - l0[i]));
        check (worst <= 1e-6f, "mix 0 hands back the input untouched (worst dev "
                               + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    // The headline invariant. Every stage except rotation and trim rewrites the
    // SIDE only, and L + R = 2M for any side at all — so the fold-down a
    // broadcaster hears is the fold-down they would have heard without the
    // device in the chain. No comb filtering, at any setting.
    std::printf ("== MONO SUM IS PRESERVED across randomised parameter sets ==\n");
    {
        struct Setting { float width, haas, bassMono, mix; };
        const Setting settings[] = {
            { 100.0f,  0.0f,   0.0f, 100.0f },
            {   0.0f, 15.0f, 120.0f, 100.0f },
            { 200.0f, 40.0f, 500.0f, 100.0f },
            { 130.0f,  7.5f, 200.0f,  50.0f },
            { 175.0f, 23.0f,  80.0f,  35.0f },
            {  60.0f,  3.0f, 300.0f,  90.0f },
        };

        float worstOverall = 0.0f;
        bool allOk = true;

        for (const auto& st : settings)
        {
            StereoEngine e;
            e.prepare (kSR, 512);
            e.setWidthPercent (st.width);
            e.setHaasMs       (st.haas);
            e.setBassMonoHz   (st.bassMono);
            e.setMixPercent   (st.mix);
            e.reset();

            Noise n;
            std::vector<float> l (8192), r (8192), sum0 (8192);
            for (std::size_t i = 0; i < l.size(); ++i)
            {
                l[i] = n.next();
                r[i] = n.next();
                sum0[i] = l[i] + r[i];
            }

            run (e, l, r);

            float worst = 0.0f;
            for (std::size_t i = 0; i < l.size(); ++i)
                worst = std::fmax (worst, std::fabs ((l[i] + r[i]) - sum0[i]));

            worstOverall = std::fmax (worstOverall, worst);
            if (worst > 1e-5f) allOk = false;
        }

        check (allOk, "L+R identical to the input's L+R at every setting "
                      "(worst dev " + std::to_string (worstOverall) + ")");
    }

    std::printf ("== rotation is the ONE stage that is not mono-safe (documented) ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setRotationDegrees (30.0f);
        e.reset();

        std::vector<float> l (8192), r (8192), sum0 (8192);
        fillTone (l, r, 500.0f, 1.0f, 0.2f);
        for (std::size_t i = 0; i < l.size(); ++i) sum0[i] = l[i] + r[i];
        run (e, l, r);

        float worst = 0.0f;
        for (std::size_t i = l.size() / 2; i < l.size(); ++i)
            worst = std::fmax (worst, std::fabs ((l[i] + r[i]) - sum0[i]));
        check (worst > 0.01f, "rotation DOES change the mono sum, as its maths "
                              "requires (dev " + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== clamping matches the advertised ranges exactly ==\n");
    {
        StereoEngine e;
        e.setWidthPercent (999.0f);     check (e.getWidthPercent() == StereoEngine::kMaxWidthPct,    "width clamps to 200");
        e.setWidthPercent (-5.0f);      check (e.getWidthPercent() == StereoEngine::kMinWidthPct,    "width clamps to 0");
        e.setRotationDegrees (180.0f);  check (e.getRotationDegrees() == StereoEngine::kMaxRotationDeg, "rotation clamps to +45");
        e.setRotationDegrees (-180.0f); check (e.getRotationDegrees() == StereoEngine::kMinRotationDeg, "rotation clamps to -45");
        e.setBassMonoHz (9000.0f);      check (e.getBassMonoHz() == StereoEngine::kMaxBassMonoHz,    "bass mono clamps to 500");
        e.setHaasMs (500.0f);           check (e.getHaasMs() == StereoEngine::kMaxHaasMs,            "haas clamps to 40 ms");
        e.setMixPercent (500.0f);       check (e.getMixPercent() == StereoEngine::kMaxMixPct,        "mix clamps to 100");
        e.setTrimDb (999.0f);           check (e.getTrimDb() == StereoEngine::kMaxTrimDb,            "trim clamps to +24");
    }

    // -----------------------------------------------------------------------
    std::printf ("== mono input: no invented second channel, trim still applies ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setWidthPercent (200.0f);
        e.setHaasMs (20.0f);
        e.setTrimDb (-6.0f);
        e.reset();

        std::vector<float> l (8192);
        for (std::size_t i = 0; i < l.size(); ++i)
            l[i] = std::sin (6.28318530718f * 500.0f * (float) i / (float) kSR);

        for (std::size_t i = 0; i < l.size(); i += 256)
            e.process (l.data() + i, nullptr, 256);

        check (near (tailPeak (l), 0.50119f, 0.005f),
               "a mono buffer gets the trim and nothing else (peak "
               + std::to_string (tailPeak (l)) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== no NaN or inf escapes, at any setting ==\n");
    {
        StereoEngine e;
        e.prepare (kSR, 512);
        e.setWidthPercent (200.0f);
        e.setHaasMs (40.0f);
        e.setBassMonoHz (500.0f);
        e.setRotationDegrees (45.0f);
        e.setTrimDb (24.0f);
        e.setMixPercent (73.0f);
        e.reset();

        Noise n;
        std::vector<float> l (16384), r (16384);
        for (std::size_t i = 0; i < l.size(); ++i) { l[i] = n.next(); r[i] = n.next(); }
        run (e, l, r);

        bool finite = true;
        for (std::size_t i = 0; i < l.size(); ++i)
            if (! std::isfinite (l[i]) || ! std::isfinite (r[i])) finite = false;
        check (finite, "every output sample is finite");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
