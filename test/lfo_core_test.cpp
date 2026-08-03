// Standalone test for LfoCore (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source lfo_core_test.cpp -o lfotest && ./lfotest
//
// What this pins is the stuff that is invisible until it is wrong in the field:
// the shapes' DC (a tremolo that gets quieter when you switch to saw), phase
// continuity under a rate change, the tempo-sync arithmetic, and the output
// smoothing that keeps SQUARE from clicking once per cycle.

#include "EedLfoCore.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace echojay;

static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }

// Run the LFO for n samples and hand back every left-channel value.
static std::vector<float> run (LfoCore& lfo, int n)
{
    std::vector<float> out ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        lfo.nextStereo (l, r);
        out[(size_t) i] = l;
    }
    return out;
}

// Rising zero crossings over a run == cycles completed, which is the only
// measurement of "rate" that does not assume anything about the waveform.
static int risingZeroCrossings (const std::vector<float>& v)
{
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] < 0.0f && v[i] >= 0.0f) ++n;
    return n;
}

int main()
{
    constexpr double kSr = 48000.0;

    std::printf ("== shapes are bipolar and hit their expected values ==\n");
    {
        check (near (LfoCore::shapeAt (LfoCore::kSine, 0.0f),  0.0, 1e-6), "sine starts at 0");
        check (near (LfoCore::shapeAt (LfoCore::kSine, 0.25f), 1.0, 1e-6), "sine peaks at +1");
        check (near (LfoCore::shapeAt (LfoCore::kSine, 0.75f), -1.0, 1e-6), "sine troughs at -1");

        check (near (LfoCore::shapeAt (LfoCore::kTriangle, 0.0f),  0.0, 1e-6), "tri starts at 0");
        check (near (LfoCore::shapeAt (LfoCore::kTriangle, 0.25f), 1.0, 1e-6), "tri peaks at +1");
        check (near (LfoCore::shapeAt (LfoCore::kTriangle, 0.5f),  0.0, 1e-6), "tri crosses 0 at half");
        check (near (LfoCore::shapeAt (LfoCore::kTriangle, 0.75f), -1.0, 1e-6), "tri troughs at -1");

        check (near (LfoCore::shapeAt (LfoCore::kSquare, 0.25f),  1.0, 1e-6), "square high in first half");
        check (near (LfoCore::shapeAt (LfoCore::kSquare, 0.75f), -1.0, 1e-6), "square low in second half");

        check (near (LfoCore::shapeAt (LfoCore::kSaw, 0.0f),  0.0, 1e-6), "saw starts at 0");
        check (near (LfoCore::shapeAt (LfoCore::kSaw, 0.25f), 0.5, 1e-6), "saw rises through +0.5");
        check (near (LfoCore::shapeAt (LfoCore::kSaw, 0.5f), -1.0, 1e-6), "saw resets to -1 at half");
    }

    std::printf ("== every shape is ZERO-MEAN (switching waveform must not shift the centre) ==\n");
    {
        // RANDOM is held within a cycle, so its zero-mean claim lives over its
        // CYCLE LOOP rather than over one period; the deterministic shapes are
        // averaged across a single cycle like before.
        for (int s = 0; s < LfoCore::kNumShapes; ++s)
        {
            double sum = 0.0;
            double tol = 1e-3;

            if (s == LfoCore::kRandom)
            {
                constexpr int kCycles = LfoCore::kRandomCycleMask + 1;
                for (int c = 0; c < kCycles; ++c)
                    sum += LfoCore::shapeAt (s, (float) c + 0.5f);
                sum /= kCycles;
                // 1024 uniform draws: the statistical mean is ~0.02-ish, not 1e-3.
                tol = 0.05;
            }
            else
            {
                constexpr int kSteps = 100000;
                for (int i = 0; i < kSteps; ++i)
                    sum += LfoCore::shapeAt (s, (float) i / (float) kSteps);
                sum /= kSteps;
            }

            check (near (sum, 0.0, tol),
                   std::string ("mean of ") + LfoCore::shapeName (s) + " is 0");
        }
    }

    std::printf ("== HARMONIC: bounded, hits +/-1, and flatter at the crest than sine ==\n");
    {
        bool bounded = true;
        float peak = 0.0f;
        int   nearTopH = 0, nearTopS = 0;
        constexpr int kSteps = 20000;
        for (int i = 0; i < kSteps; ++i)
        {
            const float p = (float) i / (float) kSteps;
            const float h = LfoCore::shapeAt (LfoCore::kHarmonic, p);
            if (std::fabs (h) > 1.0001f) bounded = false;
            peak = std::max (peak, std::fabs (h));
            if (h > 0.95f) ++nearTopH;
            if (LfoCore::shapeAt (LfoCore::kSine, p) > 0.95f) ++nearTopS;
        }
        check (bounded, "never leaves +/-1");
        check (peak > 0.999f, "still reaches full scale");
        check (near (LfoCore::shapeAt (LfoCore::kHarmonic, 0.0f), 0.0, 1e-5),
               "rises through 0 at phase 0, like every other shape");
        // "Smoother at the peaks" made measurable: the curve LINGERS near its
        // crest, so it spends more of the cycle above 95% than sine does.
        check (nearTopH > nearTopS,
               "spends longer above 0.95 than sine (" + std::to_string (nearTopH)
               + " vs " + std::to_string (nearTopS) + " of 20000 steps)");
    }

    std::printf ("== RANDOM: held for exactly one period, changes between periods ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (4.0f);                // 12000 samples per period at 48k
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kRandom);
        lfo.setSmoothingMs (0.0f);           // the raw staircase
        lfo.reset();

        const int period = (int) (kSr / 4.0);
        const auto v = run (lfo, period * 4);

        // Within each period, one value; across periods, different values.
        bool heldFlat = true, changes = true;
        for (int c = 0; c < 4; ++c)
        {
            // Skip a few samples either side of the boundary: the rate smoother
            // makes the period a hair off the nominal count, which is the phase
            // glide contract, not a hold failure.
            const float mid = v[(size_t) (c * period + period / 2)];
            for (int i = c * period + 8; i < (c + 1) * period - 8; ++i)
                if (v[(size_t) i] != mid) heldFlat = false;
            if (c > 0 && v[(size_t) (c * period + period / 2)]
                      == v[(size_t) ((c - 1) * period + period / 2)]) changes = false;
        }
        check (heldFlat, "each period is a single held value");
        check (changes,  "and consecutive periods hold different values");

        bool bounded = true;
        for (float x : v) if (std::fabs (x) > 1.0f) bounded = false;
        check (bounded, "the staircase stays inside +/-1");
    }

    std::printf ("== RANDOM: smoothing turns the step into a glide ==\n");
    {
        auto worstStepAt = [&] (float smoothMs)
        {
            LfoCore lfo;
            lfo.prepare (kSr);
            lfo.setRateHz (8.0f);
            lfo.setDepthPercent (100.0f);
            lfo.setShape (LfoCore::kRandom);
            lfo.setSmoothingMs (smoothMs);
            lfo.reset();
            const auto v = run (lfo, (int) kSr);
            float worst = 0.0f;
            for (size_t i = 1; i < v.size(); ++i)
                worst = std::max (worst, std::fabs (v[i] - v[i - 1]));
            return worst;
        };

        const float hard = worstStepAt (0.0f);
        const float soft = worstStepAt (40.0f);
        check (hard > 0.5f,  "unsmoothed, the value JUMPS at the period boundary");
        check (soft < 0.02f, "40 ms of smoothing makes the same boundary a glide (worst step "
                             + std::to_string (soft) + ")");
    }

    std::printf ("== RANDOM: stereo offset 0 keeps both channels on the same staircase ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (6.0f);
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kRandom);
        lfo.setStereoPhaseDeg (0.0f);
        lfo.reset();

        bool identical = true;
        for (int i = 0; i < (int) kSr; ++i)
        {
            float l = 0.0f, r = 0.0f;
            lfo.nextStereo (l, r);
            if (l != r) identical = false;
        }
        check (identical, "0 degrees: L and R hold identical values");
    }

    std::printf ("== phase wraps into [0,1) for any input ==\n");
    {
        bool ok = true;
        const float probes[] = { -12.7f, -1.0f, -0.25f, 0.0f, 0.5f, 1.0f, 3.3f, 1000.9f };
        for (float p : probes)
        {
            const float w = LfoCore::wrap01 (p);
            if (! (w >= 0.0f && w < 1.0f)) ok = false;
        }
        check (ok, "wrap01 always lands in [0,1)");
        check (near (LfoCore::wrap01 (-0.25f), 0.75, 1e-6), "wrap01(-0.25) == 0.75");
    }

    std::printf ("== free rate: cycles per second match the dialled Hz ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kSine);
        // Measure the oscillator, not the output filter: the smoother's group
        // delay would shift the last crossing of the window by its own time
        // constant. Smoothing gets its own test below.
        lfo.setSmoothingMs (0.0f);

        for (float hz : { 1.0f, 4.0f, 10.0f })
        {
            lfo.setRateHz (hz);
            lfo.reset();
            // A shade over a second: the run STARTS at a zero crossing, so the
            // last cycle's crossing lands exactly on sample kSr and would fall
            // outside a window of exactly one second.
            const auto v = run (lfo, (int) kSr + 64);
            const int cycles = risingZeroCrossings (v);
            check (cycles == (int) hz,
                   std::to_string ((int) hz) + " Hz gives exactly "
                   + std::to_string ((int) hz) + " cycles/sec (got "
                   + std::to_string (cycles) + ")");
        }
    }

    std::printf ("== rate is clamped to the advertised range ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (999.0f);
        check (lfo.getRateHz() == LfoCore::kMaxRateHz, "clamps to 20 Hz");
        lfo.setRateHz (-5.0f);
        check (lfo.getRateHz() == LfoCore::kMinRateHz, "clamps to 0.01 Hz");
        lfo.setDepthPercent (500.0f);
        check (lfo.getDepthPercent() == LfoCore::kMaxDepth, "depth clamps to 100%");
        lfo.setDepthPercent (-1.0f);
        check (lfo.getDepthPercent() == LfoCore::kMinDepth, "depth clamps to 0%");
        lfo.setShape (99);
        check (lfo.getShape() == LfoCore::kNumShapes - 1, "shape clamps to the last waveform");
        lfo.setDivisionIndex (-3);
        check (lfo.getDivisionIndex() == 0, "division clamps to the first entry");
    }

    std::printf ("== tempo sync: division arithmetic ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setTempoSync (true);
        lfo.setTempoBpm (120.0);

        // 120 BPM: a quarter note is 0.5 s, so 1/4 == 2 Hz.
        lfo.setDivisionIndex (LfoCore::kDefaultDivision);
        check (near (lfo.effectiveRateHz(), 2.0, 1e-4), "1/4 at 120 BPM == 2 Hz");

        // 1/8 is half the period, so twice the rate.
        lfo.setDivisionIndex (8);
        check (near (lfo.effectiveRateHz(), 4.0, 1e-4), "1/8 at 120 BPM == 4 Hz");

        // 1/1 is four beats == 2 s.
        lfo.setDivisionIndex (2);
        check (near (lfo.effectiveRateHz(), 0.5, 1e-4), "1/1 at 120 BPM == 0.5 Hz");

        // Tempo scales it linearly.
        lfo.setTempoBpm (60.0);
        lfo.setDivisionIndex (LfoCore::kDefaultDivision);
        check (near (lfo.effectiveRateHz(), 1.0, 1e-4), "1/4 at 60 BPM == 1 Hz");

        // Off the top of the range, the synced rate still cannot exceed what a
        // manual rate can reach.
        lfo.setTempoBpm (200.0);
        lfo.setDivisionIndex (LfoCore::kNumDivisions - 1);      // 1/32
        check (lfo.effectiveRateHz() <= LfoCore::kMaxRateHz,
               "a fast division at a fast tempo is clamped, not out of range");

        // Sync off hands the manual rate straight back.
        lfo.setTempoSync (false);
        lfo.setRateHz (3.5f);
        check (near (lfo.effectiveRateHz(), 3.5, 1e-5), "sync off returns the dialled Hz");
    }

    std::printf ("== divisions are ordered slowest -> fastest ==\n");
    {
        bool ordered = true;
        for (int i = 1; i < LfoCore::kNumDivisions; ++i)
            if (! (LfoCore::division (i).beats < LfoCore::division (i - 1).beats))
                ordered = false;
        check (ordered, "turning the division dial up always speeds the LFO up");
        check (std::string (LfoCore::division (LfoCore::kDefaultDivision).name) == "1/4",
               "the default division is 1/4");
    }

    std::printf ("== a missing host tempo does NOT freeze a synced LFO ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setTempoSync (true);
        lfo.setTempoBpm (0.0);        // what an absent/invalid playhead looks like
        check (lfo.effectiveRateHz() > 0.0f, "stand-in tempo keeps it running");
    }

    std::printf ("== depth scales the output, and 0 means OFF ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (2.0f);
        lfo.setShape (LfoCore::kSine);

        lfo.setDepthPercent (0.0f);
        lfo.reset();
        auto v = run (lfo, 4096);
        float peak = 0.0f;
        for (float x : v) peak = std::max (peak, std::fabs (x));
        check (peak < 1e-5f, "depth 0% produces silence");

        lfo.setDepthPercent (100.0f);
        lfo.reset();
        v = run (lfo, (int) kSr);       // a full second covers several cycles
        peak = 0.0f;
        for (float x : v) peak = std::max (peak, std::fabs (x));
        check (near (peak, 1.0, 0.02), "depth 100% reaches full scale");

        lfo.setDepthPercent (50.0f);
        lfo.reset();
        v = run (lfo, (int) kSr);
        peak = 0.0f;
        for (float x : v) peak = std::max (peak, std::fabs (x));
        check (near (peak, 0.5, 0.02), "depth 50% reaches half scale");
    }

    std::printf ("== stereo phase offset ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (2.0f);
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kSine);

        lfo.setStereoPhaseDeg (0.0f);
        lfo.reset();
        bool identical = true;
        for (int i = 0; i < 4096; ++i)
        {
            float l = 0.0f, r = 0.0f;
            lfo.nextStereo (l, r);
            if (! near (l, r, 1e-6)) identical = false;
        }
        check (identical, "0 degrees: both channels in step");

        lfo.setStereoPhaseDeg (180.0f);
        lfo.reset();
        bool inverted = true;
        for (int i = 0; i < 4096; ++i)
        {
            float l = 0.0f, r = 0.0f;
            lfo.nextStereo (l, r);
            if (! near (l, -r, 1e-4)) inverted = false;
        }
        check (inverted, "180 degrees: right is the mirror of left");

        lfo.setStereoPhaseDeg (90.0f);
        lfo.reset();
        double maxDiff = 0.0;
        for (int i = 0; i < 4096; ++i)
        {
            float l = 0.0f, r = 0.0f;
            lfo.nextStereo (l, r);
            maxDiff = std::max (maxDiff, (double) std::fabs (l - r));
        }
        check (maxDiff > 0.5, "90 degrees: the channels genuinely differ");
    }

    std::printf ("== output smoothing tames SQUARE's edge (this is what stops the click) ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (2.0f);
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kSquare);
        lfo.setSmoothingMs (2.0f);
        lfo.reset();

        auto v = run (lfo, (int) kSr);
        float worstStep = 0.0f;
        for (size_t i = 1; i < v.size(); ++i)
            worstStep = std::max (worstStep, std::fabs (v[i] - v[i - 1]));

        // Unsmoothed the step would be a full 2.0 in one sample.
        check (worstStep < 0.05f,
               "no full-scale jump between samples (worst step "
               + std::to_string (worstStep) + ")");

        // It still gets all the way to both rails: smoothing must not shrink it.
        float lo = 1.0f, hi = -1.0f;
        for (float x : v) { lo = std::min (lo, x); hi = std::max (hi, x); }
        check (hi > 0.98f && lo < -0.98f, "square still reaches both rails");
    }

    std::printf ("== smoothing 0 ms leaves the edge hard ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (2.0f);
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kSquare);
        lfo.setSmoothingMs (0.0f);
        lfo.reset();

        auto v = run (lfo, (int) kSr);
        float worstStep = 0.0f;
        for (size_t i = 1; i < v.size(); ++i)
            worstStep = std::max (worstStep, std::fabs (v[i] - v[i - 1]));
        check (worstStep > 1.9f, "an unsmoothed square steps the full 2.0");
    }

    std::printf ("== a rate change GLIDES: phase stays continuous ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kSine);
        lfo.setSmoothingMs (0.0f);      // isolate the rate smoother
        lfo.setRateHz (1.0f);
        lfo.reset();

        std::vector<float> v;
        v.reserve (8000);
        for (int i = 0; i < 4000; ++i) v.push_back (lfo.next());
        lfo.setRateHz (20.0f);                       // the biggest jump possible
        for (int i = 0; i < 4000; ++i) v.push_back (lfo.next());

        float worstStep = 0.0f;
        for (size_t i = 1; i < v.size(); ++i)
            worstStep = std::max (worstStep, std::fabs (v[i] - v[i - 1]));

        // At 20 Hz / 48 kHz a sine moves at most ~0.0026 per sample. Anything
        // near that is a glide; a phase jump would be orders of magnitude bigger.
        check (worstStep < 0.01f,
               "no discontinuity across a 1 -> 20 Hz change (worst step "
               + std::to_string (worstStep) + ")");
    }

    std::printf ("== depth changes are smoothed too ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (1.0f);
        lfo.setShape (LfoCore::kSine);
        lfo.setSmoothingMs (0.0f);
        lfo.setDepthPercent (0.0f);
        lfo.reset();

        // Park the phase near the peak, where a depth step would be loudest.
        std::vector<float> v;
        for (int i = 0; i < 12000; ++i) v.push_back (lfo.next());   // quarter cycle
        lfo.setDepthPercent (100.0f);
        for (int i = 0; i < 4000; ++i) v.push_back (lfo.next());

        float worstStep = 0.0f;
        for (size_t i = 1; i < v.size(); ++i)
            worstStep = std::max (worstStep, std::fabs (v[i] - v[i - 1]));
        check (worstStep < 0.01f,
               "no step when depth goes 0 -> 100% at the waveform peak (worst "
               + std::to_string (worstStep) + ")");
    }

    std::printf ("== valueAt() is the multi-tap path Chorus and Phaser use ==\n");
    {
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (1.0f);
        lfo.setDepthPercent (100.0f);
        lfo.setShape (LfoCore::kSine);
        lfo.reset();

        for (int i = 0; i < 1000; ++i) lfo.advance();

        const float a = lfo.valueAt (0.0f);
        const float b = lfo.valueAt (0.5f);
        check (near (a, -b, 1e-4), "a half-cycle tap is the mirror of the zero tap");

        const float c = lfo.valueAt (1.0f);
        check (near (a, c, 1e-5), "a whole-cycle tap wraps back onto itself");

        lfo.setDepthPercent (0.0f);
        for (int i = 0; i < (int) kSr; ++i) lfo.advance();   // let depth settle
        check (std::fabs (lfo.valueAt (0.3f)) < 1e-4, "taps honour depth");
    }

    std::printf ("== the display phase tap follows the oscillator ==\n");
    {
        // The scope's playhead rides this one float. If it stops tracking, the
        // dot freezes while the audio keeps modulating — a bug that looks like
        // "the visualisation is broken" and is invisible to every other test.
        LfoCore lfo;
        lfo.prepare (kSr);
        lfo.setRateHz (1.0f);
        lfo.setDepthPercent (100.0f);
        lfo.reset();

        check (near (lfo.displayPhase(), 0.0, 1e-6), "a fresh LFO taps phase 0");

        // reset() AFTER running, which is the case that matters: a transport
        // stop must park the playhead at the cycle start rather than leave it
        // wherever the last block happened to end.
        for (int i = 0; i < 5000; ++i) lfo.advance();
        check (lfo.displayPhase() > 0.0f, "the tap moved while running");
        lfo.reset();
        check (near (lfo.displayPhase(), 0.0, 1e-6), "reset republishes, it does not go stale");

        // A quarter of a second at 1 Hz is a quarter of a cycle.
        for (int i = 0; i < (int) kSr / 4; ++i) lfo.advance();
        check (near (lfo.displayPhase(), 0.25, 0.001), "a quarter cycle in, the tap reads 0.25");

        for (int i = 0; i < (int) kSr / 4; ++i) lfo.advance();
        check (near (lfo.displayPhase(), 0.5, 0.001), "half a cycle in, the tap reads 0.5");

        // It must agree with the audio thread's own phase, not drift from it.
        check (near (lfo.displayPhase(), lfo.currentPhase(), 1e-6),
               "the tap IS the oscillator's phase, not a second copy of it");

        // And it stays in range across a wrap, so a playhead cannot fly off the plot.
        bool inRange = true;
        for (int i = 0; i < (int) kSr; ++i)
        {
            lfo.advance();
            const float p = lfo.displayPhase();
            if (! (p >= 0.0f && p < 1.0f)) inRange = false;
        }
        check (inRange, "the tap stays in [0,1) across the wrap");

        // nextStereo() advances too, so a device using that path still animates.
        LfoCore other;
        other.prepare (kSr);
        other.setRateHz (2.0f);
        other.reset();
        for (int i = 0; i < 4096; ++i) { float l, r; other.nextStereo (l, r); }
        check (other.displayPhase() > 0.0f, "nextStereo() publishes the tap as well");
    }

    std::printf ("== unipolar helper ==\n");
    {
        check (near (LfoCore::toUnipolar (-1.0f), 0.0, 1e-6), "-1 maps to 0");
        check (near (LfoCore::toUnipolar ( 0.0f), 0.5, 1e-6), " 0 maps to 0.5");
        check (near (LfoCore::toUnipolar ( 1.0f), 1.0, 1e-6), "+1 maps to 1");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
