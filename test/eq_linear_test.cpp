// Standalone test for the linear-phase mode (P4 of SURGICAL_EQ_ENHANCEMENTS.md).
// No JUCE. Build:
//   g++ -std=c++17 -O2 -I../Source eq_linear_test.cpp ../Source/EqEngine.cpp \
//       -o lintest && ./lintest
//
// Claims under test:
//   * Zero mode (the default) reports zero latency and passes an impulse at
//     n == 0 — the pre-P4 engine, untouched;
//   * Linear mode delays exactly by the reported latencySamples(), with a flat
//     EQ coming through as a clean delayed delta;
//   * the linear path's MAGNITUDE matches the analytic curve (what the UI
//     draws and what the SVF path realises) within tolerance;
//   * the impulse response is SYMMETRIC about the reported latency — that is
//     what "linear phase" means;
//   * a Side-routed band in linear mode still leaves an L==R signal alone.

#include "EqEngine.h"
#include "EqFft.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

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

// Push an impulse through the engine (stereo, block=256) and collect n samples.
static void collectImpulse (EqEngine& eq, std::vector<float>& outL,
                            std::vector<float>& outR, int n)
{
    outL.assign ((size_t) n, 0.0f);
    outR.assign ((size_t) n, 0.0f);
    outL[0] = 1.0f;
    outR[0] = 1.0f;
    const int block = 256;
    for (int i = 0; i < n; i += block)
    {
        float* ch[2] = { outL.data() + i, outR.data() + i };
        eq.process (ch, 2, std::min (block, n - i));
    }
}

int main()
{
    const double fs = 48000.0;

    // -----------------------------------------------------------------------
    std::printf ("== zero mode is the default and reports zero latency ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);
        check (eq.getPhaseMode() == EqEngine::PhaseMode::Zero, "fresh engine is Zero");
        check (eq.latencySamples() == 0, "Zero reports 0 samples");

        std::vector<float> l, r;
        collectImpulse (eq, l, r, 1024);
        check (std::fabs (l[0] - 1.0f) < 1.0e-6f, "impulse passes at n == 0");
    }

    // -----------------------------------------------------------------------
    std::printf ("== flat linear EQ: a clean delta at exactly the reported latency ==\n");
    int latency = 0;
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);
        eq.setPhaseMode (EqEngine::PhaseMode::Linear);
        latency = eq.latencySamples();
        check (latency > 0, "Linear reports latency (" + std::to_string (latency) + ")");

        std::vector<float> l, r;
        collectImpulse (eq, l, r, latency + 4096);

        int   peakAt = -1; float peak = 0.0f;
        float offPeakEnergy = 0.0f;
        for (int i = 0; i < (int) l.size(); ++i)
        {
            const float a = std::fabs (l[(size_t) i]);
            if (a > peak) { peak = a; peakAt = i; }
        }
        for (int i = 0; i < (int) l.size(); ++i)
            if (std::abs (i - peakAt) > 2)
                offPeakEnergy = std::max (offPeakEnergy, std::fabs (l[(size_t) i]));

        check (peakAt == latency, "delta lands at latencySamples ("
                                  + std::to_string (peakAt) + ")");
        check (std::fabs (peak - 1.0f) < 1.0e-3f, "…at unity gain ("
                                  + std::to_string (peak) + ")");
        check (offPeakEnergy < 1.0e-4f, "…and nothing else ("
                                  + std::to_string (offPeakEnergy) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== linear magnitude matches the analytic curve ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);
        setOneBand (eq, { true, BandType::Bell, 1000, 6.0f, 1.0f, 12 });
        eq.setPhaseMode (EqEngine::PhaseMode::Linear);

        const int N = 16384;                    // impulse capture, power of two
        std::vector<float> l, r;
        collectImpulse (eq, l, r, N);

        std::vector<float> im ((size_t) N, 0.0f);
        eqFft (l.data(), im.data(), N, false);

        const double freqs[] = { 250.0, 500.0, 1000.0, 2000.0, 4000.0 };
        double worst = 0.0;
        for (double f : freqs)
        {
            const int    k    = (int) std::lround (f * N / fs);
            const double magF = std::hypot ((double) l[(size_t) k], (double) im[(size_t) k]);
            const double gotDb = 20.0 * std::log10 (std::max (magF, 1.0e-12));

            float ff = (float) f, wantDb = 0.0f;
            eq.getMagnitudeResponse (&ff, &wantDb, 1);
            worst = std::max (worst, std::fabs (gotDb - (double) wantDb));
        }
        check (worst < 0.25, "worst deviation from analytic over 250..4k: "
                             + std::to_string (worst) + " dB");
    }

    // -----------------------------------------------------------------------
    std::printf ("== the impulse response is symmetric about the latency ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);
        setOneBand (eq, { true, BandType::Bell, 2000, -9.0f, 3.0f, 12 });
        eq.setPhaseMode (EqEngine::PhaseMode::Linear);

        std::vector<float> l, r;
        collectImpulse (eq, l, r, latency * 2 + 512);

        double worst = 0.0;
        for (int d = 1; d < latency - 512; ++d)     // whole windowed FIR span
        {
            const double a = (double) l[(size_t) (latency - d)];
            const double b = (double) l[(size_t) (latency + d)];
            worst = std::max (worst, std::fabs (a - b));
        }
        check (worst < 1.0e-5, "worst asymmetry " + std::to_string (worst)
                               + " — phase is genuinely linear");
    }

    // -----------------------------------------------------------------------
    std::printf ("== routing still holds on the FIR path ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);
        BandSpec b { true, BandType::HighShelf, 8000, 6.0f, 0.707f, 12 };
        b.channel = BandChannel::Side;
        setOneBand (eq, b);
        eq.setPhaseMode (EqEngine::PhaseMode::Linear);

        const int N = 8192;
        std::vector<float> l ((size_t) N), r ((size_t) N);
        for (int i = 0; i < N; ++i)
            l[(size_t) i] = r[(size_t) i] = 0.4f * std::sin (0.037f * (float) i);

        const int block = 256;
        for (int i = 0; i < N; i += block)
        {
            float* ch[2] = { l.data() + i, r.data() + i };
            eq.process (ch, 2, block);
        }

        // The output is the DELAYED input; compare against the delayed source.
        double worst = 0.0;
        for (int i = latency; i < N; ++i)
        {
            const double want = 0.4 * std::sin (0.037 * (double) (i - latency));
            worst = std::max (worst, std::fabs ((double) l[(size_t) i] - want));
            worst = std::max (worst, std::fabs ((double) l[(size_t) i]
                                              - (double) r[(size_t) i]));
        }
        check (worst < 2.0e-3, "a side shelf leaves L==R material untouched "
                               "through the FIR (worst " + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== mode flips are live and re-report ==\n");
    {
        EqEngine eq;
        eq.prepare (fs, 256, 2);
        eq.setPhaseMode (EqEngine::PhaseMode::Linear);
        check (eq.latencySamples() > 0, "linear latency on");
        eq.setPhaseMode (EqEngine::PhaseMode::Zero);
        check (eq.latencySamples() == 0, "back to zero on flip");

        // and the zero path still works after a round trip
        std::vector<float> l, r;
        collectImpulse (eq, l, r, 512);
        check (std::fabs (l[0] - 1.0f) < 1.0e-6f, "impulse at n == 0 again");
    }

    std::printf (g_fail == 0 ? "\nALL PASS (0 failures)\n"
                             : "\n%d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
