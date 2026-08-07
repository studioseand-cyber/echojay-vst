// Passive-detection CPU bench (KEY_PRECONDITION_SPEC.md §5.1). Build:
//   g++ -std=c++17 -O2 -I../Source key_passive_bench.cpp ../Source/EedKeyEngine.cpp \
//       -o keybench && ./keybench
//
// Measures the two costs the Link's passive key detection adds on top of its
// existing metering, and expresses them against the duty cycle the scheduler
// actually runs (an 8 s committed pass every 30 s, silence skipped):
//
//   1. the audio-thread tap  — pushBlock(): stereo downmix, two biquads,
//      decimate, ring write. Paid on every block while Active.
//   2. the analysis pass     — one committed 8 s window (HPSS on), run on the
//      worker thread. Paid at most once per 30 s.
//
// The number that matters is the duty-cycled average: pass_ms / 30 000 ms,
// plus the tap as a % of real time. Not a pass/fail suite — a measurement.

#include "EedKeyEngine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace echojay;
using Clock = std::chrono::steady_clock;

static constexpr double kFs = 48000.0;

// Harmonically dense fake mix: chords + bass + hats-ish noise, so HPSS and
// the constant-Q sweep do realistic work rather than idling on a sine.
static std::vector<float> makeMix (double seconds)
{
    std::vector<float> buf ((size_t) (seconds * kFs), 0.0f);
    unsigned s = 777u;
    auto rnd = [&s]() {
        s = s * 1664525u + 1013904223u;
        return (float) ((double) (s >> 8) / 8388608.0 - 1.0);
    };
    const int chord[4] = { 48, 55, 60, 64 };
    for (int c = 0; c < 4; ++c)
    {
        const double f = 440.0 * std::pow (2.0, (chord[c] - 69) / 12.0);
        for (int h = 1; h <= 6; ++h)
        {
            const double w = 2.0 * 3.14159265358979323846 * f * h / kFs;
            if (f * h >= kFs * 0.45) break;
            for (size_t i = 0; i < buf.size(); ++i)
                buf[i] += (float) std::sin (w * (double) i) * (0.15f / (float) h);
        }
    }
    for (auto& v : buf) v += rnd() * 0.05f;   // broadband "percussion" bed
    return buf;
}

int main()
{
    const auto mix = makeMix (10.0);

    // ---- 1. the audio-thread tap ------------------------------------------
    // 60 s of audio in 512-sample blocks, the per-block call the Link makes.
    {
        KeyEngine e;
        e.prepare (kFs, 512);
        const int blocks = (int) (60.0 * kFs) / 512;
        const auto t0 = Clock::now();
        int pos = 0;
        for (int b = 0; b < blocks; ++b)
        {
            if (pos + 512 > (int) mix.size()) pos = 0;
            e.pushBlock (mix.data() + pos, mix.data() + pos, 512);
            pos += 512;
        }
        const double ms = std::chrono::duration<double, std::milli> (Clock::now() - t0).count();
        std::printf ("tap  : %7.2f ms per 60 s of 48 kHz stereo audio  ->  %.4f %% of real time\n",
                     ms, 100.0 * ms / 60000.0);
    }

    // ---- 2. one committed 8 s pass (HPSS on), median of 5 -----------------
    {
        double bestMs = 1.0e9;
        double passMs[5];
        for (int rep = 0; rep < 5; ++rep)
        {
            KeyEngine e;
            e.prepare (kFs, 512);
            e.setWindowSeconds (8.0f);
            e.setLiveChromaEnabled (false);   // the Link's setting
            e.startAnalysis();
            // Feed the whole window without consuming (the tap ring holds
            // ~32 s decimated), then time the ONE update() that drains it and
            // runs the analysis — drain + pass is the real worker-thread cost.
            int pos = 0;
            const int need = (int) (8.6 * kFs);
            while (pos < need)
            {
                const int m = std::min (512, need - pos);
                e.pushBlock (mix.data() + (pos % ((int) mix.size() - 512)),
                             nullptr, m);
                pos += m;
            }
            const auto t0 = Clock::now();
            e.update();                                   // drain + the pass
            const double ms = std::chrono::duration<double, std::milli> (Clock::now() - t0).count();
            passMs[rep] = ms;
            if (ms < bestMs) bestMs = ms;
            if (! e.getReading().valid && e.isCollecting())
                std::printf ("  (warn: pass %d did not complete in the timed update)\n", rep);
        }
        std::printf ("pass : %7.2f ms for one committed 8 s window (best of 5: %.2f)\n",
                     passMs[2], bestMs);
        std::printf ("duty : one pass / 30 s  ->  %.4f %% of one core (using median-ish rep)\n",
                     100.0 * passMs[2] / 30000.0);
    }

    std::printf ("\nADDED CPU (tap %% realtime + duty-cycled pass) is the number to report.\n");
    return 0;
}
