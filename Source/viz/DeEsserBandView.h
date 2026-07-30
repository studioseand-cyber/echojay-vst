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

    ANALYTIC + THE DWELL GLOW, like the transfer curve: the band shape is a pure
    function of freq_hz (evaluated through echojay::Biquad, the same struct the
    DSP filters with, so the drawn band cannot be a different band), and the live
    half is the same 128-bin histogram every Dynamics face glows with.

    WHAT THE GLOW MEANS HERE, and why it is not the transfer curve's question.
    That plot's x axis IS a level, so "how much time does the signal spend at
    this x" is a direct lookup. This plot's x axis is FREQUENCY, and a level
    histogram does not know which frequency inside the band its energy came
    from — so a glow that claimed to would be inventing data.

    What the histogram genuinely does answer, once the filter is taken into
    account, is the question this device is actually tuned by. Content at
    frequency f reaches the detector attenuated by the sidechain band's response
    there, so to push the detector over the threshold it has to be louder by
    exactly that attenuation. Ask the histogram how often the detector gets that
    loud, and the answer per frequency is:

        "how often does content HERE actually trigger the de-esser"

    which is what freq_hz is set by ear for. At the band centre nothing is
    attenuated, so the bar is the threshold itself; out on the skirts the bar
    climbs and the signal reaches it less and less. The result is a hot core
    exactly where the device is listening hardest AND the signal is actually
    getting loud enough to matter.

    WHAT A WRONG FREQUENCY LOOKS LIKE is worth being precise about, because the
    obvious guess is wrong. A band parked on the vowel does NOT stay cold — the
    vowel is loud and constant, so it clears the threshold constantly and the
    band sits PINNED white. That is not the glow failing; it is the glow
    reporting the actual fault, which is a de-esser triggering on everything.
    The tell is in the motion: pointed at the sibilance the core pulses with
    the esses and falls back between them, and pointed at the vowel it never
    falls back at all. A de-esser that is always on is always wrong, and this
    is the view that shows it without listening for it.

    Cumulative ("at or above") rather than a point lookup, deliberately: a
    point lookup is not monotonic, so a skirt that happened to land on the
    histogram's peak would out-glow the band's own centre and draw a ring.
*/

#pragma once

#include "VizView.h"
#include "DwellGlow.h"
#include "../EedDynamicsCore.h"

namespace echojay::viz
{

class DeEsserBandView : public VizView,
                        private juce::Timer
{
public:
    DeEsserBandView();
    ~DeEsserBandView() override;

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

    // ---- the dwell glow ----------------------------------------------------
    // The de-esser's OWN core publishes this from gainForSidechain, so it is
    // already the sidechain band's level distribution — the exact thing this
    // view wants and nothing extra had to be tapped for it.
    //
    // Same contract as TransferCurveView::setDwellSource: a pointer that
    // outlives the view, re-passed by the editor every refresh, polled on this
    // view's own 60 Hz timer. nullptr detaches and stops the timer.
    void setDwellSource (const echojay::dyn::DwellTap* tap, bool active);

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
    void timerCallback() override;

    // Magnitude response of the sidechain bandpass at a frequency, 0..1.
    float bandMagnitude (float hz) const noexcept;

    void rebuildPaths();

    // Fills the glow's columns along the band's own crest and renders it.
    void renderGlow (juce::Graphics& g, juce::Rectangle<float> plot, float a);

    // How far below the band's peak the glow is still allowed to reach, in dB
    // of filter attenuation. Past this the trigger bar is so high that the
    // answer is always "never", and evaluating it is only work — but it also
    // bounds how wide the glow can appear to be when a signal is slamming the
    // detector, which is what keeps a hot band from filling the whole plot.
    static constexpr float kMaxBandAttenDb = 36.0f;

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

    // Rebuilt on a band or size change only, never per frame. bandCrest_ is the
    // fill's top edge as POINTS, which is the line the glow rides — the same
    // relationship TransferCurveView's curvePts_ has to its curve.
    juce::Path bandPath_, bandFill_;
    std::vector<juce::Point<float>> bandCrest_;
    std::vector<float>              crestAttenDb_;

    DwellGlow glow_;
    double    lastTickMs_ = 0.0;

    // The sample rate the drawn response is evaluated at. The picture is of a
    // filter shape, not of a session, and 48 kHz keeps the shape stable when a
    // device is opened before it has ever been prepared.
    static constexpr double kDrawSampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeEsserBandView)
};

} // namespace echojay::viz
