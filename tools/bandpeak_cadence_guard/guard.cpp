// ===============================================================================================
//  bandpeak_cadence_guard - bandPeak on a sample-counted 256-sample cadence (K's MA10, 28 Oct 2026)
// ===============================================================================================
//
//  READ THIS FIRST (28 Oct 2026, 17:05). K's confirmation test came back NEGATIVE: the V1 crest does
//  NOT separate a real drum excess from a body excess - a 0.13 dB gap - so Sean's ruling is that the
//  crest is DIRECTIONAL ONLY before the beta and never decides, and this branch stays UNMERGED AND
//  PARKED. What this guard establishes is narrower than it reads, and still worth keeping: the
//  cadence does what it says (a figure that no longer moves with the host's block size), bandPower is
//  untouched, and the COST is measured. It does NOT establish that the crest is usable, and BP3 below
//  must not be read as saying so - it separates a synthetic click train from a synthetic tone, which
//  is the easiest case there is and not her stems.
//
//  WHAT IT WAS FOR. K's V1 crest is bandPeak/bandPower within each 100 ms hop, and on her MA10 run
//  it separated drums (6.4-7.6 dB) from pad, bass and vocal (1.4-5.4) - PROVIDED bandPeak does not
//  depend on the host's block size. Today it does: the engine runs one band FFT per processBlock call,
//  so a hop holds sampleRate*0.1/blockSize frames (34 at a 128 block, 4 at 1024) and a MAX over more
//  frames is a higher max. Her measurement of that drift is "up to 0.38 dB from block size alone".
//
//  So the one thing this guard must establish is a NEGATIVE: with the cadence on, the crest does not
//  move with block size - her bound is 0.003 dB. A leg that only showed that would be unfalsifiable,
//  so every row here has its other direction: the same signals with the cadence OFF must show the
//  drift, or this guard is measuring nothing.
//
//  AND THE COST, which Sean asked for by name: per engine, at 128, 512 and 1024 blocks, with the
//  cadence off and on, as microseconds of CPU per second of audio. Measured here rather than
//  reasoned about, because "172 FFTs a second" is an arithmetic claim and the figure he needs is time.
#include <JuceHeader.h>
#include "MeterEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0, passes = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok    %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                                       detail.isNotEmpty() ? ("\n        " + detail).toRawUTF8() : ""); }
    std::fflush (stdout);
}

constexpr double kSr = 44100.0;

// ---- the two signals, chosen to be the two ends of her range -------------------------------------
// DRUMS: 2 Hz of short bursts - a crest the hop's max sees and its mean does not.
// PAD:   a steady two-tone - the same energy in every frame, so its crest is near 0 by construction.
// Both are deterministic, because a guard whose signal changes between runs cannot hold a 0.003 dB
// bound.
void fillDrums (std::vector<float>& l, std::vector<float>& r, int n, int startSample)
{
    for (int i = 0; i < n; ++i)
    {
        const int    s     = startSample + i;
        const int    per   = (int) (kSr / 2.0);            // a hit every 500 ms
        const int    into  = s % per;
        const double env   = into < 220 ? std::exp (-(double) into / 40.0) : 0.0;   // ~5 ms crack
        const double tone  = std::sin (2.0 * juce::MathConstants<double>::pi * 180.0 * s / kSr)
                           + std::sin (2.0 * juce::MathConstants<double>::pi * 2400.0 * s / kSr);
        const float  v     = (float) (0.45 * env * tone * 0.5);
        l[(size_t) i] = v; r[(size_t) i] = v;
    }
}
void fillPad (std::vector<float>& l, std::vector<float>& r, int n, int startSample)
{
    for (int i = 0; i < n; ++i)
    {
        const int s = startSample + i;
        const double tone = std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * s / kSr)
                          + 0.7 * std::sin (2.0 * juce::MathConstants<double>::pi * 1100.0 * s / kSr);
        const float v = (float) (0.22 * tone * 0.5);
        l[(size_t) i] = v; r[(size_t) i] = v;
    }
}

struct Run
{
    std::vector<MeterEngine::HopFrame> hops;
    double microsPerSecondOfAudio = 0.0;
    juce::int64 fineFfts = 0;
};

/** One engine, one signal, one block size. `seconds` of audio, timed. */
Run feed (bool fineCadence, int blockSize, double seconds,
          void (*filler) (std::vector<float>&, std::vector<float>&, int, int))
{
    MeterEngine eng;
    eng.prepare (kSr, blockSize);
    eng.setFinePeakCadence (fineCadence);
    std::vector<float> l ((size_t) blockSize), r ((size_t) blockSize);
    const int blocks = (int) (seconds * kSr / blockSize);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    int at = 0;
    for (int b = 0; b < blocks; ++b)
    {
        filler (l, r, blockSize, at);
        at += blockSize;
        eng.processBlock (l.data(), r.data(), blockSize);
    }
    const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;
    Run out;
    out.hops = eng.hopFrames();
    out.fineFfts = eng.finePeakFftCount();
    out.microsPerSecondOfAudio = (elapsedMs * 1000.0) / std::max (0.001, seconds);
    return out;
}

/** Her V1 crest: 10*log10(bandPeak/bandPower) within each hop, per band, taken at the 95th
    percentile over the hops so one odd frame cannot carry it. Hops with no spectral evidence are
    not in it - a figure from a hop that measured nothing is not a figure. */
double crestDb (const std::vector<MeterEngine::HopFrame>& hops, int band)
{
    std::vector<double> xs;
    for (const auto& h : hops)
    {
        if (h.bandFrames == 0) continue;
        const double pw = h.bandPower[(size_t) band], pk = h.bandPeak[(size_t) band];
        if (pw <= 1e-20 || pk <= 1e-20) continue;
        xs.push_back (10.0 * std::log10 (pk / pw));
    }
    if (xs.empty()) return 0.0;
    std::sort (xs.begin(), xs.end());
    const size_t idx = (size_t) std::min ((double) xs.size() - 1.0,
                                          std::floor (0.95 * (double) (xs.size() - 1)));
    return xs[idx];
}
double maxCrestSpread (const Run& a, const Run& b)
{
    double worst = 0.0;
    for (int bi = 0; bi < 6; ++bi)
        worst = std::max (worst, std::abs (crestDb (a.hops, bi) - crestDb (b.hops, bi)));
    return worst;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("bandpeak_cadence_guard - bandPeak on a 256-sample cadence (K's MA10)\n\n");

    const double kSeconds = 12.0;   // > 8 s, so every bound her crest needs is met

    // =========================================================================================
    //  1. THE ASK: with the cadence ON, the crest does not move with the host's block size
    // =========================================================================================
    const auto on128  = feed (true, 128,  kSeconds, fillDrums);
    const auto on512  = feed (true, 512,  kSeconds, fillDrums);
    const auto on1024 = feed (true, 1024, kSeconds, fillDrums);
    {
        const double spread = std::max (maxCrestSpread (on128, on512), maxCrestSpread (on128, on1024));
        check (spread < 0.003,
               "BP1 with the cadence ON the crest moves less than 0.003 dB across 128, 512 and 1024 "
               "blocks - K's own bound",
               "worst band spread " + juce::String (spread, 6) + " dB");
        check (on128.hops.size() == on1024.hops.size(),
               "BP1b ...and the hop CLOCK is the same either way: the same count of 100 ms hops",
               juce::String ((int) on128.hops.size()) + " vs " + juce::String ((int) on1024.hops.size()));
    }

    // =========================================================================================
    //  2. THE CONTROL: with it OFF, the drift she measured is there
    // =========================================================================================
    const auto off128  = feed (false, 128,  kSeconds, fillDrums);
    const auto off512  = feed (false, 512,  kSeconds, fillDrums);
    const auto off1024 = feed (false, 1024, kSeconds, fillDrums);
    {
        const double spread = std::max (maxCrestSpread (off128, off512), maxCrestSpread (off128, off1024));
        check (spread > 0.05,
               "BP2 CONTROL: with the cadence OFF the same signals DO move the crest with block size "
               "- so BP1 is measuring something",
               "worst band spread " + juce::String (spread, 4) + " dB");
    }

    // =========================================================================================
    //  3. THE SEPARATION her crest exists for: drums against a pad
    // =========================================================================================
    {
        const auto padOn = feed (true, 512, kSeconds, fillPad);
        double worstDrum = 1e9, bestPad = -1e9;
        for (int bi = 0; bi < 6; ++bi)
        {
            const double d = crestDb (on512.hops, bi), p = crestDb (padOn.hops, bi);
            if (d > 0.01) worstDrum = std::min (worstDrum, d);
            bestPad = std::max (bestPad, p);
        }
        // BP3 IS ABOUT THE CADENCE, NOT ABOUT THE CREST'S USEFULNESS. Her confirmation test (28
        // Oct) found a 0.13 dB gap between a real drum excess and a body excess, which is why the
        // crest is directional only before the beta. A click train against a steady tone is the
        // easiest case there is: this row says the cadence RESOLVES a transient at all, which is the
        // property the 256-sample hop was built for, and says nothing about her stems.
        check (worstDrum > bestPad + 1.0,
               "BP3 a transient signal's crest stands clear of a steady one's - the cadence resolves "
               "a transient at all (NOT a claim about her stems: her drums-vs-body gap is 0.13 dB)",
               "drums (worst band with any crest) " + juce::String (worstDrum, 2)
               + " dB vs pad (best band) " + juce::String (bestPad, 2) + " dB");
    }

    // =========================================================================================
    //  4. bandPower IS UNTOUCHED - the instruction, held as an assertion
    // =========================================================================================
    {
        const size_t n = std::min (on512.hops.size(), off512.hops.size());
        double worst = 0.0;
        for (size_t i = 0; i < n; ++i)
            for (int bi = 0; bi < 6; ++bi)
                worst = std::max (worst, (double) std::abs (on512.hops[i].bandPower[(size_t) bi]
                                                          - off512.hops[i].bandPower[(size_t) bi]));
        check (n > 0 && worst == 0.0,
               "BP4 bandPower is BIT-FOR-BIT the same with the cadence on and off - \"bandPower stays "
               "exactly as it is\"",
               juce::String ((int) n) + " hops compared, worst difference "
               + juce::String (worst, 12));
        check (n > 0 && on512.hops[n / 2].bandFrames == off512.hops[n / 2].bandFrames,
               "BP4b ...and so is bandFrames, which is what its mean is over");
    }

    // =========================================================================================
    //  5. THE CADENCE IS THE SAMPLE COUNT, not the block count
    // =========================================================================================
    {
        const int expected = (int) std::llround (kSr * 0.1 / (double) MeterEngine::kFinePeakHopSamples);
        const size_t mid = on512.hops.size() / 2;
        const int got = on512.hops.empty() ? 0 : (int) on512.hops[mid].peakFrames;
        check (std::abs (got - expected) <= 1,
               "BP5 a hop carries sampleRate*0.1/256 peak frames - 17 at 44.1 kHz - whatever the "
               "block size is",
               "expected " + juce::String (expected) + ", got " + juce::String (got));
        const int got1024 = on1024.hops.empty() ? 0 : (int) on1024.hops[on1024.hops.size() / 2].peakFrames;
        check (std::abs (got1024 - expected) <= 1,
               "BP5b ...and the same count at a 1024 block, where the BLOCK cadence would give four",
               "got " + juce::String (got1024));
        check (off512.hops.empty() || off512.hops[mid].peakFrames == 0,
               "BP5c ...and with the cadence off the field is 0, which says the peak came from the "
               "block cadence as before - not a count of zero frames");
    }

    // =========================================================================================
    //  6. THE COST, per engine, as Sean asked: 128, 512, 1024, off against on
    // =========================================================================================
    std::printf ("\nthe cost, per engine, microseconds of CPU per SECOND of audio (%.0f s measured):\n",
                 kSeconds);
    std::printf ("  block   cadence off     cadence on      on/off   fine FFTs/s\n");
    for (int bs : { 128, 512, 1024 })
    {
        const auto off = feed (false, bs, kSeconds, fillDrums);
        const auto on  = feed (true,  bs, kSeconds, fillDrums);
        std::printf ("  %5d   %10.1f us   %10.1f us   %5.2fx   %6.1f\n",
                     bs, off.microsPerSecondOfAudio, on.microsPerSecondOfAudio,
                     on.microsPerSecondOfAudio / std::max (1.0, off.microsPerSecondOfAudio),
                     (double) on.fineFfts / kSeconds);
    }
    std::printf ("  (the fine cadence is a FIXED %.0f FFTs a second at 44.1 kHz; the block cadence is\n"
                 "   sampleRate/blockSize - 344 at 128, 86 at 512, 43 at 1024 - which is why the ratio\n"
                 "   falls below 1 at the smallest block and rises at the largest.)\n",
                 kSr / MeterEngine::kFinePeakHopSamples);

    std::printf ("\n==== bandpeak_cadence_guard: %s (%d ok, %d failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", passes, failures);
    return failures == 0 ? 0 : 1;
}
