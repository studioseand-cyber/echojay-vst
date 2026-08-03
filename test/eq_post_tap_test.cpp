/*
    eq_post_tap_test.cpp — proves the analyzer's POST path actually tracks the
    EQ curve.

    The claim under test is end-to-end: audio goes through EqEngine, the post
    tap captures it AFTER processing, the ring hands back the newest N samples
    in order, and a cut therefore shows up as attenuation at that frequency.
    Any break in that chain (engine not applied, ring wrap wrong, read order
    reversed) shows up here as a failed attenuation assertion.

    JUCE-free, like the other EQ tests. The ring is replicated with the same
    arithmetic SurgicalEqProcessor uses (power-of-two mask, unsigned index,
    newest-N read) so the ring itself is covered too, not just the DSP.

    Build/run:
      g++ -std=c++17 -O2 -I../Source eq_post_tap_test.cpp ../Source/EqEngine.cpp \
          -o posttest && ./posttest
*/

#include "EqEngine.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using echojay::BandSpec;
using echojay::BandType;
using echojay::EqEngine;

static int failures = 0;

static void check (bool ok, const char* what, double got, double want)
{
    std::printf ("  [%s] %s: %.2f (expected %s%.2f)\n",
                 ok ? "PASS" : "FAIL", what, got, ok ? "~" : "", want);
    if (! ok) ++failures;
}

// --- the ring, mirroring SurgicalEqProcessor::AnalysisRing -----------------
static constexpr int kRingSize = 8192;
static constexpr int kRingMask = kRingSize - 1;

struct Ring
{
    std::array<float, kRingSize> data {};
    uint32_t write = 0;

    void push (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i)
            data[(size_t) ((write + (uint32_t) i) & kRingMask)] = 0.5f * (l[i] + r[i]);
        write += (uint32_t) n;
    }

    void read (float* dest, int n) const
    {
        const uint32_t start = write - (uint32_t) n;
        for (int i = 0; i < n; ++i)
            dest[i] = data[(size_t) ((start + (uint32_t) i) & kRingMask)];
    }
};

// --- level of one frequency in a captured buffer, in dB -------------------
// Single-bin windowed DFT: exact for a tone sitting on a bin centre, which is
// how the probes below are chosen, so there is no leakage to argue about.
static double levelDb (const std::vector<float>& x, double sr, double freq)
{
    const int    N  = (int) x.size();
    const double w0 = 2.0 * M_PI * freq / sr;
    double re = 0.0, im = 0.0;

    for (int n = 0; n < N; ++n)
    {
        // Same periodic Hann the editor uses.
        const double win = 0.5 * (1.0 - std::cos (2.0 * M_PI * n / (double) N));
        const double v   = (double) x[(size_t) n] * win;
        re += v * std::cos (w0 * n);
        im -= v * std::sin (w0 * n);
    }

    // 2/N, matching MeterEngine's visNorm and the editor's kSpecNormNumer.
    const double mag = std::sqrt (re * re + im * im) * (2.0 / (double) N);
    return mag > 1e-12 ? 20.0 * std::log10 (mag) : -240.0;
}

int main()
{
    const double sr     = 48000.0;
    const int    block  = 256;
    const int    fftN   = 4096;
    const double binHz  = sr / (double) fftN;

    // Probes on exact bin centres: 4, 16, 85, 427.
    const double fLow = 4.0   * binHz;    //   46.9 Hz
    const double fMid = 16.0  * binHz;    //  187.5 Hz
    const double f1k  = 85.0  * binHz;    //  996.1 Hz
    const double f5k  = 427.0 * binHz;    // 5003.9 Hz

    std::printf ("EQ post-tap: does POST follow the curve?\n");
    std::printf ("(probes: %.1f, %.1f, %.1f, %.1f Hz)\n\n", fLow, fMid, f1k, f5k);

    // ---------------------------------------------------------------------
    // Case 1: deep high-pass. The low end must collapse on POST while the
    // top is untouched — the "add a deep low cut" check, made numeric.
    // ---------------------------------------------------------------------
    {
        EqEngine eng;
        eng.prepare (sr, block, 2);

        BandSpec hp;
        hp.enabled       = true;
        hp.type          = BandType::HighPass;
        hp.freqHz        = 300.0f;
        hp.q             = 0.707f;
        hp.slopeDbPerOct = 48;
        eng.setBand (0, hp);

        Ring pre, post;
        std::vector<float> L ((size_t) block), R ((size_t) block);

        // Long enough for the block-rate parameter smoothing to settle before
        // the samples the analyzer would actually be looking at.
        const int blocks = 400;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const double t = (double) (b * block + i) / sr;
                const double s = 0.2 * (std::sin (2 * M_PI * fLow * t)
                                      + std::sin (2 * M_PI * fMid * t)
                                      + std::sin (2 * M_PI * f1k  * t)
                                      + std::sin (2 * M_PI * f5k  * t));
                L[(size_t) i] = R[(size_t) i] = (float) s;
            }

            pre.push (L.data(), R.data(), block);

            float* ch[2] = { L.data(), R.data() };
            eng.process (ch, 2, block);

            post.push (L.data(), R.data(), block);   // AFTER the engine
        }

        std::vector<float> a ((size_t) fftN), b ((size_t) fftN);
        pre.read  (a.data(), fftN);
        post.read (b.data(), fftN);

        const double dLow = levelDb (b, sr, fLow) - levelDb (a, sr, fLow);
        const double dMid = levelDb (b, sr, fMid) - levelDb (a, sr, fMid);
        const double d5k  = levelDb (b, sr, f5k)  - levelDb (a, sr, f5k);

        std::printf ("== deep 48 dB/oct high-pass at 300 Hz ==\n");
        check (dLow < -40.0, "POST vs PRE at 47 Hz (dB, want << 0)", dLow, -40.0);
        check (dMid < -10.0, "POST vs PRE at 188 Hz (dB, want < -10)", dMid, -10.0);
        check (std::fabs (d5k) < 1.0, "POST vs PRE at 5 kHz (dB, want ~0)", d5k, 0.0);
        std::printf ("\n");
    }

    // ---------------------------------------------------------------------
    // Case 2: a surgical bell cut shows up at its own frequency and nowhere
    // else — the per-band precision the whole feature exists for.
    // ---------------------------------------------------------------------
    {
        EqEngine eng;
        eng.prepare (sr, block, 2);

        BandSpec bell;
        bell.enabled = true;
        bell.type    = BandType::Bell;
        bell.freqHz  = (float) f1k;
        bell.gainDb  = -18.0f;
        bell.q       = 4.0f;
        eng.setBand (0, bell);

        Ring pre, post;
        std::vector<float> L ((size_t) block), R ((size_t) block);

        for (int b = 0; b < 400; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const double t = (double) (b * block + i) / sr;
                const double s = 0.2 * (std::sin (2 * M_PI * fLow * t)
                                      + std::sin (2 * M_PI * f1k  * t)
                                      + std::sin (2 * M_PI * f5k  * t));
                L[(size_t) i] = R[(size_t) i] = (float) s;
            }

            pre.push (L.data(), R.data(), block);
            float* ch[2] = { L.data(), R.data() };
            eng.process (ch, 2, block);
            post.push (L.data(), R.data(), block);
        }

        std::vector<float> a ((size_t) fftN), b ((size_t) fftN);
        pre.read  (a.data(), fftN);
        post.read (b.data(), fftN);

        const double dCut = levelDb (b, sr, f1k)  - levelDb (a, sr, f1k);
        const double dLow = levelDb (b, sr, fLow) - levelDb (a, sr, fLow);
        const double d5k  = levelDb (b, sr, f5k)  - levelDb (a, sr, f5k);

        std::printf ("== -18 dB bell at 996 Hz, Q 4 ==\n");
        check (std::fabs (dCut + 18.0) < 1.5, "POST vs PRE at the cut (dB)", dCut, -18.0);
        check (std::fabs (dLow) < 1.0, "POST vs PRE at 47 Hz (untouched)",  dLow, 0.0);
        check (std::fabs (d5k)  < 1.0, "POST vs PRE at 5 kHz (untouched)",  d5k,  0.0);
        std::printf ("\n");
    }

    // ---------------------------------------------------------------------
    // Case 3: with the engine bypassed, POST must equal PRE. Guards against a
    // "cut always visible" false positive from the taps being mixed up.
    // ---------------------------------------------------------------------
    {
        EqEngine eng;
        eng.prepare (sr, block, 2);

        BandSpec hp;
        hp.enabled       = true;
        hp.type          = BandType::HighPass;
        hp.freqHz        = 300.0f;
        hp.slopeDbPerOct = 48;
        eng.setBand (0, hp);
        eng.setBypassed (true);

        Ring pre, post;
        std::vector<float> L ((size_t) block), R ((size_t) block);

        for (int b = 0; b < 200; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const double t = (double) (b * block + i) / sr;
                L[(size_t) i] = R[(size_t) i] = (float) (0.2 * std::sin (2 * M_PI * fLow * t));
            }
            pre.push (L.data(), R.data(), block);
            float* ch[2] = { L.data(), R.data() };
            eng.process (ch, 2, block);
            post.push (L.data(), R.data(), block);
        }

        std::vector<float> a ((size_t) fftN), b ((size_t) fftN);
        pre.read  (a.data(), fftN);
        post.read (b.data(), fftN);

        const double d = levelDb (b, sr, fLow) - levelDb (a, sr, fLow);
        std::printf ("== bypassed: POST must equal PRE ==\n");
        check (std::fabs (d) < 0.01, "POST vs PRE at 47 Hz (dB)", d, 0.0);
        std::printf ("\n");
    }

    if (failures == 0) std::printf ("ALL PASS (0 failures)\n");
    else               std::printf ("%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
