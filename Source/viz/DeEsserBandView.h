/*
    DeEsserBandView.h  —  what the de-esser is LISTENING to, and what it is doing
    about it, on a frequency axis (VISUALS_PLAN.md, Dynamics signature
    visualisation — the band-device case).

    A transfer curve answers "how much" and says nothing about "where", and for
    this device "where" is the entire setting. freq_hz is the one knob a de-esser
    lives or dies on: a few hundred Hz too low and it ducks the vowel instead of
    the "s", which sounds like a mix problem rather than like a wrong number. On
    a dial that mistake is invisible. On a frequency axis it is the picture.

    So this plots, on a log-frequency axis:

      * THE DETECTOR'S BAND — the response of the actual sidechain bandpass, from
        the same freq_hz and the same Q the DSP filters with. This is the shape
        the device is triggering on, so it is drawn as the shape, not as a marker
        line at the centre frequency.
      * WHAT MOVES — the region the gain is applied to, pulled down live by the
        current reduction. In WIDE mode that is the whole spectrum; in SPLIT mode
        only the part above freq_hz. Those two are audibly different devices, and
        the difference is one shaded shape versus another.
      * HOW MUCH — the reduction as a depth on the same dB axis, so "6 dB of
        de-essing" is a distance you can see rather than a number to trust.

    ANALYTIC + ONE FLOAT TAP, like the transfer curve: the band shape is a pure
    function of freq_hz (evaluated through echojay::Biquad, the same struct the
    DSP filters with, so the drawn band cannot be a different band), and the only
    live values are the two floats the GR meter already reads.
*/

#pragma once

#include "VizView.h"
#include "../EedDynamicsCore.h"

namespace echojay::viz
{

class DeEsserBandView : public VizView
{
public:
    DeEsserBandView();

    // Matches TransferCurveView: anything at or below this hides the live parts.
    static constexpr float kNoLevel = -200.0f;

    // ---- analytic ---------------------------------------------------------
    // The sidechain filter, by the numbers the processor built it from. Q is
    // taken rather than assumed so that this is the DSP's filter, not a
    // look-alike.
    void setBand (float freqHz, double q);

    // Split ducks only above freq_hz; wide ducks everything.
    void setSplitMode (bool split);

    // How deep the dB axis runs, as a positive number. The device's range_db
    // ceiling, so the plot never runs out of room before the device does.
    void setDepthDb (float db);

    // ---- one float tap ----------------------------------------------------
    // The reduction currently applied, NEGATIVE dB, as the core reports it.
    void setGainReductionDb (float db);

    // The detector's level in the band, dBFS, and the threshold it is being
    // compared against. Together these are "is there sibilance right now, and is
    // it over the line" — which is what tuning freq_hz is actually looking for.
    void setBandLevelDb (float db);
    void setThresholdDb (float db);

    // LISTEN is a monitoring mode that makes the device output something other
    // than its result, so the picture says so rather than showing a processed
    // spectrum it is not producing.
    void setListening (bool on);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;
    void resized() override;

private:
    // Magnitude response of the sidechain bandpass at a frequency, 0..1.
    float bandMagnitude (float hz) const noexcept;

    void rebuildPaths();

    // The plot's frequency span. Fixed rather than following freq_hz, so that
    // dialling the frequency MOVES the band across a stable axis instead of
    // rescaling the world around it — a band that never appears to move is a
    // band you cannot tune.
    static constexpr float kMinHz = 200.0f;
    static constexpr float kMaxHz = 20000.0f;

    float  freqHz_    = 6500.0f;
    double q_         = 2.0;
    bool   split_     = true;
    bool   listening_ = false;
    float  depthDb_   = 24.0f;
    float  grDb_      = 0.0f;
    float  levelDb_   = kNoLevel;
    float  threshDb_  = -28.0f;

    // Rebuilt on a band or size change only, never per frame.
    juce::Path bandPath_, bandFill_;

    // The sample rate the drawn response is evaluated at. The picture is of a
    // filter shape, not of a session, and 48 kHz keeps the shape stable when a
    // device is opened before it has ever been prepared.
    static constexpr double kDrawSampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeEsserBandView)
};

} // namespace echojay::viz
