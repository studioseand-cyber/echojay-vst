// Standalone test for PsolaEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source psola_engine_test.cpp -o psolatest && ./psolatest
//
// P1 acceptance (PITCH_CORRECTION_SPEC.md §9): TD-PSOLA to a fixed target with
// formants preserved, proven on SUSTAINED NOTES before any musical logic
// exists. Three things have to be true and each is measured rather than
// eyeballed:
//
//   1. the output is actually at the target pitch  - measured by running the
//      output back through PitchEngine, the same detector the plugin uses,
//   2. the FORMANTS DO NOT MOVE - measured against a resampler, which is the
//      thing PSOLA has to beat and the reason it earns its cost,
//   3. unvoiced and passthrough output is BIT-IDENTICAL to the delayed dry
//      input - spec §8, the rule that stops consonants being corrected.

#include "EedPsolaEngine.h"
#include "EedPitchEngine.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
static std::vector<float> saw (double fs, double f, double seconds, float amp = 0.4f)
{
    std::vector<float> v ((size_t) (fs * seconds));
    const int nh = std::max (1, (int) (0.4 * fs / f));
    for (size_t i = 0; i < v.size(); ++i)
    {
        double s = 0.0;
        for (int h = 1; h <= nh; ++h)
            s += std::sin (2.0 * M_PI * f * h * (double) i / fs) / h;
        v[i] = amp * (float) (s * (2.0 / M_PI));
    }
    return v;
}

// A glottal-ish source through a strong resonator: this is the signal that can
// tell PSOLA and a resampler apart, because it HAS a formant to move.
//
// The source is a band-limited IMPULSE TRAIN, not a sawtooth. A saw's 1/h roll-
// off makes its second harmonic louder than any reasonable resonance, so the
// spectral peak lands on the fundamental's neighbours and the measurement tells
// you nothing about formants. A flat harmonic spectrum lets the resonator BE
// the peak, which is what makes "did the formant move?" answerable.
static std::vector<float> impulseTrain (double fs, double f0, double seconds,
                                        float amp = 0.3f)
{
    std::vector<float> v ((size_t) (fs * seconds));
    const int nh = std::max (1, (int) (0.4 * fs / f0));
    for (size_t i = 0; i < v.size(); ++i)
    {
        double s = 0.0;
        for (int h = 1; h <= nh; ++h)
            s += std::cos (2.0 * M_PI * f0 * h * (double) i / fs);
        v[i] = amp * (float) (s / (double) nh);
    }
    return v;
}

static std::vector<float> vowel (double fs, double f0, double formantHz,
                                 double seconds, double q = 14.0)
{
    std::vector<float> v = impulseTrain (fs, f0, seconds);
    const double w0 = 2.0 * M_PI * formantHz / fs;
    const double alpha = std::sin (w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double b0 =  alpha / a0, b2 = -alpha / a0;
    const double a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - alpha) / a0;
    double z1 = 0.0, z2 = 0.0;
    for (auto& s : v)
    {
        const double x = s;
        const double y = b0 * x + z1;
        z1 = z2 - a1 * y;
        z2 = b2 * x - a2 * y;
        s = (float) (0.15 * x + 6.0 * y);    // a resonance that dominates the band
    }
    return v;
}

static std::vector<float> noise (double fs, double seconds, float amp, uint32_t seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    std::vector<float> v ((size_t) (fs * seconds));
    for (auto& s : v) s = amp * d (rng);
    return v;
}

// Naive resampling shift: the thing PSOLA must beat. Formants move WITH pitch.
static std::vector<float> resampleShift (const std::vector<float>& x, double ratio)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = 0; i < y.size(); ++i)
    {
        const double sp = (double) i * ratio;
        const size_t i0 = (size_t) sp;
        if (i0 + 1 >= x.size()) break;
        const double fr = sp - (double) i0;
        y[i] = (float) ((1.0 - fr) * x[i0] + fr * x[i0 + 1]);
    }
    return y;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------
// Goertzel magnitude at one frequency - enough to trace a spectral envelope
// without dragging in an FFT.
static double magAt (const std::vector<float>& x, double fs, double f,
                     size_t from, size_t len)
{
    if (from + len > x.size()) return 0.0;
    const double w = 2.0 * M_PI * f / fs;
    const double c = 2.0 * std::cos (w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < len; ++i)
    {
        s0 = (double) x[from + i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) / (double) len;
}

// Where is the spectral peak, searching a band? This is the formant tracker.
static double peakHz (const std::vector<float>& x, double fs,
                      double lo, double hi, size_t from, size_t len)
{
    double bestF = lo, bestM = -1.0;
    for (double f = lo; f <= hi; f *= 1.01)
    {
        const double m = magAt (x, fs, f, from, len);
        if (m > bestM) { bestM = m; bestF = f; }
    }
    return bestF;
}

// Run the shifter over a source, returning its output.
struct ShiftResult { std::vector<float> out; int latency = 0; };

static ShiftResult shift (const std::vector<float>& in, double fs,
                          float sourceF0, float targetHz, bool voiced = true,
                          float lowestF0 = 80.0f)
{
    PsolaEngine e;
    e.prepare (fs, 512, lowestF0, 25.0f);
    e.setTargetHz (targetHz);

    ShiftResult r;
    r.latency = e.latencySamples();
    r.out.assign (in.size(), 0.0f);

    constexpr int kBlock = 256;
    for (size_t p = 0; p + kBlock <= in.size(); p += kBlock)
        e.process (in.data() + p, r.out.data() + p, kBlock, sourceF0, voiced);
    return r;
}

// Detected pitch of a buffer, using the SAME detector the plugin runs.
static float detect (const std::vector<float>& x, double fs, int voiceType,
                     size_t skip)
{
    PitchEngine e;
    e.prepare (fs, 512);
    e.setVoiceType (voiceType);
    e.setTracking (PitchEngine::kRelaxed);

    std::vector<float> f0s;
    constexpr int kBlock = 512;
    for (size_t p = 0; p + kBlock <= x.size(); p += kBlock)
    {
        e.process (x.data() + p, nullptr, kBlock);
        const PitchReading r = e.getReading();
        if (r.voiced && p > skip) f0s.push_back (r.f0Hz);
    }
    if (f0s.empty()) return 0.0f;
    std::sort (f0s.begin(), f0s.end());
    return f0s[f0s.size() / 2];
}

int main()
{
    constexpr double fs = 48000.0;

    std::printf ("== passthrough (target 0) is the DELAYED DRY INPUT, bit for bit ==\n");
    {
        const auto in = saw (fs, 220.0, 1.0);
        const ShiftResult r = shift (in, fs, 220.0f, 0.0f);

        size_t compared = 0, differing = 0;
        for (size_t i = (size_t) r.latency; i + 512 < in.size(); ++i)
        {
            ++compared;
            if (r.out[i] != in[i - (size_t) r.latency]) ++differing;
        }
        char msg[160];
        std::snprintf (msg, sizeof (msg),
                       "%zu samples compared, %zu differ (latency %d)",
                       compared, differing, r.latency);
        check (compared > 20000 && differing == 0, msg);
    }

    std::printf ("== UNVOICED passes through untouched, even with a target set ==\n");
    {
        // Spec §8: the consonant null. A shifter that touches unvoiced frames
        // is the thing that makes cheap correctors sound cheap.
        const auto in = noise (fs, 1.0, 0.3f, 99);
        const ShiftResult r = shift (in, fs, 0.0f, 440.0f, /*voiced*/ false);

        size_t compared = 0, differing = 0;
        for (size_t i = (size_t) r.latency; i + 512 < in.size(); ++i)
        {
            ++compared;
            if (r.out[i] != in[i - (size_t) r.latency]) ++differing;
        }
        char msg[160];
        std::snprintf (msg, sizeof (msg), "%zu unvoiced samples compared, %zu differ",
                       compared, differing);
        check (compared > 20000 && differing == 0, msg);
    }

    std::printf ("== the output really is at the target pitch (sustained notes) ==\n");
    {
        struct Case { double src, dst; int vt; float lowest; };
        const Case cases[] = {
            { 220.0, 440.0, PitchEngine::kAltoTenor,  80.0f },   // up an octave
            { 220.0, 110.0, PitchEngine::kAltoTenor,  80.0f },   // down an octave
            { 220.0, 277.2, PitchEngine::kAltoTenor,  80.0f },   // up a major third
            { 330.0, 220.0, PitchEngine::kAltoTenor,  80.0f },   // down a fifth
            { 110.0, 146.8, PitchEngine::kLowMale,    55.0f },   // low male, up a fourth
        };
        for (const auto& c : cases)
        {
            const auto in = saw (fs, c.src, 1.5);
            const ShiftResult r = shift (in, fs, (float) c.src, (float) c.dst,
                                         true, c.lowest);
            const float got = detect (r.out, fs, c.vt, (size_t) (fs * 0.3));
            const float cents = got > 0 ? PitchEngine::centsBetween (got, (float) c.dst)
                                        : 9999.0f;
            char msg[180];
            std::snprintf (msg, sizeof (msg),
                           "%.1f -> %.1f Hz: detected %.2f Hz (%+.1f cents)",
                           c.src, c.dst, got, cents);
            check (std::fabs (cents) < 35.0f, msg);
        }
    }

    std::printf ("== FORMANTS STAY PUT - the reason PSOLA earns its cost ==\n");
    {
        // A vowel at 150 Hz with a strong 900 Hz formant, shifted up an octave.
        // PSOLA should leave the formant near 900; a resampler drags it to
        // ~1800 (chipmunk). If these two are not clearly different, the
        // implementation is not doing what it claims.
        const double f0 = 150.0, formant = 900.0;
        const auto in = vowel (fs, f0, formant, 1.5);

        const ShiftResult r = shift (in, fs, (float) f0, (float) (f0 * 2.0), true, 80.0f);

        const size_t from = (size_t) (fs * 0.6), len = (size_t) (fs * 0.5);
        const double srcPeak    = peakHz (in,    fs, 300.0, 3000.0, from, len);
        const double psolaPeak  = peakHz (r.out, fs, 300.0, 3000.0, from, len);

        const auto resampled = resampleShift (in, 2.0);
        const double resPeak  = peakHz (resampled, fs, 300.0, 3000.0, from, len);

        char msg[200];
        std::snprintf (msg, sizeof (msg),
                       "source formant %.0f Hz -> PSOLA %.0f Hz, resampler %.0f Hz",
                       srcPeak, psolaPeak, resPeak);
        std::printf ("  %s\n", msg);

        // Assert against the KNOWN resonator frequency, not the measured source
        // peak: the source is a harmonic series, so its measured peak can land
        // on whichever harmonic sits nearest the resonance and that wobble
        // would weaken the claim. `formant` is the ground truth here.
        check (std::fabs (psolaPeak / formant - 1.0) < 0.25,
               "PSOLA leaves the formant within 25% of its true 900 Hz");
        check (resPeak > formant * 1.6,
               "the resampler drags it up with the pitch - the two are clearly different");
        check (resPeak > psolaPeak * 1.5,
               "...and the gap between them is unambiguous");
    }

    std::printf ("== the shift is stable, not just correct on average ==\n");
    {
        // A wandering output pitch reads as warble even when the median is
        // right, so spread matters as much as centre.
        const auto in = saw (fs, 220.0, 2.0);
        const ShiftResult r = shift (in, fs, 220.0f, 330.0f);

        PitchEngine e;
        e.prepare (fs, 512);
        e.setVoiceType (PitchEngine::kAltoTenor);
        e.setTracking (PitchEngine::kRelaxed);

        std::vector<float> f0s;
        constexpr int kBlock = 512;
        for (size_t p = 0; p + kBlock <= r.out.size(); p += kBlock)
        {
            e.process (r.out.data() + p, nullptr, kBlock);
            const PitchReading rd = e.getReading();
            if (rd.voiced && p > (size_t) (fs * 0.4)) f0s.push_back (rd.f0Hz);
        }
        check (! f0s.empty(), "the shifted signal is detectable at all");
        if (! f0s.empty())
        {
            std::sort (f0s.begin(), f0s.end());
            const float p05 = f0s[f0s.size() / 20];
            const float p95 = f0s[f0s.size() * 19 / 20];
            const float spread = std::fabs (PitchEngine::centsBetween (p95, p05));
            char msg[160];
            std::snprintf (msg, sizeof (msg),
                           "5th-95th percentile spread %.1f cents (want < 60)", spread);
            check (spread < 60.0f, msg);
        }
    }

    std::printf ("== level is preserved: normalised overlap-add does not breathe ==\n");
    {
        auto rms = [] (const std::vector<float>& x, size_t from, size_t len)
        {
            double a = 0.0;
            for (size_t i = from; i < from + len && i < x.size(); ++i) a += (double) x[i] * x[i];
            return std::sqrt (a / (double) len);
        };
        const auto in = saw (fs, 220.0, 1.5);
        const size_t from = (size_t) (fs * 0.6), len = (size_t) (fs * 0.5);
        const double dry = rms (in, from, len);

        // Two bars, because they are two different claims. Inside a REAL
        // correction range - a few semitones, which is all a corrector ever
        // asks for - level must be tight. At extreme ratios coherent copies of
        // the same pulse partially cancel and some loss is inherent to
        // TD-PSOLA; that is allowed, but it is bounded and stated rather than
        // hidden. P1's fixed target is what makes octave moves reachable at
        // all; P2 onward never asks for one.
        struct LevelCase { double target; double tolDb; const char* what; };
        const LevelCase cases[] = {
            { 196.0, 1.5, "down a whole tone" },
            { 208.0, 1.5, "down a semitone"   },
            { 220.0, 0.5, "unison"            },
            { 233.1, 1.5, "up a semitone"     },
            { 261.6, 1.5, "up a minor third"  },
            { 165.0, 2.0, "down a fourth"     },
            { 330.0, 2.0, "up a fifth"        },
            { 440.0, 3.5, "up an OCTAVE - extreme for a corrector" },
            { 110.0, 3.5, "down an OCTAVE - extreme for a corrector" },
        };
        for (const auto& c : cases)
        {
            const ShiftResult r = shift (in, fs, 220.0f, (float) c.target);
            const double wet = rms (r.out, from, len);
            const double db  = 20.0 * std::log10 (std::max (1e-9, wet / dry));
            char msg[200];
            std::snprintf (msg, sizeof (msg),
                           "220 -> %.1f Hz (%s): %+.2f dB, tol %.1f", c.target, c.what, db, c.tolDb);
            check (std::fabs (db) < c.tolDb, msg);
        }
    }

    std::printf ("== nothing pathological: no NaN, no runaway ==\n");
    {
        for (double t : { 40.0, 1600.0 })
        {
            const auto in = saw (fs, 220.0, 0.6);
            const ShiftResult r = shift (in, fs, 220.0f, (float) t);
            bool bad = false;
            float peak = 0.0f;
            for (float s : r.out)
            {
                if (! std::isfinite (s)) bad = true;
                peak = std::max (peak, std::fabs (s));
            }
            char msg[160];
            std::snprintf (msg, sizeof (msg),
                           "extreme target %.0f Hz: finite, peak %.2f", t, peak);
            check (! bad && peak < 4.0f, msg);
        }
    }

    std::printf ("== latency is reported, and tracks the voice type's floor ==\n");
    {
        int last = 0;
        for (float lowest : { 180.0f, 80.0f, 55.0f, 25.0f })
        {
            PsolaEngine e;
            e.prepare (fs, 512, lowest, 25.0f);
            const int lat = e.latencySamples();
            const double ms = 1000.0 * lat / fs;
            char msg[160];
            std::snprintf (msg, sizeof (msg),
                           "floor %.0f Hz -> latency %d samples (%.1f ms)", lowest, lat, ms);
            check (lat > 0 && lat >= last, msg);
            last = lat;
        }
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
