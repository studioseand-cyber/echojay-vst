// Standalone test for the TIME cluster's shared core (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source delay_core_test.cpp -o delaycoretest && ./delaycoretest
//
// The core is the piece both Time devices stand on, so a bug here is a bug in
// two devices at once. What is pinned:
//
//   * an integer delay lands on the EXACT sample (off-by-one here is a delay
//     that is one sample wrong at every setting, which nothing downstream can
//     detect),
//   * the read/write ORDER convention, which is what makes a feedback loop have
//     a non-zero delay,
//   * fractional reads: linear halves, cubic reproduces a ramp exactly,
//   * the buffer wraps correctly past its own length,
//   * the filters do what their names say at DC and at Nyquist,
//   * the allpass preserves energy (that is the whole point of an allpass),
//   * softClip is bounded AND strictly contractive, which is what keeps 100%
//     feedback from diverging.

#include "EedDelayCore.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }

int main()
{
    constexpr double kSr = 48000.0;

    std::printf ("== DelayLine: integer delay lands on the exact sample ==\n");
    {
        DelayLine d;
        d.prepare (kSr, 0.1);

        // Push an impulse, then 20 zeros. After N pushes total, the impulse is
        // (N-1) samples "before the most recent push".
        d.push (1.0f);
        for (int i = 0; i < 20; ++i) d.push (0.0f);

        check (d.readInt (20) == 1.0f, "impulse is at k = 20 after 20 more pushes");
        check (d.readInt (19) == 0.0f, "k = 19 is empty");
        check (d.readInt (21) == 0.0f, "k = 21 is empty");
    }

    std::printf ("== DelayLine: k = 0 is the sample just pushed ==\n");
    {
        DelayLine d;
        d.prepare (kSr, 0.1);
        d.push (0.25f);
        check (d.readInt (0) == 0.25f, "k = 0 is the most recent push");
    }

    std::printf ("== DelayLine: read-before-write gives a real loop delay ==\n");
    {
        // The pattern every feedback loop in the cluster uses: read k = M-1,
        // then push. The value read must be M pushes old relative to the one
        // being written, or the loop is algebraic.
        DelayLine d;
        d.prepare (kSr, 0.1);

        const int M = 8;
        std::vector<float> out;
        for (int n = 0; n < 24; ++n)
        {
            const float back = d.readInt (M - 1);
            out.push_back (back);
            d.push (n == 0 ? 1.0f : 0.0f);
        }
        check (out[(size_t) M] == 1.0f, "the sample written at n = 0 is read at n = M");
        check (out[(size_t) M - 1] == 0.0f, "and not one sample earlier");
    }

    std::printf ("== DelayLine: linear interpolation halves between neighbours ==\n");
    {
        DelayLine d;
        d.prepare (kSr, 0.1);
        d.reset();

        // Lay down ... 0, 0, 1, 0, 0 ... so k = 4 holds 1.0 and k = 3/5 hold 0.
        d.push (1.0f);
        for (int i = 0; i < 4; ++i) d.push (0.0f);

        check (near (d.readLinear (4.0), 1.0, 1e-6),  "readLinear(4.0) == 1.0");
        check (near (d.readLinear (4.5), 0.5, 1e-6),  "readLinear(4.5) is the midpoint");
        check (near (d.readLinear (3.5), 0.5, 1e-6),  "readLinear(3.5) is the midpoint");
        check (near (d.readLinear (3.25), 0.25, 1e-6), "readLinear(3.25) is a quarter in");
    }

    std::printf ("== DelayLine: cubic reproduces a ramp EXACTLY ==\n");
    {
        // Catmull-Rom fits a cubic through four points, so a linear signal must
        // come back out linear. If interpolation is wrong at all, it shows here
        // as a fractional-delay-dependent error.
        DelayLine d;
        d.prepare (kSr, 0.1);
        d.reset();

        for (int n = 0; n < 64; ++n) d.push ((float) n);   // last pushed = 63

        bool ok = true;
        double worst = 0.0;
        for (int i = 1; i <= 30; ++i)
        {
            const double frac = 0.1 * (double) (i % 10);
            const double dly  = (double) i + frac;
            const double want = 63.0 - dly;              // value at that delay
            const double got  = d.readCubic (dly);
            worst = std::max (worst, std::fabs (got - want));
            if (std::fabs (got - want) > 1e-3) ok = false;
        }
        check (ok, "cubic on a ramp is exact (worst err "
                   + std::to_string (worst) + ")");
    }

    std::printf ("== DelayLine: wraps correctly past its own length ==\n");
    {
        DelayLine d;
        d.prepare (kSr, 0.001);          // ~48 samples -> 64-slot buffer
        d.reset();

        // Push far more than the buffer holds; the recent history must still be
        // right, which only happens if the mask wrap is correct.
        for (int n = 0; n < 5000; ++n) d.push ((float) (n % 97));

        bool ok = true;
        for (int k = 0; k < 32; ++k)
            if (d.readInt (k) != (float) ((4999 - k) % 97)) ok = false;

        check (ok, "history is intact after 5000 pushes through a 64-slot buffer");
    }

    std::printf ("== DelayLine: out-of-range reads are clamped, not wrapped ==\n");
    {
        DelayLine d;
        d.prepare (kSr, 0.001);
        d.reset();
        d.push (1.0f);
        for (int i = 0; i < 10; ++i) d.push (0.0f);

        // Asking for a delay longer than the line holds must not alias back onto
        // the impulse — that would be a phantom echo at a time nobody asked for.
        const float far = d.readInt (100000);
        check (far == 0.0f, "an absurd delay reads the oldest sample, not a wrapped one");
        check (d.readCubic (1.0e9) == d.readCubic (d.maxDelaySamples()),
               "cubic clamps to maxDelaySamples");
    }

    std::printf ("== OnePoleLowpass: DC passes, Nyquist is killed ==\n");
    {
        OnePoleLowpass lp;
        lp.setCutoff (1000.0, kSr);
        lp.reset();

        float y = 0.0f;
        for (int i = 0; i < 20000; ++i) y = lp.process (1.0f);
        check (near (y, 1.0, 1e-3), "DC settles at unity");

        lp.reset();
        float peak = 0.0f;
        for (int i = 0; i < 20000; ++i)
        {
            const float x = (i & 1) ? 1.0f : -1.0f;      // Nyquist
            const float o = lp.process (x);
            if (i > 10000) peak = std::max (peak, std::fabs (o));
        }
        check (peak < 0.1f, "Nyquist is attenuated below -20 dB");
    }

    std::printf ("== OnePoleHighpass: DC is blocked ==\n");
    {
        OnePoleHighpass hp;
        hp.setCutoff (200.0, kSr);
        hp.reset();

        float y = 0.0f;
        for (int i = 0; i < 40000; ++i) y = hp.process (1.0f);
        check (std::fabs (y) < 1e-2f, "DC decays away");

        hp.reset();
        float peak = 0.0f;
        for (int i = 0; i < 20000; ++i)
        {
            const float x = (i & 1) ? 1.0f : -1.0f;
            const float o = hp.process (x);
            if (i > 10000) peak = std::max (peak, std::fabs (o));
        }
        check (peak > 0.9f, "Nyquist passes through");
    }

    std::printf ("== LoopFilter: both ends trimmed, midband survives ==\n");
    {
        LoopFilter f;
        f.setHighpass (200.0, kSr);
        f.setLowpass (4000.0, kSr);
        f.reset();

        // 1 kHz sine sits between the two corners and must come through close to
        // unity, or the "filter" is really a volume control on the feedback.
        double peak = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            const float x = (float) std::sin (2.0 * M_PI * 1000.0 * i / kSr);
            const float o = f.process (x);
            if (i > 24000) peak = std::max (peak, (double) std::fabs (o));
        }
        check (peak > 0.85 && peak < 1.05, "1 kHz passes near unity (peak "
                                           + std::to_string (peak) + ")");
    }

    std::printf ("== SchroederAllpass: unity magnitude (energy in == energy out) ==\n");
    {
        SchroederAllpass ap;
        ap.prepare (kSr, 0.05);
        ap.reset();
        ap.setDelaySamples (233);
        ap.setGain (0.7f);

        // An allpass redistributes energy in time but does not add or remove it.
        // Feed an impulse and let the whole response run out.
        double energy = 0.0;
        double peak   = 0.0;
        for (int i = 0; i < 400000; ++i)
        {
            const float o = ap.process (i == 0 ? 1.0f : 0.0f);
            energy += (double) o * (double) o;
            peak = std::max (peak, (double) std::fabs (o));
        }
        check (near (energy, 1.0, 1e-3), "impulse energy is preserved (got "
                                         + std::to_string (energy) + ")");
        check (peak <= 1.0 + 1e-6, "no sample exceeds the input impulse");
    }

    std::printf ("== SchroederAllpass: diffuses (one in, many out) ==\n");
    {
        SchroederAllpass ap;
        ap.prepare (kSr, 0.05);
        ap.reset();
        ap.setDelaySamples (100);
        ap.setGain (0.6f);

        int nonZero = 0;
        for (int i = 0; i < 2000; ++i)
            if (std::fabs (ap.process (i == 0 ? 1.0f : 0.0f)) > 1e-4f) ++nonZero;

        check (nonZero > 5, "one impulse in produces a train out ("
                            + std::to_string (nonZero) + " taps)");
    }

    std::printf ("== SchroederAllpass: a zero delay is refused ==\n");
    {
        SchroederAllpass ap;
        ap.prepare (kSr, 0.05);
        ap.setDelaySamples (0);
        check (ap.delaySamples() >= 1, "delay is forced to at least 1 (no algebraic loop)");
    }

    std::printf ("== Lfo: rate, range and quadrature ==\n");
    {
        Lfo lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (1.0f);
        lfo.reset (0.0);

        check (near (lfo.sine(), 0.0, 1e-6),       "sine starts at 0");
        check (near (lfo.quadrature(), 1.0, 1e-6), "quadrature starts at 1 (90 deg ahead)");

        // A full second at 1 Hz is exactly one cycle back to the start.
        for (int i = 0; i < (int) kSr; ++i) lfo.advance();
        check (near (lfo.sine(), 0.0, 1e-3), "one second at 1 Hz is one whole cycle");

        // Quarter cycle: sine at peak, quadrature at zero.
        lfo.reset (0.0);
        for (int i = 0; i < (int) (kSr / 4); ++i) lfo.advance();
        check (near (lfo.sine(), 1.0, 1e-3),       "quarter cycle: sine at +1");
        check (near (lfo.quadrature(), 0.0, 1e-3), "quarter cycle: quadrature at 0");

        // Range, over a long run — a phase accumulator that drifts out of [0,1)
        // would eventually lose precision rather than fail loudly.
        lfo.setRateHz (7.3f);
        double lo = 1.0, hi = -1.0;
        for (int i = 0; i < 200000; ++i) { lfo.advance(); lo = std::min (lo, (double) lfo.sine()); hi = std::max (hi, (double) lfo.sine()); }
        check (lo < -0.999 && hi > 0.999, "sine still spans -1..+1 after 200k samples");
    }

    std::printf ("== Smoother: glides, settles, and snaps ==\n");
    {
        Smoother s;
        s.prepare (kSr, 0.020);
        s.snap (0.0f);
        s.setTarget (1.0f);

        const float first = s.next();
        check (first > 0.0f && first < 0.01f, "the first step is small (no jump)");

        for (int i = 0; i < 9600; ++i) s.next();     // 200 ms = 10 tau
        check (near (s.current(), 1.0, 1e-3), "settles on the target");

        s.setTarget (5.0f);
        s.snap();
        check (s.current() == 5.0f, "snap() jumps straight to the target");
    }

    std::printf ("== softClip: bounded, transparent, and strictly contractive ==\n");
    {
        check (softClip (0.0f) == 0.0f, "zero maps to zero");
        check (near (softClip (0.01f), 0.01, 1e-4), "transparent at low level");
        check (softClip (1000.0f) < 4.01f, "bounded above (huge input stays under 4)");
        check (softClip (-1000.0f) > -4.01f, "bounded below");

        // THE property that makes 100% feedback safe: |softClip(x)| < |x| for
        // every x != 0, so a unity-gain loop containing it always decays.
        bool contractive = true;
        for (int i = 1; i <= 4000; ++i)
        {
            const float x = (float) i * 0.01f;
            if (! (softClip (x) < x) || ! (softClip (-x) > -x)) contractive = false;
        }
        check (contractive, "strictly contractive everywhere off zero");
    }

    std::printf ("== flushDenorm ==\n");
    {
        check (flushDenorm (1.0e-30f) == 0.0f, "denormal-range value is flushed");
        check (flushDenorm (0.5f) == 0.5f,     "audible value is untouched");
        check (flushDenorm (-1.0e-30f) == 0.0f, "negative denormal is flushed");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
