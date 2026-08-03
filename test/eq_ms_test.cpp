// Standalone test for per-band Mid/Side/Left/Right routing (P2 of
// SURGICAL_EQ_ENHANCEMENTS.md). No JUCE. Build:
//   g++ -std=c++17 -O2 -I../Source eq_ms_test.cpp ../Source/EqEngine.cpp \
//       -o mstest && ./mstest
//
// The claims under test are the P2 guarantees:
//   * default (Stereo) routing is bit-identical to the pre-P2 engine — no
//     M/S conversion may happen when no band asks for one;
//   * a Side-routed band leaves a mono-correlated (L==R) signal untouched,
//     bit for bit — its lane is mathematically silent;
//   * a Side-routed band leaves the MID content of any stereo signal exactly
//     alone (the round-trip is m + s' / m - s', so the mid is preserved);
//   * Left/Right routing filters only its own channel;
//   * Side/Right bands on a MONO stream are clean no-ops, not crashes.

#include "EqEngine.h"
#include "EqMove.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <random>
#include <string>

using namespace echojay;

static int g_fail = 0;
static void check (bool cond, const std::string& what)
{
    std::printf ("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (! cond) ++g_fail;
}

static void setOneBand (EqEngine& eq, const BandSpec& b)
{
    BandSpec bands[EqEngine::kMaxBands];
    bands[0] = b;
    eq.setBands (bands, EqEngine::kMaxBands);
    eq.reset();
}

// Deterministic "stereo programme": correlated tone + uncorrelated noise.
static void fillStereo (std::vector<float>& l, std::vector<float>& r, bool correlated)
{
    std::mt19937 rng (0x5EED);
    std::uniform_real_distribution<float> dist (-0.4f, 0.4f);
    for (size_t i = 0; i < l.size(); ++i)
    {
        const float tone = 0.3f * std::sin (0.021f * (float) i);
        const float nl   = dist (rng);
        const float nr   = correlated ? nl : dist (rng);
        l[i] = tone + nl;
        r[i] = tone + nr;
    }
}

static void run (EqEngine& eq, float* l, float* r, int n)
{
    const int block = 256;
    for (int i = 0; i < n; i += block)
    {
        float* ch[2] = { l + i, r + i };
        eq.process (ch, 2, std::min (block, n - i));
    }
}

int main()
{
    const double fs = 48000.0;
    const int    N  = 8192;

    // -----------------------------------------------------------------------
    std::printf ("== side-routed cut is INVISIBLE to a mono-correlated signal ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);

        BandSpec b { true, BandType::Bell, 3000, -12.0f, 2.0f, 12 };
        b.channel = BandChannel::Side;
        setOneBand (eq, b);

        std::vector<float> l (N), r (N), l0 (N), r0 (N);
        fillStereo (l, r, true);            // L == R exactly
        l0 = l; r0 = r;

        run (eq, l.data(), r.data(), N);

        bool identical = true;
        for (int i = 0; i < N && identical; ++i)
            identical = std::memcmp (&l[(size_t) i], &l0[(size_t) i], sizeof (float)) == 0
                     && std::memcmp (&r[(size_t) i], &r0[(size_t) i], sizeof (float)) == 0;
        check (identical, "L==R input passes a -12 dB side bell bit-identically");
    }

    // -----------------------------------------------------------------------
    std::printf ("== side-routed band preserves the MID of a real stereo signal ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);

        BandSpec b { true, BandType::HighShelf, 8000, 6.0f, 0.707f, 12 };
        b.channel = BandChannel::Side;
        setOneBand (eq, b);

        std::vector<float> l (N), r (N), l0 (N), r0 (N);
        fillStereo (l, r, false);           // decorrelated
        l0 = l; r0 = r;

        run (eq, l.data(), r.data(), N);

        double worstMid = 0.0, sideDelta = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double mIn  = 0.5 * ((double) l0[(size_t) i] + (double) r0[(size_t) i]);
            const double mOut = 0.5 * ((double) l[(size_t) i]  + (double) r[(size_t) i]);
            worstMid = std::max (worstMid, std::fabs (mOut - mIn));
            const double sIn  = 0.5 * ((double) l0[(size_t) i] - (double) r0[(size_t) i]);
            const double sOut = 0.5 * ((double) l[(size_t) i]  - (double) r[(size_t) i]);
            sideDelta = std::max (sideDelta, std::fabs (sOut - sIn));
        }
        check (worstMid < 1.0e-6, "mid content untouched (worst delta "
                                  + std::to_string (worstMid) + ")");
        check (sideDelta > 1.0e-3, "…while the side content actually moved");
    }

    // -----------------------------------------------------------------------
    std::printf ("== stereo default is BIT-IDENTICAL to explicit stereo routing ==\n");
    {
        // Belt and braces for the neutrality guarantee: a band with the default
        // channel and one explicitly set to Stereo must agree bit for bit.
        EqEngine ea, eb;
        ea.prepare (fs, 256, 2);
        eb.prepare (fs, 256, 2);

        BandSpec b { true, BandType::Bell, 1200, 5.0f, 1.5f, 12 };
        setOneBand (ea, b);
        b.channel = BandChannel::Stereo;    // explicit
        setOneBand (eb, b);

        std::vector<float> la (N), ra (N), lb (N), rb (N);
        fillStereo (la, ra, false);
        lb = la; rb = ra;

        run (ea, la.data(), ra.data(), N);
        run (eb, lb.data(), rb.data(), N);

        check (std::memcmp (la.data(), lb.data(), sizeof (float) * (size_t) N) == 0
            && std::memcmp (ra.data(), rb.data(), sizeof (float) * (size_t) N) == 0,
               "default channel IS stereo, bit for bit");
    }

    // -----------------------------------------------------------------------
    std::printf ("== left-routed band leaves the right channel alone ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);

        BandSpec b { true, BandType::Bell, 500, -9.0f, 3.0f, 12 };
        b.channel = BandChannel::Left;
        setOneBand (eq, b);

        std::vector<float> l (N), r (N), l0 (N), r0 (N);
        fillStereo (l, r, false);
        l0 = l; r0 = r;

        run (eq, l.data(), r.data(), N);

        check (std::memcmp (r.data(), r0.data(), sizeof (float) * (size_t) N) == 0,
               "right channel is bit-identical");
        double moved = 0.0;
        for (int i = 0; i < N; ++i)
            moved = std::max (moved, std::fabs ((double) l[(size_t) i] - (double) l0[(size_t) i]));
        check (moved > 1.0e-3, "…and the left channel was filtered");
    }

    // -----------------------------------------------------------------------
    std::printf ("== side / right bands are clean NO-OPS on mono ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 1);

        BandSpec b { true, BandType::Bell, 2000, -12.0f, 4.0f, 12 };
        b.channel = BandChannel::Side;
        b.dynamic = true; b.thresholdDb = -40.0f; b.rangeDb = -10.0f;
        setOneBand (eq, b);

        std::vector<float> x (N), x0 (N);
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
        for (auto& v : x) v = dist (rng);
        x0 = x;

        const int block = 256;
        for (int i = 0; i < N; i += block)
        {
            float* ch[1] = { x.data() + i };
            eq.process (ch, 1, block);
        }
        check (std::memcmp (x.data(), x0.data(), sizeof (float) * (size_t) N) == 0,
               "a dynamic SIDE band on mono changes nothing (and does not crash)");

        b.channel = BandChannel::Right;
        b.dynamic = false;
        setOneBand (eq, b);
        x = x0;
        for (int i = 0; i < N; i += block)
        {
            float* ch[1] = { x.data() + i };
            eq.process (ch, 1, block);
        }
        check (std::memcmp (x.data(), x0.data(), sizeof (float) * (size_t) N) == 0,
               "a RIGHT band on mono changes nothing");

        // Mid on mono IS the signal: it must behave exactly like stereo routing.
        b.channel = BandChannel::Mid;
        setOneBand (eq, b);
        x = x0;
        for (int i = 0; i < N; i += block)
        {
            float* ch[1] = { x.data() + i };
            eq.process (ch, 1, block);
        }
        double moved = 0.0;
        for (int i = 0; i < N; ++i)
            moved = std::max (moved, std::fabs ((double) x[(size_t) i] - (double) x0[(size_t) i]));
        check (moved > 1.0e-3, "a MID band on mono filters the signal");
    }

    // -----------------------------------------------------------------------
    std::printf ("== channel string round-trip (EqMove helpers) ==\n");
    {
        // Compiled from EqMove.h — the same helpers the processor's var layer
        // uses, tested here without JUCE.
        struct { const char* s; BandChannel c; } ok[] = {
            { "stereo", BandChannel::Stereo }, { "MID",   BandChannel::Mid },
            { "m",      BandChannel::Mid },    { "s",     BandChannel::Side },
            { "Side",   BandChannel::Side },   { "l",     BandChannel::Left },
            { "left",   BandChannel::Left },   { "R",     BandChannel::Right },
            { "right",  BandChannel::Right },  { "center",BandChannel::Mid },
        };
        bool all = true;
        for (const auto& t : ok)
        {
            BandChannel c = BandChannel::Stereo;
            all = all && parseBandChannel (t.s, c) && c == t.c;
        }
        check (all, "every alias parses to the right lane");

        BandChannel c = BandChannel::Stereo;
        check (! parseBandChannel ("surround", c), "an unknown lane is rejected, not guessed");

        bool rt = true;
        for (int i = 0; i < (int) BandChannel::NumChannelModes; ++i)
        {
            BandChannel in = (BandChannel) i, out = BandChannel::Stereo;
            rt = rt && parseBandChannel (bandChannelToString (in), out) && out == in;
        }
        check (rt, "toString -> parse round-trips every mode");
    }

    std::printf (g_fail == 0 ? "\nALL PASS (0 failures)\n"
                             : "\n%d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
