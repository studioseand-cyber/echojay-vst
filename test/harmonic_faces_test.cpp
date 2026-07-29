// Standalone test for the two Harmonic faces that are more than a HarmonicCore
// with knobs on: Tape (transport + head bump + HF loss) and Exciter (band split).
// No JUCE. Build:
//   g++ -std=c++17 -O2 -I../Source harmonic_faces_test.cpp ../Source/EedHarmonicCore.cpp -o ftest && ./ftest
//
// The properties worth pinning are the ones a listener would only ever notice as
// "something is subtly wrong": a dry path that is not delay-matched (comb
// filtering at partial mix), a crossover that does not reconstruct (a permanent
// notch at the split), and a reported latency that does not match the real one
// (everything in parallel with the track drifting early).

#include "EedTapeEngine.h"
#include "EedExciterEngine.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static constexpr double kPi = 3.14159265358979323846;

static double magnitudeAt (const std::vector<float>& x, double freqHz, double sr)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const std::size_t n = x.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) (n - 1));
        const double t = 2.0 * kPi * freqHz * (double) i / sr;
        re += (double) x[i] * w * std::cos (t);
        im -= (double) x[i] * w * std::sin (t);
        wsum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

static double toDb (double a) { return 20.0 * std::log10 (a > 1.0e-12 ? a : 1.0e-12); }

// Push a sine through an engine and hand back input and (settled) output, so a
// caller can compare them sample for sample.
template <typename Engine>
static void runSine (Engine& e, double freqHz, double sr, double amp, int n,
                     std::vector<float>& in, std::vector<float>& out, int block = 256)
{
    in.assign ((std::size_t) n, 0.0f);
    out.assign ((std::size_t) n, 0.0f);

    for (int i = 0; i < n; ++i)
        in[(std::size_t) i] = (float) (amp * std::sin (2.0 * kPi * freqHz * (double) i / sr));

    std::vector<float> buf ((std::size_t) block);
    for (int off = 0; off + block <= n; off += block)
    {
        for (int i = 0; i < block; ++i) buf[(std::size_t) i] = in[(std::size_t) (off + i)];
        e.process (buf.data(), nullptr, block);
        for (int i = 0; i < block; ++i) out[(std::size_t) (off + i)] = buf[(std::size_t) i];
    }
}

// The tail of a buffer, so the measurement excludes the settling transient.
static std::vector<float> tail (const std::vector<float>& v, int n)
{
    return std::vector<float> (v.end() - n, v.end());
}

int main()
{
    constexpr double sr = 48000.0;

    // =======================================================================
    std::printf ("== TAPE: reported latency is the transport plus the oversampler ==\n");
    {
        TapeEngine t;
        t.prepare (sr, 256);
        // 2.5 ms at 48 kHz is 120 samples; 4x oversampling adds 45.
        check (t.latencySamples() == 165,
               "165 samples at 48 kHz (got " + std::to_string (t.latencySamples()) + ")");
    }

    std::printf ("== TAPE: 0%% wet is the input delayed by EXACTLY that latency ==\n");
    {
        TapeEngine t;
        t.prepare (sr, 256);
        t.setDriveDb (24.0f);          // wet path hammered, and inaudible
        t.setBias (80.0f);
        t.setWow (100.0f);
        t.setFlutter (100.0f);
        t.setMixPercent (0.0f);
        t.reset();

        std::vector<float> in, out;
        runSine (t, 997.0, sr, 0.5, 16384, in, out);

        const int lat = t.latencySamples();
        double worst = 0.0;
        for (int i = 4096; i < 16384; ++i)
            worst = std::fmax (worst, std::fabs ((double) out[(std::size_t) i]
                                               - (double) in[(std::size_t) (i - lat)]));
        check (worst < 1.0e-5, "dry path is delay-matched (worst error "
                               + std::to_string (worst) + ")");
    }

    std::printf ("== TAPE: with no wobble the transport is a plain delay ==\n");
    {
        TapeEngine t;
        t.prepare (sr, 256);
        t.setDriveDb (0.0f);
        t.setBias (0.0f);
        t.setWow (0.0f);
        t.setFlutter (0.0f);
        t.setHeadBumpDb (0.0f);
        t.setSpeedIps (30.0f);         // the least HF loss available
        t.setMixPercent (100.0f);
        t.reset();

        std::vector<float> in, out;
        // A 7.4 Hz sideband is only 7.4 Hz from the carrier, so the measurement
        // window has to be long enough for the two to be several bins apart —
        // otherwise what gets measured is the window's own leakage, not a
        // sideband, and the test passes or fails on arithmetic rather than DSP.
        runSine (t, 1000.0, sr, 0.1, 131072, in, out);

        const auto seg = tail (out, 65536);          // 0.73 Hz bins: 10 bins apart
        const double fund = toDb (magnitudeAt (seg, 1000.0, sr));
        const double side = toDb (magnitudeAt (seg, 1007.4, sr));   // flutter sideband
        check (side < fund - 60.0, "no flutter sideband at 0% flutter ("
                                   + std::to_string (side - fund) + " dB down)");
    }

    std::printf ("== TAPE: wow and flutter actually modulate ==\n");
    {
        auto sidebandDb = [&] (float wow, float flutter)
        {
            TapeEngine t;
            t.prepare (sr, 256);
            t.setDriveDb (0.0f);
            t.setBias (0.0f);
            t.setHeadBumpDb (0.0f);
            t.setSpeedIps (15.0f);
            t.setWow (wow);
            t.setFlutter (flutter);
            t.setMixPercent (100.0f);
            t.reset();

            std::vector<float> in, out;
            runSine (t, 1000.0, sr, 0.1, 131072, in, out);
            const auto seg = tail (out, 65536);
            const double fund = toDb (magnitudeAt (seg, 1000.0, sr));
            const double side = toDb (magnitudeAt (seg, 1007.4, sr));
            return side - fund;
        };

        const double off = sidebandDb (0.0f, 0.0f);
        const double on  = sidebandDb (0.0f, 100.0f);
        std::printf ("    flutter sideband: off %.1f dB, on %.1f dB\n", off, on);
        check (on > off + 20.0, "100% flutter raises the 7.4 Hz sideband by >20 dB");
    }

    std::printf ("== TAPE: head bump lifts the low end by about what it says ==\n");
    {
        auto lowDb = [&] (float bumpDb, double freq)
        {
            TapeEngine t;
            t.prepare (sr, 256);
            t.setDriveDb (0.0f);
            t.setBias (0.0f);
            t.setWow (0.0f);
            t.setFlutter (0.0f);
            t.setSpeedIps (15.0f);          // bump at 3.3 * 15 = 49.5 Hz
            t.setHeadBumpDb (bumpDb);
            t.setMixPercent (100.0f);
            t.reset();

            std::vector<float> in, out;
            runSine (t, freq, sr, 0.05, 65536, in, out);
            return toDb (magnitudeAt (tail (out, 32768), freq, sr));
        };

        const double flat = lowDb (0.0f, 49.5);
        const double lift = lowDb (6.0f, 49.5);
        check (std::fabs ((lift - flat) - 6.0) < 1.0,
               "+6 dB bump gives " + std::to_string (lift - flat) + " dB at 49.5 Hz");

        const double farFlat = lowDb (0.0f, 1000.0);
        const double farLift = lowDb (6.0f, 1000.0);
        check (std::fabs (farLift - farFlat) < 1.0,
               "and leaves 1 kHz alone (" + std::to_string (farLift - farFlat) + " dB)");
    }

    std::printf ("== TAPE: speed moves the bump and the HF loss ==\n");
    {
        check (TapeEngine::bumpHzFor (7.5f)  < TapeEngine::bumpHzFor (15.0f), "7.5 ips bumps lower than 15");
        check (TapeEngine::bumpHzFor (15.0f) < TapeEngine::bumpHzFor (30.0f), "15 ips bumps lower than 30");
        check (TapeEngine::lossHzFor (7.5f, sr) < TapeEngine::lossHzFor (30.0f, sr),
               "slower tape loses more top");
        check (TapeEngine::lossHzFor (30.0f, sr) <= sr * 0.45,
               "the HF corner stays below Nyquist at any speed");

        TapeEngine t;
        t.prepare (sr, 256);
        t.setDriveDb (0.0f); t.setBias (0.0f); t.setWow (0.0f);
        t.setFlutter (0.0f); t.setHeadBumpDb (0.0f); t.setMixPercent (100.0f);

        auto topDb = [&] (float ips)
        {
            t.setSpeedIps (ips);
            t.reset();
            std::vector<float> in, out;
            runSine (t, 12000.0, sr, 0.05, 32768, in, out);
            return toDb (magnitudeAt (tail (out, 16384), 12000.0, sr));
        };

        const double slow = topDb (7.5f);
        const double fast = topDb (30.0f);
        std::printf ("    12 kHz: 7.5 ips %.2f dB, 30 ips %.2f dB\n", slow, fast);
        check (fast > slow + 2.0, "30 ips keeps " + std::to_string (fast - slow)
                                  + " dB more 12 kHz than 7.5 ips");
    }

    std::printf ("== TAPE: bias buys EVEN harmonics, and no DC with it ==\n");
    {
        auto harmonics = [&] (float bias, double& h2, double& dc)
        {
            TapeEngine t;
            t.prepare (sr, 256);
            t.setDriveDb (18.0f);
            t.setWow (0.0f); t.setFlutter (0.0f); t.setHeadBumpDb (0.0f);
            t.setSpeedIps (30.0f);
            t.setBias (bias);
            t.setMixPercent (100.0f);
            t.reset();

            std::vector<float> in, out;
            runSine (t, 500.0, sr, 0.5, 32768, in, out);
            const auto seg = tail (out, 16384);
            const double f = toDb (magnitudeAt (seg, 500.0, sr));
            h2 = toDb (magnitudeAt (seg, 1000.0, sr)) - f;

            double mean = 0.0;
            for (float v : seg) mean += (double) v;
            dc = mean / (double) seg.size();
        };

        double h2none = 0, h2max = 0, dcNone = 0, dcMax = 0;
        harmonics (0.0f,  h2none, dcNone);
        harmonics (100.0f, h2max, dcMax);
        std::printf ("    2nd harmonic: bias 0 -> %.1f dB, bias 100 -> %.1f dB\n", h2none, h2max);
        check (h2max > h2none + 15.0, "bias raises the 2nd harmonic by >15 dB");
        check (std::fabs (dcMax) < 2.0e-3, "and leaves no DC behind (mean "
                                           + std::to_string (dcMax) + ")");
    }

    // =======================================================================
    std::printf ("== EXCITER: latency is the oversampler's, and nothing else ==\n");
    {
        ExciterEngine x;
        x.prepare (sr, 256);
        check (x.latencySamples() == 45, "45 samples (got "
                                         + std::to_string (x.latencySamples()) + ")");
    }

    std::printf ("== EXCITER: amount 0 reconstructs the input EXACTLY ==\n");
    {
        // The whole reason the split is subtractive. A pair of independent
        // filters would leave a notch at the crossover that no amount of
        // "amount 0" could undo.
        ExciterEngine x;
        x.prepare (sr, 256);
        x.setFreqHz (3000.0f);
        x.setAmount (0.0f);
        x.setMixPercent (100.0f);
        x.reset();

        std::vector<float> in, out;
        runSine (x, 3000.0, sr, 0.4, 16384, in, out);       // right AT the split

        const int lat = x.latencySamples();
        double worst = 0.0;
        for (int i = 4096; i < 16384; ++i)
            worst = std::fmax (worst, std::fabs ((double) out[(std::size_t) i]
                                               - (double) in[(std::size_t) (i - lat)]));
        check (worst < 2.0e-4, "a tone at the crossover survives amount 0 untouched "
                               "(worst error " + std::to_string (worst) + ")");
    }

    std::printf ("== EXCITER: 0%% wet is the input delayed by the reported latency ==\n");
    {
        ExciterEngine x;
        x.prepare (sr, 256);
        x.setAmount (100.0f);
        x.setMixPercent (0.0f);
        x.reset();

        std::vector<float> in, out;
        runSine (x, 800.0, sr, 0.5, 16384, in, out);

        const int lat = x.latencySamples();
        double worst = 0.0;
        for (int i = 4096; i < 16384; ++i)
            worst = std::fmax (worst, std::fabs ((double) out[(std::size_t) i]
                                               - (double) in[(std::size_t) (i - lat)]));
        check (worst < 1.0e-5, "dry path is delay-matched (worst error "
                               + std::to_string (worst) + ")");
    }

    std::printf ("== EXCITER: it shapes ABOVE the split and leaves below it alone ==\n");
    {
        auto thirdHarmonic = [&] (double toneHz, float amount, float splitHz)
        {
            ExciterEngine x;
            x.prepare (sr, 256);
            x.setFreqHz (splitHz);
            x.setAmount (amount);
            x.setMode (0);                 // tube
            x.setMixPercent (100.0f);
            x.reset();

            std::vector<float> in, out;
            runSine (x, toneHz, sr, 0.4, 32768, in, out);
            const auto seg = tail (out, 16384);
            return toDb (magnitudeAt (seg, toneHz * 3.0, sr))
                 - toDb (magnitudeAt (seg, toneHz, sr));
        };

        // A tone well above the split: fully in the shaped band.
        const double hiOff = thirdHarmonic (6000.0, 0.0f,   2000.0f);
        const double hiOn  = thirdHarmonic (6000.0, 100.0f, 2000.0f);
        std::printf ("    6 kHz tone, split 2 kHz: 3rd harmonic %.1f -> %.1f dB\n", hiOff, hiOn);
        check (hiOn > hiOff + 25.0, "a tone above the split gains harmonics");

        // A tone far below the split. It is not perfectly untouched — a 12 dB/oct
        // split is not a brick wall, so a little of it does reach the shaper —
        // but the residual has to stay inaudible in absolute terms, not merely
        // smaller than the tone above the split.
        const double loOn = thirdHarmonic (150.0, 100.0f, 6000.0f);
        std::printf ("    150 Hz tone, split 6 kHz: 3rd harmonic %.1f dB\n", loOn);
        check (loOn < -60.0, "a tone below the split stays >60 dB clean ("
                             + std::to_string (loOn) + " dB)");
        check (loOn < hiOn - 30.0, "and picks up " + std::to_string (hiOn - loOn)
                                   + " dB less than the tone above the split");
    }

    std::printf ("== EXCITER: both modes work and differ ==\n");
    {
        auto secondHarmonic = [&] (int mode)
        {
            ExciterEngine x;
            x.prepare (sr, 256);
            x.setFreqHz (1000.0f);
            x.setAmount (100.0f);
            x.setMode (mode);
            x.setMixPercent (100.0f);
            x.reset();

            std::vector<float> in, out;
            runSine (x, 4000.0, sr, 0.4, 32768, in, out);
            const auto seg = tail (out, 16384);
            return toDb (magnitudeAt (seg, 8000.0, sr)) - toDb (magnitudeAt (seg, 4000.0, sr));
        };

        const double tube = secondHarmonic (0);
        const double tape = secondHarmonic (1);
        std::printf ("    2nd harmonic: tube %.1f dB, tape %.1f dB\n", tube, tape);
        check (tube > tape + 10.0, "tube mode makes far more 2nd harmonic than tape mode "
                                   "(it is the asymmetric curve)");
    }

    // =======================================================================
    std::printf ("== both faces survive silence, DC and an oversized block ==\n");
    {
        TapeEngine t;    t.prepare (sr, 64);
        ExciterEngine x; x.prepare (sr, 64);
        t.reset(); x.reset();

        std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
        t.process (l.data(), r.data(), 1024);
        x.process (l.data(), r.data(), 1024);
        bool silent = true;
        for (std::size_t i = 0; i < l.size(); ++i)
            if (std::fabs (l[i]) > 1.0e-6f || std::fabs (r[i]) > 1.0e-6f) silent = false;
        check (silent, "silence in, silence out");

        std::fill (l.begin(), l.end(), 0.9f);
        std::fill (r.begin(), r.end(), -0.9f);
        t.process (l.data(), r.data(), 1024);
        x.process (l.data(), r.data(), 1024);
        bool finite = true;
        for (std::size_t i = 0; i < l.size(); ++i)
            if (! std::isfinite (l[i]) || ! std::isfinite (r[i])) finite = false;
        check (finite, "full-scale DC through a block 16x the prepared size stays finite");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
