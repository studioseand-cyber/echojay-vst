/*
    HarmonicBars.h  —  the harmonics a saturator is actually generating, as bars
    (VISUALS_PLAN.md, Harmonic signature visualisation).

    The second of the two views that genuinely need a signal tap. The transfer
    curve says what the shaper WILL do; this says what it DID — 2nd-order
    dominant (the warm, even-harmonic sound), 3rd-order dominant (the hard,
    odd-harmonic one), or a forest of everything, which is the audible
    difference between "driven" and "distorted".

    ANALYSED BY GOERTZEL, NOT BY FFT. Only ~8 frequencies are wanted and they
    are known in advance, which is the exact case Goertzel is for: it costs one
    multiply-accumulate per sample per harmonic and needs no window, no scratch
    buffer of twice the ring, and no juce::dsp::FFT. The EQ's analyzer wants a
    whole spectrum and rightly uses an FFT; this does not.

    Magnitudes are shown RELATIVE TO THE FUNDAMENTAL, which is the only form
    that means anything: absolute harmonic levels rise and fall with the input,
    and the character of a saturator is the ratio between them.
*/

#pragma once

#include "VizView.h"

#include <vector>

namespace echojay::viz
{

class HarmonicBars : public VizView
{
public:
    HarmonicBars();

    static constexpr int kMaxHarmonics = 8;

    // How many bars, INCLUDING the fundamental. 6 fits a rack slot.
    void setNumHarmonics (int n);

    // The ring path: a frame of mono samples straight off a SpectrumTap, plus
    // the fundamental to measure against. Everything else is done in here.
    void analyse (const float* mono, int numSamples,
                  double sampleRate, double fundamentalHz);

    // The direct path, for a device that already has magnitudes: dB relative to
    // the fundamental, [0] being the fundamental itself (so, 0).
    void setMagnitudesDb (const float* db, int n);

    // The bottom of the scale. -60 shows the noise floor; -40 shows only what
    // is audible.
    void setFloorDb (float db);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    int   numHarmonics_ = 6;
    float floorDb_      = -60.0f;
    float magsDb_[kMaxHarmonics] {};
    bool  haveData_     = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonicBars)
};

} // namespace echojay::viz
