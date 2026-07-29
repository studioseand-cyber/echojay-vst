/*
    EedHarmonicAnalysis.h  —  the display-side analysis behind the Harmonic
    cluster's HarmonicBars (VISUALS_PLAN.md, Phase V1 Harmonic).

    MESSAGE THREAD ONLY. Nothing here runs on the audio thread: the processors
    write a viz::SpectrumTap and the editor's timer reads a frame and calls in
    here. That is deliberate — it means the analysis can be as careful as it
    likes without costing the audio thread anything, and it is why this can
    afford an autocorrelation the DSP never could.

    Why a fundamental estimator exists at all: viz::HarmonicBars measures
    harmonics of a fundamental the CALLER supplies, because it has no way to
    know what the signal is. Something has to find it. For a saturator that
    "something" cannot be a fixed frequency — the whole reading is "what is this
    device doing to the note being played", and the note changes.

    JUCE-free, like EqEngine and EedHarmonicCore, with a g++ test in
    test/harmonic_analysis_test.cpp. Pitch detection is exactly the kind of code
    that is confidently wrong by an octave, so it is pinned against synthesised
    signals whose answer is known.
*/

#pragma once

#include <cstddef>
#include <vector>

namespace echojay::harmonic
{

// ---------------------------------------------------------------------------
// One Goertzel bin: the magnitude of `freqHz` within `numSamples`.
//
// Rectangular-windowed, which is only exact when the frequency lands on a bin
// centre — see wholeCycleLength(), which is how a caller arranges exactly that.
// ---------------------------------------------------------------------------
double goertzelMagnitude (const float* x, int numSamples,
                          double sampleRate, double freqHz) noexcept;

// ---------------------------------------------------------------------------
// The largest frame length, no greater than `maxSamples`, that holds a WHOLE
// number of cycles of `freqHz`.
//
// This is the trick that makes an unwindowed Goertzel honest. Analysed over an
// arbitrary length, a fundamental that does not land on a bin centre leaks into
// its neighbours at only about -13 dB, which is far louder than the 2nd and 3rd
// harmonics being measured — so the bars would show the window's skirt rather
// than the device's distortion, and would show it WORST when the device is
// cleanest. Trimmed to a whole number of cycles, the fundamental and every one
// of its harmonics land exactly on bin centres and the leakage is gone. It
// costs nothing but a shorter frame.
//
// Returns 0 when no whole cycle fits.
// ---------------------------------------------------------------------------
int wholeCycleLength (int maxSamples, double sampleRate, double freqHz) noexcept;

// ---------------------------------------------------------------------------
// What the estimator found.
// ---------------------------------------------------------------------------
struct FundamentalEstimate
{
    float hz      = 0.0f;    // 0 when nothing was found
    float clarity = 0.0f;    // 0..1: the normalised autocorrelation at the peak
    float rms     = 0.0f;    // of the analysed frame, linear
    bool  locked  = false;   // tonal enough and loud enough to be believed

    // A caller draws bars only when this is true. Showing harmonic ratios for a
    // signal with no fundamental is not a reading, it is a pattern in noise —
    // and it would be moving, so it would look like it meant something.
    explicit operator bool() const noexcept { return locked; }
};

// ---------------------------------------------------------------------------
// Estimate the fundamental of a mono frame by normalised autocorrelation.
//
// Two stages, for cost and for accuracy in that order: a coarse pass on a
// signal decimated to about 6 kHz finds the period, and a fine pass at the full
// rate refines it to a fraction of a sample. The refinement is not a nicety —
// the harmonic bins are placed at k*f0, so an error in f0 is multiplied by 8 by
// the time it reaches the top bar, and a 1% coarse error would put that bar
// most of a bin away from the harmonic it is supposed to be measuring.
//
// Sub-octave guard: a periodic signal correlates just as well at twice its
// period as at its period, so the LOWEST lag that is within a whisker of the
// best one wins. Without that, every other frame reads an octave low.
// ---------------------------------------------------------------------------
FundamentalEstimate estimateFundamental (const float* mono, int numSamples,
                                         double sampleRate,
                                         float minHz = 40.0f,
                                         float maxHz = 1200.0f);

// ---------------------------------------------------------------------------
// Harmonic magnitudes of `signal`, in dB, measured at k*fundamentalHz and
// expressed RELATIVE TO `reference` measured at the fundamental.
//
// The two buffers are separate so a device can measure one signal against
// another's fundamental. That is what the Exciter needs: its bars should show
// what it GENERATED, which means analysing (wet - dry) — a signal that by
// construction has almost nothing at the fundamental. Normalising that against
// its own fundamental would divide by noise and paint a wall of full-scale
// bars. Referenced to the input's fundamental instead, the bars read as "how
// much harmonic content was added, relative to the note", which is the only
// version of the number that means anything.
//
// Pass the same pointer for both to get the ordinary self-referenced reading.
// `outDb[0]` is the signal's own level at the fundamental, so it is 0 for the
// self-referenced case and deeply negative for the Exciter's.
// ---------------------------------------------------------------------------
void harmonicMagnitudesDb (const float* signal,
                           const float* reference,
                           int numSamples,
                           double sampleRate,
                           double fundamentalHz,
                           float* outDb, int numHarmonics,
                           float floorDb = -60.0f);

} // namespace echojay::harmonic
