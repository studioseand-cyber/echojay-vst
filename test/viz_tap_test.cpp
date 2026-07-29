/*
    viz_tap_test.cpp  —  the JUCE-free half of the visualisation library.

    Source/viz/VizTap.h is the one piece of Source/viz that runs on the AUDIO
    THREAD, and it is JUCE-free precisely so it can be tested here rather than
    only through a plugin build — the same discipline EqEngine, DynamicsCore and
    StereoEngine follow.

    What is worth pinning, and why each one is a bug that has a shape:

      * the newest N samples come back in CHRONOLOGICAL order. Off-by-one in the
        modular arithmetic reads the ring rotated, which draws a goniometer that
        looks plausible and is wrong.
      * it WRAPS. A ring that is only ever written less than its own length is
        the case that always works; the failure lives past the wrap.
      * a partial fill returns what is there without garbage in front of it.
      * short storage round-trips to within a quantisation step, and CLAMPS
        rather than wrapping on over-unity input — an unclamped cast turns a
        clipping signal into a large negative one, which draws garbage at
        exactly the moment a user is staring at the scope.
      * mono in gives the same signal on both sides, so a mono source shows as a
        vertical line rather than as an empty scope.

    Build:
      g++ -std=c++17 -O2 -I../Source viz_tap_test.cpp -o viztaptest && ./viztaptest
*/

#include "viz/VizTap.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void check (bool ok, const std::string& what)
    {
        std::printf ("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (! ok) ++failures;
    }

    bool near (float a, float b, float tol) { return std::fabs (a - b) <= tol; }
}

int main()
{
    std::printf ("== FloatTap ==\n");
    {
        echojay::viz::FloatTap t;
        check (t.get() == 0.0f, "starts at zero");

        t.set (-6.5f);
        check (near (t.get(), -6.5f, 1.0e-6f), "reads back what was written");

        t.setIfLouder (-9.0f);
        check (near (t.get(), -6.5f, 1.0e-6f), "setIfLouder keeps the louder value");

        t.setIfLouder (-2.0f);
        check (near (t.get(), -2.0f, 1.0e-6f), "setIfLouder takes the louder value");

        echojay::viz::FloatTap seeded { -120.0f };
        check (near (seeded.get(), -120.0f, 1.0e-6f), "constructs with an initial value");
    }

    std::printf ("== SampleTap: order, wrap and partial fill ==\n");
    {
        // Float storage so the values are exact and an ordering fault cannot
        // hide behind a quantisation tolerance.
        echojay::viz::SampleTap<float, 16, 2> ring;

        // --- partial fill: fewer samples written than the ring holds --------
        float l[4] { 1.0f, 2.0f, 3.0f, 4.0f };
        float r[4] { -1.0f, -2.0f, -3.0f, -4.0f };
        ring.write (l, r, 4);

        float dl[8] {}, dr[8] {};
        int n = ring.read (dl, dr, 4);
        check (n == 4, "read returns the count asked for");
        check (dl[0] == 1.0f && dl[3] == 4.0f, "newest span is chronological (oldest first)");
        check (dr[0] == -1.0f && dr[3] == -4.0f, "the right channel tracks the left");

        // --- wrap: write past the ring's own length -------------------------
        // 20 more samples into a 16-slot ring, so the read must come entirely
        // from the second lap plus the tail of the first.
        std::vector<float> big (20);
        for (int i = 0; i < 20; ++i) big[(size_t) i] = 100.0f + (float) i;
        ring.write (big.data(), big.data(), 20);

        float wl[16] {}, wr[16] {};
        n = ring.read (wl, wr, 16);
        check (n == 16, "a full read returns the ring's length");

        // The last 16 written were 104..119.
        bool ordered = true;
        for (int i = 0; i < 16; ++i)
            if (wl[i] != 104.0f + (float) i) ordered = false;
        check (ordered, "the newest 16 come back in order ACROSS the wrap");

        // --- over-read is clamped, not out of bounds ------------------------
        float ol[64] {};
        n = ring.read (ol, nullptr, 64);
        check (n == 16, "reading more than the ring holds clamps to its size");
        check (ol[15] == 119.0f, "the last sample read is the newest written");

        // --- clear ----------------------------------------------------------
        ring.clear();
        float cl[16] {};
        ring.read (cl, nullptr, 16);
        bool allZero = true;
        for (float v : cl) if (v != 0.0f) allZero = false;
        check (allZero, "clear() empties the ring");
    }

    std::printf ("== SampleTap: short storage clamps and round-trips ==\n");
    {
        echojay::viz::SampleTap<short, 8, 2> ring;

        float in[4]  { 0.5f, -0.5f, 1.9f, -1.9f };
        ring.write (in, in, 4);

        float out[4] {};
        ring.read (out, nullptr, 4);

        // One 16-bit step is ~3e-5; a tolerance an order up from that catches a
        // scaling error without failing on rounding.
        check (near (out[0],  0.5f, 1.0e-4f), "+0.5 round-trips through short");
        check (near (out[1], -0.5f, 1.0e-4f), "-0.5 round-trips through short");

        // The one that matters: over-unity CLAMPS. An unclamped cast wraps
        // +1.9 round to a large negative and the scope draws nonsense exactly
        // when the signal is clipping.
        check (near (out[2],  1.0f, 1.0e-4f), "+1.9 clamps to +1, it does not wrap");
        check (near (out[3], -1.0f, 1.0e-4f), "-1.9 clamps to -1, it does not wrap");
    }

    std::printf ("== SampleTap: mono sources ==\n");
    {
        echojay::viz::SampleTap<float, 8, 2> stereo;

        float mono[4] { 0.25f, 0.5f, 0.75f, 1.0f };
        stereo.write (mono, nullptr, 4);          // null right = mono source

        float dl[4] {}, dr[4] {};
        stereo.read (dl, dr, 4);

        bool duplicated = true;
        for (int i = 0; i < 4; ++i)
            if (dl[i] != dr[i] || dl[i] != mono[i]) duplicated = false;

        // A goniometer fed a mono source must draw a vertical line, which needs
        // L == R. Leaving the right channel silent would draw a diagonal — the
        // picture of a hard-panned source, which is the opposite of the truth.
        check (duplicated, "a null right channel is duplicated, not left silent");

        // And the mono ring itself, which is what the harmonic bars use.
        echojay::viz::SampleTap<float, 8, 1> single;
        single.write (mono, 4);
        float sl[4] {};
        check (single.read (sl, 4) == 4 && sl[3] == 1.0f, "the mono overload writes and reads");
    }

    std::printf ("== the aliases the library actually uses ==\n");
    {
        // Pinned because a device declares these by name; a size change here is
        // a change to how much history every scope shows.
        check (echojay::viz::ScopeTap::size == 2048 && echojay::viz::ScopeTap::numChannels == 2,
               "ScopeTap is a 2048-sample stereo ring");
        check (echojay::viz::SpectrumTap::size == 4096 && echojay::viz::SpectrumTap::numChannels == 1,
               "SpectrumTap is a 4096-sample mono ring");

        // Degenerate inputs must be no-ops rather than crashes: a device can be
        // handed a zero-length block, and an editor can poll before anything
        // has ever been written.
        echojay::viz::ScopeTap t;
        t.write (nullptr, nullptr, 64);
        t.write (nullptr, 0);
        float l[4] {}, r[4] {};
        t.write (l, r, 0);
        check (t.read (nullptr, nullptr, 4) == 0, "reading into null is a no-op");
        check (t.read (l, r, 0) == 0, "reading zero samples is a no-op");
        check (t.read (l, r, 4) == 4, "an untouched ring still reads (as silence)");
    }

    std::printf (failures == 0 ? "\nALL VIZ TAP TESTS PASSED\n"
                               : "\n%d VIZ TAP TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
