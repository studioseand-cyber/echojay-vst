// Standalone test for PitchEngine (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source pitch_engine_test.cpp -o pitchtest && ./pitchtest
//
// P0 acceptance (PITCH_CORRECTION_SPEC.md §9): the YIN detector is proven
// across all five voice_type settings, the octave-error guard is exercised on
// the classic missing-fundamental trap, and the octave-error rate is logged.
// Real vocals cannot ship in a unit test; the vocal-shaped stand-ins here are
// sawtooth (harmonically rich, glottal-like), vibrato, and breathy (saw+noise).

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
// Signal generators (deterministic).
// ---------------------------------------------------------------------------
static std::vector<float> sine (double fs, double f, double seconds, float amp = 0.5f)
{
    std::vector<float> v ((size_t) (fs * seconds));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = amp * (float) std::sin (2.0 * M_PI * f * (double) i / fs);
    return v;
}

// Band-limited-ish sawtooth: harmonics to 40% of fs. Rich spectrum, the
// closest simple stand-in for a glottal source.
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

static std::vector<float> vibratoSine (double fs, double f, double seconds,
                                       double depthCents, double rateHz, float amp = 0.5f)
{
    std::vector<float> v ((size_t) (fs * seconds));
    double phase = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double cents = depthCents * std::sin (2.0 * M_PI * rateHz * (double) i / fs);
        const double fi = f * std::pow (2.0, cents / 1200.0);
        phase += 2.0 * M_PI * fi / fs;
        v[i] = amp * (float) std::sin (phase);
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

static void mix (std::vector<float>& into, const std::vector<float>& add)
{
    for (size_t i = 0; i < into.size() && i < add.size(); ++i) into[i] += add[i];
}

// ---------------------------------------------------------------------------
// Drive the engine block-wise; collect one reading per block.
// ---------------------------------------------------------------------------
struct RunStats
{
    int   blocks = 0, voicedBlocks = 0;
    float lastF0 = 0.0f, lastConf = 0.0f;
    float maxAbsCentsFromTarget = 0.0f;   // over voiced blocks in the scored region
    float medianF0 = 0.0f;
    PitchReading final;
};

static RunStats run (PitchEngine& e, const std::vector<float>& sig,
                     float targetHz = 0.0f, double scoreFromSeconds = 0.25,
                     double fs = 48000.0)
{
    constexpr int kBlock = 512;
    RunStats st;
    std::vector<float> f0s;
    for (size_t pos = 0; pos + kBlock <= sig.size(); pos += kBlock)
    {
        e.process (sig.data() + pos, nullptr, kBlock);
        const PitchReading r = e.getReading();
        ++st.blocks;
        if (r.voiced)
        {
            ++st.voicedBlocks;
            st.lastF0 = r.f0Hz; st.lastConf = r.confidence;
            if (targetHz > 0.0f && (double) pos / fs >= scoreFromSeconds)
            {
                f0s.push_back (r.f0Hz);
                const float c = std::fabs (PitchEngine::centsBetween (r.f0Hz, targetHz));
                st.maxAbsCentsFromTarget = std::max (st.maxAbsCentsFromTarget, c);
            }
        }
    }
    if (! f0s.empty())
    {
        std::sort (f0s.begin(), f0s.end());
        st.medianF0 = f0s[f0s.size() / 2];
    }
    st.final = e.getReading();
    return st;
}

static void initEngine (PitchEngine& e, int voiceType, double fs = 48000.0)
{
    e.prepare (fs, 512);
    e.setVoiceType (voiceType);
}

int main()
{
    std::printf ("== pure tones: accuracy across ALL FIVE voice_type settings ==\n");
    {
        struct Case { int type; double f; };
        const Case cases[] = {
            { PitchEngine::kSoprano,    260.0 }, { PitchEngine::kSoprano,    523.25 },
            { PitchEngine::kSoprano,   1046.5 },
            { PitchEngine::kAltoTenor,  110.0 }, { PitchEngine::kAltoTenor,  261.63 },
            { PitchEngine::kAltoTenor,  660.0 },
            { PitchEngine::kLowMale,     82.41 }, { PitchEngine::kLowMale,   146.83 },
            { PitchEngine::kLowMale,    392.0 },
            { PitchEngine::kInstrument,  65.41 }, { PitchEngine::kInstrument, 440.0 },
            { PitchEngine::kInstrument, 1568.0 },
            { PitchEngine::kBass,        30.87 }, { PitchEngine::kBass,        55.0 },
            { PitchEngine::kBass,       196.0 },
        };
        for (const auto& c : cases)
        {
            PitchEngine e; initEngine (e, c.type);
            const RunStats st = run (e, sine (48000.0, c.f, 1.0), (float) c.f);
            const float centsErr = st.medianF0 > 0
                ? std::fabs (PitchEngine::centsBetween (st.medianF0, (float) c.f)) : 999.0f;
            // Cents resolution shrinks with the lag: below ~16 analysis samples
            // per period the parabolic fit is the limit, not the estimator.
            const float tol = (float) (e.analysisRate() / c.f) < 16.0f ? 15.0f : 6.0f;
            char msg[160];
            std::snprintf (msg, sizeof (msg),
                           "%s @ %.2f Hz -> %.2f Hz (err %.1f c, conf %.2f, tol %.0f c)",
                           PitchEngine::voiceRange (c.type).id, c.f, st.medianF0,
                           centsErr, st.lastConf, tol);
            check (st.voicedBlocks > st.blocks / 2 && centsErr <= tol, msg);
            check (st.lastConf >= 0.85f, std::string (PitchEngine::voiceRange (c.type).id)
                                          + " confidence high on a clean tone");
        }
    }

    std::printf ("== sawtooth (vocal-shaped spectrum): fundamental wins, not a harmonic ==\n");
    {
        struct Case { int type; double f; };
        const Case cases[] = {
            { PitchEngine::kAltoTenor, 220.0 },
            { PitchEngine::kLowMale,   110.0 },
            { PitchEngine::kBass,       41.2 },
        };
        for (const auto& c : cases)
        {
            PitchEngine e; initEngine (e, c.type);
            const RunStats st = run (e, saw (48000.0, c.f, 1.0), (float) c.f);
            const float centsErr = st.medianF0 > 0
                ? std::fabs (PitchEngine::centsBetween (st.medianF0, (float) c.f)) : 999.0f;
            char msg[160];
            std::snprintf (msg, sizeof (msg), "saw %s @ %.1f Hz -> %.2f Hz (err %.1f c)",
                           PitchEngine::voiceRange (c.type).id, c.f, st.medianF0, centsErr);
            check (centsErr <= 8.0f, msg);
        }
    }

    std::printf ("== sample-rate independence: A440 at 44.1k / 48k / 96k ==\n");
    {
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            PitchEngine e; initEngine (e, PitchEngine::kAltoTenor, fs);
            const RunStats st = run (e, sine (fs, 440.0, 1.0), 440.0f, 0.25, fs);
            const float centsErr = st.medianF0 > 0
                ? std::fabs (PitchEngine::centsBetween (st.medianF0, 440.0f)) : 999.0f;
            char msg[128];
            std::snprintf (msg, sizeof (msg), "fs %.0f -> %.2f Hz (err %.1f c)",
                           fs, st.medianF0, centsErr);
            check (centsErr <= 5.0f, msg);
        }
    }

    std::printf ("== voiced / unvoiced: silence and noise must NOT read as pitch ==\n");
    {
        PitchEngine e; initEngine (e, PitchEngine::kAltoTenor);
        RunStats st = run (e, std::vector<float> (48000, 0.0f));
        check (st.voicedBlocks == 0, "silence: zero voiced blocks");
        check (st.final.rmsDb <= -100.0f, "silence: RMS reads the floor");

        PitchEngine e2; initEngine (e2, PitchEngine::kAltoTenor);
        st = run (e2, noise (48000.0, 1.0, 0.3f, 1234));
        char msg[128];
        std::snprintf (msg, sizeof (msg), "white noise: voiced on %d/%d blocks (want few)",
                       st.voicedBlocks, st.blocks);
        check (st.voicedBlocks <= st.blocks / 10, msg);

        // Breathy: saw + noise at ~8 dB SNR still reads as the pitch.
        PitchEngine e3; initEngine (e3, PitchEngine::kAltoTenor);
        auto breathy = saw (48000.0, 220.0, 1.0, 0.4f);
        mix (breathy, noise (48000.0, 1.0, 0.1f, 99));
        st = run (e3, breathy, 220.0f);
        const float centsErr = st.medianF0 > 0
            ? std::fabs (PitchEngine::centsBetween (st.medianF0, 220.0f)) : 999.0f;
        std::snprintf (msg, sizeof (msg),
                       "breathy saw 220 -> %.2f Hz (err %.1f c, voiced %d/%d)",
                       st.medianF0, centsErr, st.voicedBlocks, st.blocks);
        check (st.voicedBlocks > st.blocks / 2 && centsErr <= 20.0f, msg);
    }

    std::printf ("== confidence orders itself: clean > breathy > noise ==\n");
    {
        PitchEngine a; initEngine (a, PitchEngine::kAltoTenor);
        const float clean = run (a, sine (48000.0, 220.0, 1.0)).lastConf;
        PitchEngine b; initEngine (b, PitchEngine::kAltoTenor);
        auto breathy = sine (48000.0, 220.0, 1.0);
        mix (breathy, noise (48000.0, 1.0, 0.15f, 7));
        const float dirty = run (b, breathy).lastConf;
        char msg[128];
        std::snprintf (msg, sizeof (msg), "clean %.3f > breathy %.3f", clean, dirty);
        check (clean > dirty && dirty > 0.0f, msg);
    }

    std::printf ("== THE OCTAVE TRAP: dominant 2nd harmonic must not read an octave up ==\n");
    {
        // 0.15 * f + 1.0 * 2f: YIN's first sub-threshold dip is at the HALF
        // period — the classic doubling error. The guard's continuity bias +
        // candidate re-score must hold the true fundamental.
        PitchEngine e; initEngine (e, PitchEngine::kAltoTenor);
        auto sig = sine (48000.0, 220.0, 0.6f, 0.5f);            // establish 220
        auto trap = sine (48000.0, 440.0, 0.5f, 0.5f);           // dominant octave-up
        mix (trap, sine (48000.0, 220.0, 0.5f, 0.075f));         // weak fundamental
        sig.insert (sig.end(), trap.begin(), trap.end());

        const uint32_t firesBefore = [&] { run (e, sig, 220.0f, 0.25); return e.getReading().guardFires; }();
        const PitchReading r = e.getReading();
        char msg[160];
        std::snprintf (msg, sizeof (msg),
                       "held %.2f Hz through the trap (want ~220), guard fired %u times",
                       r.f0Hz, firesBefore);
        check (r.voiced && std::fabs (PitchEngine::centsBetween (r.f0Hz, 220.0f)) < 100.0f, msg);
        check (firesBefore > 0, "guard counter logged the trap");
    }

    std::printf ("== vibrato: tracks, stays in the right octave, guard stays quiet ==\n");
    {
        PitchEngine e; initEngine (e, PitchEngine::kAltoTenor);
        const RunStats st = run (e, vibratoSine (48000.0, 220.0, 1.5, 60.0, 5.5), 220.0f);
        char msg[160];
        std::snprintf (msg, sizeof (msg),
                       "vibrato +/-60c: max dev %.1f c (want < 130), voiced %d/%d, fires %u",
                       st.maxAbsCentsFromTarget, st.voicedBlocks, st.blocks,
                       st.final.guardFires);
        check (st.voicedBlocks > st.blocks * 3 / 4
                 && st.maxAbsCentsFromTarget < 130.0f, msg);
        check (st.final.guardFires <= (uint32_t) std::max (1, (int) st.final.voicedHops / 20),
               "guard fires on < 5% of voiced hops under vibrato");
    }

    std::printf ("== a real octave JUMP is followed, not fought ==\n");
    {
        PitchEngine e; initEngine (e, PitchEngine::kAltoTenor);
        auto sig = sine (48000.0, 220.0, 0.75);
        auto up  = sine (48000.0, 440.0, 0.75);
        sig.insert (sig.end(), up.begin(), up.end());
        const RunStats st = run (e, sig);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "after a genuine 220->440 jump reads %.2f Hz", st.lastF0);
        check (std::fabs (PitchEngine::centsBetween (st.lastF0, 440.0f)) < 50.0f, msg);
    }

    std::printf ("== voice_type switch mid-stream reconfigures and recovers ==\n");
    {
        PitchEngine e; initEngine (e, PitchEngine::kBass);
        run (e, sine (48000.0, 55.0, 0.75), 55.0f);
        check (std::fabs (PitchEngine::centsBetween (e.getReading().f0Hz, 55.0f)) < 15.0f,
               "bass reads 55 Hz");
        e.setVoiceType (PitchEngine::kSoprano);
        const RunStats st = run (e, sine (48000.0, 880.0, 0.75), 880.0f);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "after switch to soprano reads %.2f Hz", st.lastF0);
        check (std::fabs (PitchEngine::centsBetween (st.lastF0, 880.0f)) < 15.0f, msg);
    }

    std::printf ("== stats reset ==\n");
    {
        PitchEngine e; initEngine (e, PitchEngine::kAltoTenor);
        run (e, sine (48000.0, 220.0, 0.5));
        check (e.getReading().totalHops > 0, "hops counted");
        e.resetStats();
        const PitchReading r = e.getReading();
        check (r.totalHops == 0 && r.voicedHops == 0 && r.guardFires == 0,
               "reset_stats zeroes all three counters");
    }

    std::printf ("== note formatting ==\n");
    {
        char buf[8]; float cents = 0.0f;
        PitchEngine::noteName (440.0f, buf, sizeof (buf), &cents);
        check (std::string (buf) == "A4" && std::fabs (cents) < 0.01f, "440 -> A4 +0c");
        PitchEngine::noteName (446.0f, buf, sizeof (buf), &cents);
        check (std::string (buf) == "A4" && cents > 20.0f && cents < 30.0f, "446 -> A4 ~+23c");
        PitchEngine::noteName (261.63f, buf, sizeof (buf), &cents);
        check (std::string (buf) == "C4" && std::fabs (cents) < 1.0f, "261.63 -> C4");
    }

    // The P0 log the spec asks for: octave-guard rate per voice type on
    // vibrato material (fires-constantly means the WINDOW is wrong).
    std::printf ("== octave-guard fire-rate log (vibrato saw, per voice type) ==\n");
    for (int t = 0; t < PitchEngine::kNumVoiceTypes; ++t)
    {
        const auto& vr = PitchEngine::voiceRange (t);
        const double f = std::sqrt ((double) vr.fMinHz * vr.fMaxHz);   // geometric mid
        PitchEngine e; initEngine (e, t);
        run (e, vibratoSine (48000.0, f, 1.5, 40.0, 5.5), (float) f);
        const PitchReading r = e.getReading();
        const double rate = r.voicedHops > 0 ? 100.0 * r.guardFires / r.voicedHops : 0.0;
        std::printf ("  %-11s mid %6.1f Hz: %u fires / %u voiced hops (%.1f%%)\n",
                     vr.id, f, r.guardFires, r.voicedHops, rate);
        check (rate < 10.0, std::string (vr.id) + " guard rate under 10% on in-range material");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
