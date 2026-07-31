// Standalone test for the Harmonic cluster's shared DSP core (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source harmonic_core_test.cpp ../Source/EedHarmonicCore.cpp -o htest && ./htest
//
// The claim worth testing here is the anti-aliasing one, because it is the only
// part of a saturator you cannot hear as "wrong" — a folded harmonic is not a
// distorted note, it is an in-tune-sounding whistle at an unrelated pitch, and
// it is exactly what separates a usable saturator from a harsh one. So the alias
// products are MEASURED, at 1x and at 4x, and the difference is asserted.

#include "EedHarmonicCore.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay::harmonic;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }

static constexpr double kPi = 3.14159265358979323846;

// Amplitude of one frequency in a buffer, by direct correlation (a one-bin DFT).
// Windowed, so a component that is not exactly on a bin does not smear into the
// neighbours and flatter the result.
static double magnitudeAt (const std::vector<float>& x, double freqHz, double sr)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const std::size_t n = x.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        // Hann
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) (n - 1));
        const double t = 2.0 * kPi * freqHz * (double) i / sr;
        re += (double) x[i] * w * std::cos (t);
        im -= (double) x[i] * w * std::sin (t);
        wsum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

static double toDb (double a) { return 20.0 * std::log10 (a > 1.0e-12 ? a : 1.0e-12); }

// Run a sine through a HarmonicCore configured a particular way and return the
// (settled) output.
static std::vector<float> runTone (HarmonicCore& core, double freqHz, double sr,
                                   double amp, int numSamples, int block = 256)
{
    std::vector<float> out ((std::size_t) numSamples, 0.0f);

    // Run and discard a settling pass so the smoothers, the filters and the
    // oversampler's history are all at steady state before anything is measured.
    const int warmup = 8192;
    long phase = 0;
    std::vector<float> buf ((std::size_t) block);

    for (int done = 0; done < warmup; done += block)
    {
        for (int i = 0; i < block; ++i, ++phase)
            buf[(std::size_t) i] = (float) (amp * std::sin (2.0 * kPi * freqHz * (double) phase / sr));
        core.process (buf.data(), nullptr, block);
    }

    for (int done = 0; done < numSamples; done += block)
    {
        const int n = std::min (block, numSamples - done);
        for (int i = 0; i < n; ++i, ++phase)
            buf[(std::size_t) i] = (float) (amp * std::sin (2.0 * kPi * freqHz * (double) phase / sr));
        core.process (buf.data(), nullptr, n);
        for (int i = 0; i < n; ++i) out[(std::size_t) (done + i)] = buf[(std::size_t) i];
    }

    return out;
}

int main()
{
    // -----------------------------------------------------------------------
    std::printf ("== curves: unity slope at zero (min drive is not a level change) ==\n");
    {
        const float h = 1.0e-4f;
        for (int i = 0; i < kCurveCount; ++i)
        {
            const Curve c = curveFromIndex (i);
            const double slope = (shape (c, h) - shape (c, -h)) / (2.0 * h);
            check (near (slope, 1.0, 1.0e-3),
                   std::string (curveName (c)) + ": slope at 0 is 1 (got "
                   + std::to_string (slope) + ")");
        }
    }

    std::printf ("== curves: bounded, monotonic, and odd-ish through the origin ==\n");
    {
        for (int i = 0; i < kCurveCount; ++i)
        {
            const Curve c = curveFromIndex (i);
            bool bounded = true, monotonic = true;
            float prev = shape (c, -50.0f);
            for (int k = -5000; k <= 5000; ++k)
            {
                const float y = shape (c, (float) k * 0.01f);
                if (! std::isfinite (y) || std::fabs (y) > 2.0f) bounded = false;
                if (y < prev - 1.0e-6f) monotonic = false;
                prev = y;
            }
            check (bounded,   std::string (curveName (c)) + ": stays finite and bounded");
            check (monotonic, std::string (curveName (c)) + ": monotonic (no fold-back)");
            check (near (shape (c, 0.0f), 0.0, 1.0e-9), std::string (curveName (c)) + ": f(0) == 0");
        }
    }

    std::printf ("== tube and diode are ASYMMETRIC (that is where 2nd harmonic lives) ==\n");
    {
        check (std::fabs (shape (Curve::Tube, 3.0f) + shape (Curve::Tube, -3.0f)) > 0.2f,
               "tube: f(x) != -f(-x)");
        check (std::fabs (shape (Curve::Diode, 3.0f) + shape (Curve::Diode, -3.0f)) > 0.2f,
               "diode: f(x) != -f(-x)");
        check (near (shape (Curve::Soft, 3.0f) + shape (Curve::Soft, -3.0f), 0.0, 1.0e-6),
               "soft: symmetric, odd harmonics only");
        check (near (shape (Curve::Tape, 3.0f) + shape (Curve::Tape, -3.0f), 0.0, 1.0e-6),
               "tape: symmetric, odd harmonics only");
    }

    std::printf ("== bias adds asymmetry WITHOUT adding DC ==\n");
    {
        check (near (shapeBiased (Curve::Tape, 0.0f, 0.4f), 0.0, 1.0e-6),
               "a silent input stays silent at any bias (no DC step)");
        const float a = shapeBiased (Curve::Tape, 0.5f, 0.4f);
        const float b = shapeBiased (Curve::Tape, -0.5f, 0.4f);
        check (std::fabs (a + b) > 1.0e-3f, "biased curve is asymmetric about the input");
    }

    // -----------------------------------------------------------------------
    std::printf ("== emphasis: Both IS the shipped curve, and the maths behaves ==\n");
    {
        // The neutrality claim, at the function level and bit-exact: this is
        // what lets an existing preset restore unchanged.
        bool exact = true;
        for (int i = 0; i < kCurveCount; ++i)
            for (int k = -400; k <= 400; ++k)
            {
                const Curve c = curveFromIndex (i);
                const float x = (float) k * 0.01f;
                if (shapeEmphasis (c, Emphasis::Both, x) != shape (c, x)) exact = false;
                if (shapeEmphasisBiased (c, Emphasis::Both, x, 0.0f) != shape (c, x)) exact = false;
            }
        check (exact, "Both emphasis (and zero bias) is BIT-EXACTLY the plain curve");

        const float h = 1.0e-4f;
        for (int e = 0; e < kEmphasisCount; ++e)
            for (int i = 0; i < kCurveCount; ++i)
            {
                const Curve    c  = curveFromIndex (i);
                const Emphasis em = emphasisFromIndex (e);
                const double slope = (shapeEmphasis (c, em, h) - shapeEmphasis (c, em, -h))
                                     / (2.0 * h);
                check (near (slope, 1.0, 1.0e-3)
                       && near (shapeEmphasis (c, em, 0.0f), 0.0, 1.0e-9),
                       std::string (curveName (c)) + "/" + emphasisName (em)
                       + ": slope 1 at 0 and f(0) == 0");
            }

        // Odd symmetrises everything; Even makes even the symmetric curves
        // asymmetric. That is the entire contract.
        for (int i = 0; i < kCurveCount; ++i)
        {
            const Curve c = curveFromIndex (i);
            check (near (shapeEmphasis (c, Emphasis::Odd, 3.0f)
                         + shapeEmphasis (c, Emphasis::Odd, -3.0f), 0.0, 1.0e-6),
                   std::string (curveName (c)) + "/odd: symmetric, odd harmonics only");
            check (std::fabs (shapeEmphasis (c, Emphasis::Even, 3.0f)
                              + shapeEmphasis (c, Emphasis::Even, -3.0f)) > 0.2f,
                   std::string (curveName (c)) + "/even: asymmetric, even harmonics live");
        }
    }

    std::printf ("== EMPHASIS: even vs odd measurably flips the 2nd/3rd ratio ==\n");
    {
        constexpr double sr = 48000.0;

        auto ratio = [&] (Emphasis em, double& h2, double& h3)
        {
            HarmonicCore core;
            core.setOversampling (4);
            core.prepare (sr, 256);
            core.setCurve (Curve::Soft);   // symmetric base: emphasis does ALL the work
            core.setEmphasis (em);
            core.setDriveDb (24.0f);
            core.setMixPercent (100.0f);
            core.reset();

            const auto out = runTone (core, 220.0, sr, 0.4, 16384);
            const double f = toDb (magnitudeAt (out, 220.0, sr));
            h2 = toDb (magnitudeAt (out, 440.0, sr)) - f;
            h3 = toDb (magnitudeAt (out, 660.0, sr)) - f;
        };

        double h2e = 0, h3e = 0, h2o = 0, h3o = 0, h2b = 0, h3b = 0;
        ratio (Emphasis::Even, h2e, h3e);
        ratio (Emphasis::Odd,  h2o, h3o);
        ratio (Emphasis::Both, h2b, h3b);

        std::printf ("    soft curve: even 2nd %.1f / 3rd %.1f, odd 2nd %.1f / 3rd %.1f, "
                     "both 2nd %.1f / 3rd %.1f dB\n", h2e, h3e, h2o, h3o, h2b, h3b);

        check (h2e > -30.0, "even emphasis makes a real 2nd harmonic on a symmetric curve");
        check (h2o < h2e - 25.0, "odd emphasis has " + std::to_string (h2e - h2o)
                                 + " dB less 2nd than even - the ratio flips");
        check (h3o > -30.0, "odd emphasis still makes a strong 3rd (it is not just quiet)");
        check ((h2e - h3e) > (h2o - h3o) + 25.0,
               "the 2nd/3rd RATIO moves by >25 dB between even and odd");

        // And on an asymmetric curve, odd takes the 2nd AWAY.
        auto tube2nd = [&] (Emphasis em)
        {
            HarmonicCore core;
            core.setOversampling (4);
            core.prepare (sr, 256);
            core.setCurve (Curve::Tube);
            core.setEmphasis (em);
            core.setDriveDb (24.0f);
            core.setMixPercent (100.0f);
            core.reset();
            const auto out = runTone (core, 220.0, sr, 0.4, 16384);
            return toDb (magnitudeAt (out, 440.0, sr)) - toDb (magnitudeAt (out, 220.0, sr));
        };

        const double tubeBoth = tube2nd (Emphasis::Both);
        const double tubeOdd  = tube2nd (Emphasis::Odd);
        std::printf ("    tube curve: 2nd with both %.1f dB, with odd %.1f dB\n",
                     tubeBoth, tubeOdd);
        check (tubeOdd < tubeBoth - 20.0,
               "odd emphasis strips the tube curve's own 2nd harmonic");
    }

    // -----------------------------------------------------------------------
    std::printf ("== HPF: the lows are left unshaped, the highs still driven ==\n");
    {
        constexpr double sr = 48000.0;

        auto thirdHarmonic = [&] (double toneHz, float hpfHz)
        {
            HarmonicCore core;
            core.setOversampling (4);
            core.prepare (sr, 256);
            core.setCurve (Curve::Soft);
            core.setDriveDb (30.0f);
            core.setHpfHz (hpfHz);
            core.setMixPercent (100.0f);
            core.reset();
            const auto out = runTone (core, toneHz, sr, 0.4, 32768);
            return toDb (magnitudeAt (out, toneHz * 3.0, sr))
                 - toDb (magnitudeAt (out, toneHz, sr));
        };

        // A 60 Hz tone under a 300 Hz split: the sub stays clean.
        const double subOff = thirdHarmonic (60.0, 0.0f);
        const double subOn  = thirdHarmonic (60.0, 300.0f);
        std::printf ("    60 Hz tone: 3rd harmonic %.1f dB (hpf 0) -> %.1f dB (hpf 300)\n",
                     subOff, subOn);
        check (subOff > -20.0, "with the split off, the sub really is saturated (the "
                               "test would be vacuous otherwise)");
        check (subOn < subOff - 20.0, "hpf 300 leaves the 60 Hz tone >20 dB cleaner");

        // A tone well above the split keeps its distortion.
        const double hiOff = thirdHarmonic (2000.0, 0.0f);
        const double hiOn  = thirdHarmonic (2000.0, 120.0f);
        std::printf ("    2 kHz tone: 3rd harmonic %.1f dB (hpf 0) vs %.1f dB (hpf 120)\n",
                     hiOff, hiOn);
        check (std::fabs (hiOn - hiOff) < 3.0, "and a 2 kHz tone is still driven the same");
    }

    std::printf ("== HPF: the split recombines transparently at min drive ==\n");
    {
        // The low band takes the subtractive-split path and the high band the
        // oversampled one; if their delays disagreed this would comb. A quiet
        // tone NEAR the split is the worst case.
        constexpr double sr = 48000.0;
        HarmonicCore core;
        core.setOversampling (4);
        core.prepare (sr, 256);
        core.setCurve (Curve::Soft);
        core.setDriveDb (0.0f);
        core.setHpfHz (300.0f);
        core.setMixPercent (100.0f);
        core.reset();

        const auto out = runTone (core, 300.0, sr, 0.05, 16384);
        const double lvl = toDb (magnitudeAt (out, 300.0, sr));
        check (near (lvl, toDb (0.05), 1.0),
               "a tone AT the split comes out at its own level ("
               + std::to_string (lvl - toDb (0.05)) + " dB error)");
    }

    // -----------------------------------------------------------------------
    std::printf ("== half-band: the two polyphase phases have EQUAL DC gain ==\n");
    {
        // Unequal phases would put a mirror image at Nyquist. Feed DC and check
        // both interpolated phases land on the same number.
        HalfBandUp2x up;
        up.reset();
        float e = 0.0f, o = 0.0f;
        for (int i = 0; i < 512; ++i) up.process (1.0f, e, o);
        check (near (e, 1.0, 1.0e-5), "even phase passes DC at unity (got " + std::to_string (e) + ")");
        check (near (o, 1.0, 1.0e-5), "odd phase passes DC at unity (got " + std::to_string (o) + ")");
    }

    std::printf ("== oversampler: reported latency, and a clean signal survives it ==\n");
    {
        check (Oversampler::latencyForFactor (1) == 0,  "1x adds no latency");
        check (Oversampler::latencyForFactor (2) == 30, "2x reports 30 samples");
        check (Oversampler::latencyForFactor (4) == 45, "4x reports 45 samples");
        check (Oversampler::latencyForFactor (8) == 48, "8x reports 48 samples (the short "
                                                        "stage adds a WHOLE 3)");

        // Up then straight back down, no shaping: the signal must come out
        // intact, delayed by exactly the reported amount.
        for (int factor : { 2, 4, 8 })
        {
            Oversampler ovs;
            ovs.prepare (factor, 512);

            const int lat = ovs.latencySamples();
            const int n   = 4096;
            std::vector<float> in ((std::size_t) n), out ((std::size_t) n);
            for (int i = 0; i < n; ++i)
                in[(std::size_t) i] = (float) std::sin (2.0 * kPi * 1000.0 * i / 48000.0);

            for (int off = 0; off < n; off += 512)
            {
                float* os = ovs.upsample (in.data() + off, 512);
                (void) os;                       // no shaping: a pure round trip
                ovs.downsample (out.data() + off, 512);
            }

            double worst = 0.0;
            for (int i = 2048; i < n - 8; ++i)
                worst = std::fmax (worst, std::fabs ((double) out[(std::size_t) i]
                                                   - (double) in[(std::size_t) (i - lat)]));
            check (worst < 2.0e-3,
                   std::to_string (factor) + "x round trip is transparent at 1 kHz, delayed by "
                   + std::to_string (lat) + " (worst error " + std::to_string (worst) + ")");
        }
    }

    // -----------------------------------------------------------------------
    // THE test. An 11 kHz tone at 48 kHz, hard driven. The 5th harmonic sits at
    // 55 kHz and folds to 48 - 55 + 48 ... concretely: 55 kHz mirrors about the
    // 48 kHz sample rate to 48 - (55 - 48) ... it lands at 7 kHz. Nothing
    // legitimate is at 7 kHz, so whatever is measured there is alias, full stop.
    // -----------------------------------------------------------------------
    std::printf ("== ALIAS SUPPRESSION: 11 kHz driven hard, measured at the fold ==\n");
    {
        constexpr double sr    = 48000.0;
        constexpr double tone  = 11000.0;
        constexpr double alias = 7000.0;      // 5 * 11k = 55k -> folds to 7k
        constexpr double alias3 = 15000.0;    // 3 * 11k = 33k -> folds to 15k
        const int n = 32768;

        auto measure = [&] (int factor, double& aliasDb, double& alias3Db, double& fundDb)
        {
            HarmonicCore core;
            core.setOversampling (factor);
            core.prepare (sr, 256);
            core.setCurve (Curve::Soft);       // symmetric: no 2nd harmonic to confuse things
            core.setDriveDb (30.0f);
            core.setToneDb (0.0f);
            core.setMixPercent (100.0f);
            core.setOutputDb (0.0f);
            core.reset();

            const auto out = runTone (core, tone, sr, 0.5, n);
            fundDb   = toDb (magnitudeAt (out, tone,   sr));
            aliasDb  = toDb (magnitudeAt (out, alias,  sr));
            alias3Db = toDb (magnitudeAt (out, alias3, sr));
        };

        double a1 = 0, a1b = 0, f1 = 0, a4 = 0, a4b = 0, f4 = 0;
        measure (1, a1, a1b, f1);
        measure (4, a4, a4b, f4);

        std::printf ("    1x: fundamental %.1f dB, 7 kHz fold %.1f dB, 15 kHz fold %.1f dB\n", f1, a1, a1b);
        std::printf ("    4x: fundamental %.1f dB, 7 kHz fold %.1f dB, 15 kHz fold %.1f dB\n", f4, a4, a4b);

        check (a1 > -60.0, "1x really does alias (the test would be vacuous otherwise): "
                           + std::to_string (a1) + " dB");
        check (a4 < a1 - 30.0, "4x suppresses the 5th-harmonic fold by >30 dB (by "
                               + std::to_string (a1 - a4) + " dB)");
        check (a4b < a1b - 20.0, "4x suppresses the 3rd-harmonic fold by >20 dB (by "
                                 + std::to_string (a1b - a4b) + " dB)");
        check (near (f4, f1, 3.0), "the fundamental is unchanged by oversampling ("
                                   + std::to_string (f1) + " vs " + std::to_string (f4) + " dB)");
    }

    // -----------------------------------------------------------------------
    // The LADDER: each advertised setting suppresses more than the one below
    // it. Moderate drive on purpose — a smoothly saturated tanh has a fast-
    // decaying harmonic series, which is exactly the regime where each
    // doubling of the rate pushes the first folding harmonic far up that
    // series. (Hard-driven the series approaches a square's 1/n and the steps
    // compress; the block above already covers that regime at 1x vs 4x.)
    // -----------------------------------------------------------------------
    std::printf ("== ALIAS LADDER: 2x < 4x < 8x, measured at the folds ==\n");
    {
        constexpr double sr   = 48000.0;
        constexpr double tone = 11000.0;
        // Every place an odd harmonic of 11 kHz can land in-band at SOME rate:
        // 1x: 7k (5th), 15k (3rd), 3k (9th); 2x: 19k (7th), 3k (9th);
        // 4x: 5k (17th), 17k (19th); 8x: 21k (33rd), 1k (35th).
        const double folds[] = { 1000.0, 3000.0, 5000.0, 7000.0,
                                 15000.0, 17000.0, 19000.0, 21000.0 };

        auto worstAlias = [&] (int factor)
        {
            HarmonicCore core;
            core.setOversampling (factor);
            core.prepare (sr, 256);
            core.setCurve (Curve::Soft);
            core.setDriveDb (12.0f);
            core.setMixPercent (100.0f);
            core.reset();

            const auto out  = runTone (core, tone, sr, 0.5, 32768);
            const double f  = toDb (magnitudeAt (out, tone, sr));
            double worst = -200.0;
            for (double a : folds)
                worst = std::max (worst, toDb (magnitudeAt (out, a, sr)) - f);
            return worst;
        };

        const double w1 = worstAlias (1);
        const double w2 = worstAlias (2);
        const double w4 = worstAlias (4);
        const double w8 = worstAlias (8);
        std::printf ("    worst in-band alias: 1x %.1f, 2x %.1f, 4x %.1f, 8x %.1f dB\n",
                     w1, w2, w4, w8);

        check (w1 > -40.0, "1x aliases audibly at 12 dB drive (the ladder has a bottom rung)");
        check (w2 < w1 - 8.0,  "2x beats 1x by " + std::to_string (w1 - w2) + " dB");
        check (w4 < w2 - 8.0,  "4x beats 2x by " + std::to_string (w2 - w4) + " dB");
        check (w8 < w4 - 3.0 || w8 < -90.0,
               "8x beats 4x by " + std::to_string (w4 - w8) + " dB (or both sit at the floor)");
    }

    // -----------------------------------------------------------------------
    std::printf ("== a LIVE factor change lands: latency retargets, audio stays sane ==\n");
    {
        constexpr double sr = 48000.0;
        HarmonicCore core;
        core.setOversampling (4);
        core.prepare (sr, 256);
        core.setCurve (Curve::Soft);
        core.setDriveDb (12.0f);
        core.setMixPercent (100.0f);
        core.reset();

        check (core.latencySamples() == 45, "prepared at 4x: 45 samples");

        std::vector<float> buf (256, 0.0f);
        long phase = 0;
        auto push = [&] { for (int i = 0; i < 256; ++i, ++phase)
                              buf[(std::size_t) i] = 0.3f * (float) std::sin (0.05 * (double) phase);
                          core.process (buf.data(), nullptr, 256); };

        for (int b = 0; b < 20; ++b) push();

        core.setOversampling (8);
        check (core.latencySamples() == 48, "switched to 8x WITHOUT re-prepare: 48 samples");
        bool finite = true;
        for (int b = 0; b < 20; ++b)
        {
            push();
            for (float v : buf) if (! std::isfinite (v)) finite = false;
        }

        core.setOversampling (2);
        check (core.latencySamples() == 30, "and down to 2x: 30 samples");
        for (int b = 0; b < 20; ++b)
        {
            push();
            for (float v : buf) if (! std::isfinite (v)) finite = false;
        }
        check (finite, "audio through both switches stays finite");
    }

    // -----------------------------------------------------------------------
    // The depth pass's core-level neutrality claim: a core left at the new
    // params' defaults is BIT-EXACTLY the core as it shipped. The registry test
    // proves the same thing again at device level, through processBlock.
    std::printf ("== the new params' defaults are bit-exact neutral ==\n");
    {
        constexpr double sr = 48000.0;

        auto render = [&] (bool touchDefaults)
        {
            HarmonicCore core;
            core.setOversampling (4);
            core.prepare (sr, 256);
            core.setCurve (Curve::Tube);
            core.setDriveDb (18.0f);
            core.setToneDb (3.0f);
            core.setMixPercent (70.0f);
            if (touchDefaults)
            {
                // Explicitly set every depth param to its neutral value.
                core.setEmphasis (Emphasis::Both);
                core.setBias (0.0f);
                core.setHpfHz (0.0f);
            }
            core.reset();
            return runTone (core, 220.0, sr, 0.4, 8192);
        };

        const auto a = render (false);
        const auto b = render (true);
        float worst = 0.0f;
        for (std::size_t i = 0; i < a.size(); ++i)
            worst = std::fmax (worst, std::fabs (a[i] - b[i]));
        check (worst == 0.0f, "defaults vs explicit-neutral: worst delta is EXACTLY 0 (got "
                              + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== the core is transparent at min drive, fully wet ==\n");
    {
        constexpr double sr = 48000.0;
        HarmonicCore core;
        core.setOversampling (4);
        core.prepare (sr, 256);
        core.setCurve (Curve::Soft);
        core.setDriveDb (0.0f);
        core.setToneDb (0.0f);
        core.setMixPercent (100.0f);
        core.setOutputDb (0.0f);
        core.reset();

        const auto out = runTone (core, 1000.0, sr, 0.05, 16384);
        const double lvl = toDb (magnitudeAt (out, 1000.0, sr));
        const double h3  = toDb (magnitudeAt (out, 3000.0, sr));
        check (near (lvl, toDb (0.05), 1.0), "a quiet tone comes out at its own level ("
                                             + std::to_string (lvl) + " dB)");
        check (h3 < lvl - 55.0, "and essentially undistorted (3rd harmonic "
                                + std::to_string (h3 - lvl) + " dB down)");
    }

    // -----------------------------------------------------------------------
    // The dry path has to be delayed by the same latency as the wet one, or a
    // partial mix comb-filters itself. At 0% wet the output must be the INPUT,
    // delayed by exactly the reported latency, and nothing else.
    std::printf ("== mix: the dry path is delay-matched to the wet path ==\n");
    {
        constexpr double sr = 48000.0;
        HarmonicCore core;
        core.setOversampling (4);
        core.prepare (sr, 256);
        core.setCurve (Curve::Tube);
        core.setDriveDb (24.0f);              // wet path hard driven, and unheard
        core.setMixPercent (0.0f);
        core.reset();

        const int n = 8192;
        const int lat = core.latencySamples();
        std::vector<float> in ((std::size_t) n), buf ((std::size_t) 256);
        for (int i = 0; i < n; ++i)
            in[(std::size_t) i] = (float) (0.5 * std::sin (2.0 * kPi * 997.0 * i / sr));

        std::vector<float> out ((std::size_t) n);
        for (int off = 0; off < n; off += 256)
        {
            for (int i = 0; i < 256; ++i) buf[(std::size_t) i] = in[(std::size_t) (off + i)];
            core.process (buf.data(), nullptr, 256);
            for (int i = 0; i < 256; ++i) out[(std::size_t) (off + i)] = buf[(std::size_t) i];
        }

        double worst = 0.0;
        for (int i = 2048; i < n; ++i)
            worst = std::fmax (worst, std::fabs ((double) out[(std::size_t) i]
                                               - (double) in[(std::size_t) (i - lat)]));
        check (lat == 45, "4x core reports 45 samples of latency");
        check (worst < 1.0e-4, "0% wet == the input delayed by exactly the reported latency "
                               "(worst error " + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== asymmetric curves do not leak DC ==\n");
    {
        constexpr double sr = 48000.0;
        HarmonicCore core;
        core.setOversampling (4);
        core.prepare (sr, 256);
        core.setCurve (Curve::Diode);         // the most lopsided curve we ship
        core.setDriveDb (24.0f);
        core.setMixPercent (100.0f);
        core.reset();

        const auto out = runTone (core, 220.0, sr, 0.7, 16384);
        double mean = 0.0;
        for (float v : out) mean += (double) v;
        mean /= (double) out.size();
        check (std::fabs (mean) < 5.0e-3, "mean of a hard-driven diode output is ~0 (got "
                                          + std::to_string (mean) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== tone tilt: 0 dB is exactly unity, and the sign is right ==\n");
    {
        constexpr double sr = 48000.0;

        auto toneAt = [&] (float tiltDb, double freq)
        {
            HarmonicCore core;
            core.setOversampling (2);
            core.prepare (sr, 256);
            core.setCurve (Curve::Soft);
            core.setDriveDb (0.0f);
            core.setToneDb (tiltDb);
            core.setMixPercent (100.0f);
            core.reset();
            const auto out = runTone (core, freq, sr, 0.02, 16384);
            return toDb (magnitudeAt (out, freq, sr));
        };

        const double flatLo = toneAt (0.0f, 100.0), flatHi = toneAt (0.0f, 8000.0);
        check (near (flatLo, flatHi, 0.2), "tone at 0 dB is flat across the band");

        const double upLo = toneAt (12.0f, 100.0), upHi = toneAt (12.0f, 8000.0);
        check (upHi - upLo > 9.0, "+12 tilt puts 8 kHz " + std::to_string (upHi - upLo)
                                  + " dB above 100 Hz");

        const double dnLo = toneAt (-12.0f, 100.0), dnHi = toneAt (-12.0f, 8000.0);
        check (dnLo - dnHi > 9.0, "-12 tilt puts 100 Hz " + std::to_string (dnLo - dnHi)
                                  + " dB above 8 kHz");
    }

    // -----------------------------------------------------------------------
    std::printf ("== drive compensates: character changes, loudness roughly does not ==\n");
    {
        constexpr double sr = 48000.0;

        auto rmsAtDrive = [&] (float driveDb)
        {
            HarmonicCore core;
            core.setOversampling (4);
            core.prepare (sr, 256);
            core.setCurve (Curve::Soft);
            core.setDriveDb (driveDb);
            core.setMixPercent (100.0f);
            core.reset();
            const auto out = runTone (core, 440.0, sr, 0.25, 16384);
            double acc = 0.0;
            for (float v : out) acc += (double) v * (double) v;
            return toDb (std::sqrt (acc / (double) out.size()));
        };

        const double r0 = rmsAtDrive (0.0f);
        const double r12 = rmsAtDrive (12.0f);
        const double r24 = rmsAtDrive (24.0f);
        std::printf ("    RMS: 0 dB drive %.2f, 12 dB %.2f, 24 dB %.2f\n", r0, r12, r24);
        check (std::fabs (r12 - r0) < 5.0, "12 dB of drive moves RMS by <5 dB");
        check (std::fabs (r24 - r0) < 7.0, "24 dB of drive moves RMS by <7 dB");
    }

    // -----------------------------------------------------------------------
    std::printf ("== a drive move RAMPS: no step at a block boundary ==\n");
    {
        constexpr double sr = 48000.0;
        HarmonicCore core;
        core.setOversampling (4);
        core.prepare (sr, 128);
        core.setCurve (Curve::Soft);
        core.setDriveDb (0.0f);
        core.setMixPercent (100.0f);
        core.reset();

        std::vector<float> buf (128);
        long phase = 0;
        auto fill = [&] { for (int i = 0; i < 128; ++i, ++phase)
                              buf[(std::size_t) i] = 0.3f; };   // DC: any wobble is the smoother

        for (int b = 0; b < 40; ++b) { fill(); core.process (buf.data(), nullptr, 128); }

        core.setDriveDb (36.0f);                                 // the biggest jump available

        double worst = 0.0;
        float prev = buf[127];
        for (int b = 0; b < 40; ++b)
        {
            fill();
            core.process (buf.data(), nullptr, 128);
            for (int i = 0; i < 128; ++i)
            {
                worst = std::fmax (worst, std::fabs ((double) buf[(std::size_t) i] - (double) prev));
                prev = buf[(std::size_t) i];
            }
        }
        check (worst < 0.02, "no sample-to-sample jump above 0.02 across a 0 -> 36 dB drive move "
                             "(worst " + std::to_string (worst) + ")");
    }

    // -----------------------------------------------------------------------
    std::printf ("== a block longer than the prepared maximum is chunked, not trusted ==\n");
    {
        HarmonicCore core;
        core.setOversampling (4);
        core.prepare (48000.0, 64);            // promised 64
        core.setCurve (Curve::Soft);
        core.setDriveDb (12.0f);
        core.setMixPercent (100.0f);
        core.reset();

        std::vector<float> buf (1024, 0.1f);
        core.process (buf.data(), nullptr, 1024);   // handed 1024

        bool finite = true;
        for (float v : buf) if (! std::isfinite (v)) finite = false;
        check (finite, "a 1024-sample block through a core prepared for 64 stays finite");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
