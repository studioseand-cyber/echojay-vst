/*
    MeterEngine per-block cost benchmark.

    WHY THIS EXISTS. MeterEngine runs on the audio thread of BOTH plugins, and
    on up to kRegMaxSlots = 16 Links at once, so its per-block cost is
    multiplied by every Link a session carries. That cost had only ever been
    ESTIMATED, and on 2 Aug 2026 the estimate turned out to be wrong by nearly
    3x: gating the Link's unused 4096-point visual FFT was predicted to save
    "roughly a third" of per-Link metering and actually saved 12 percent. The
    estimate counted transforms and ignored that most of the analysis path is
    the 64-bin mapping, the macro bands and the spectrum ring rather than its
    FFT. Without a harness the next person guesses the same way.

    IT MEASURES THE SHIPPING CODE. The build script links each plugin's own
    Release SharedCode archive, so the MeterEngine under test is the object
    that ships, compiled with the flags it ships with. It does NOT recompile
    MeterEngine.cpp with its own flags, which would measure something else.
    That works because ECHOJAY_NO_VISUAL_FFT gates .cpp bodies only and never
    a member, so the class layout is identical in both builds and this
    translation unit can link against either archive.

    THE TWO VARIANTS are the two real builds:
      EchoJay V2   ungated, the visual FFT compiled in  (the main plugin
                   draws Visualisation, the spectrum and the spectrogram)
      EchoJay Link gated, the visual FFT compiled out   (nothing there reads
                   a spectrum; LinkMeterFrame carries no bins)
    The variant identifies ITSELF from behaviour rather than from a define:
    getVisualSpectrum returns true only where the visual path ran, so a
    mislabelled or stale archive cannot masquerade as the other build.

    METHODOLOGY, which is what makes the number trustworthy: 20000 blocks of
    512 samples at 44.1k (about 232 seconds of audio) after a 200-block warm
    up, run three times per variant. The figures below were taken on an idle
    machine and held under 1 percent spread.
    TAKE THE MINIMUM ACROSS RUNS, not the mean: interference only ever ADDS
    time, so the fastest run is the closest to the code's real cost. A first
    run several percent above the others is normal on a machine that has
    just built something, and is the reason three runs exist; if the minima
    still disagree between variants by less than their own spread, the
    change being measured is below this harness's resolution and should be
    reported as such rather than as a win.

    BASELINE, 2 Aug 2026, Apple Silicon, Release, 44.1k / 512:
        ungated   83.5 us per block   0.719 percent of one core, per instance
        gated     73.5 us per block   0.633 percent of one core, per instance
        delta     10.0 us per block   0.086 points, about 12 percent
    A future run should reproduce those within the spread on comparable
    hardware. A LARGE move in either direction is the finding, and the first
    question is which stage moved: the remaining floor is the 2048-point
    analysis FFT every block plus its 64-bin mapping, macro bands and
    spectrum ring, 4x oversampled true peak on both channels, K-weighting,
    three band-crest filter banks, correlation, the width HPF and the
    waveform ring push.

    NOT A GATE, deliberately. This is not in ~/reinstall-v2.sh's self-test
    loop and must not be added to it: it takes real time and it answers with
    a NUMBER, not a pass or a fail. A benchmark that fails a build teaches
    people to ignore it.

    bandRel GUARD. The run also reports how many macroBandDb bands carry
    signal, because that is the reading that would go silently empty if
    someone ever gated the ANALYSIS FFT by mistake. It must read 6 of 6 in
    BOTH variants.
*/

#include "MeterEngine.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

int main (int argc, char** argv)
{
    const char* label = argc > 1 ? argv[1] : "(unlabelled)";

    constexpr double kSampleRate = 44100.0;
    constexpr int    kBlockSize  = 512;
    constexpr int    kWarmUp     = 200;
    constexpr int    kBlocks     = 20000;   // about 232 s of audio

    MeterEngine eng;
    eng.prepare (kSampleRate, kBlockSize);

    // Deterministic noise: the same signal every run, so a difference
    // between runs is the code or the machine, never the input.
    std::vector<float> L ((size_t) kBlockSize), R ((size_t) kBlockSize);
    std::mt19937 rng (12345);
    std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
    for (int i = 0; i < kBlockSize; ++i)
    {
        L[(size_t) i] = dist (rng);
        R[(size_t) i] = dist (rng);
    }

    for (int b = 0; b < kWarmUp; ++b)
        eng.processBlock (L.data(), R.data(), kBlockSize);

    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < kBlocks; ++b)
        eng.processBlock (L.data(), R.data(), kBlockSize);
    const auto t1 = std::chrono::steady_clock::now();

    const double totalUs  = std::chrono::duration<double, std::micro> (t1 - t0).count();
    const double perBlock = totalUs / (double) kBlocks;
    // Wall-clock budget for one block of audio: exceeding this is 100 percent
    // of a core spent on metering alone.
    const double audioUsPerBlock = 1.0e6 * (double) kBlockSize / kSampleRate;

    // Self-identify from BEHAVIOUR, not from a compile flag: only the ungated
    // build ever sets visReady.
    std::array<float, MeterEngine::kVisBins> vis {};
    double binHz = 0.0;
    const bool visualLive = eng.getVisualSpectrum (vis, binHz);

    // bandRel guard: macroBandDb comes from the ANALYSIS FFT and must survive
    // in both variants.
    const auto md = eng.getMeterData();
    int validBands = 0;
    for (auto db : md.macroBandDb)
        if (db > -119.0f) ++validBands;

    std::printf ("%-14s %8.3f us/block   %6.3f%% of one core   visualFFT=%-3s   macroBands=%d/6\n",
                 label, perBlock, 100.0 * perBlock / audioUsPerBlock,
                 visualLive ? "on" : "off", validBands);
    return 0;
}
