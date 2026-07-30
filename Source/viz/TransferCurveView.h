/*
    TransferCurveView.h  —  the dynamics transfer curve: input dB across, output
    dB up (VISUALS_PLAN.md, Dynamics signature visualisation).

    THE CURVE IS THE DSP'S OWN CURVE. This does not re-derive "threshold, ratio,
    knee" in drawing code — it holds an echojay::GainCurve, the exact struct
    DynamicsCore runs on the audio thread, and asks it for reductionDb() at each
    pixel. So the picture cannot disagree with the sound: a knee change to the
    gain computer redraws correctly with no edit here, and a drawn curve that
    looked right while the DSP was wrong is not a shape this can take.

    That is what "analytic" means in the plan: no processor change, no tap, no
    FFT. Threshold / ratio / knee / range are values the editor already reads
    through getParamValue, and the whole plot is a pure function of them.

    THE DWELL GLOW is the live half, and it replaced a flying dot. The dot was
    honest and it was wrong: a single float polled at 20 Hz is twenty opinions a
    second about a signal that had thousands of levels in that time, so it could
    only ever be drawn as a point that jumps, and a jumping point is read as
    "the signal is unstable" when what is actually unstable is the sampling.

    What a user wants from this plot is not "where is the signal RIGHT NOW", it
    is "where does the signal LIVE, and is that anywhere near the knee". So the
    curve itself glows, brightest where the detector spends the most time — cold
    teal through amber to white-hot — and the answer is legible at a glance
    without watching anything move.

    Smoothness is not a finish on that, it is the mechanism, and it is built in
    three layers, none of which the other two can substitute for:

      1. AUDIO RATE. DynamicsCore accumulates the histogram per sample and
         publishes it whole (dyn::DwellTap). The density is an INTEGRAL, not a
         sample, so it is already continuous before the editor sees it — and it
         is right about transients that live entirely between two UI frames.
      2. TIME-CONSTANT EASING. This view keeps a displayed histogram it eases
         toward the published one with (1 - exp(-dt/tau)), not a fixed multiply.
         A fixed multiply is a different speed at every frame rate; this is the
         same speed at 60 Hz, at 24 Hz, and across a dropped frame.
      3. SPATIAL SMOOTHING across bins, so the gradient along the curve is
         continuous rather than 128 visible steps.
      4. A PER-PIXEL RENDER, composited column by column with a soft
         perpendicular falloff.

    All four, the contrast that concentrates the glow into a core, and the
    cold-to-white-hot ramp now live in DwellGlow — shared with the de-esser's
    band view, because two glows tuned separately are two glows that drift apart
    on the first nudge, and a compressor and a de-esser have to look like the
    same instrument. What is left here is the only part that is this view's own:
    the curve's shape, and WHICH LEVEL each pixel of it stands for.

    The instantaneous level is still taken (setInputLevelDb) but it is now only
    a soft gaussian lift in the glow — a whisper of "and here, this instant" —
    deliberately not a dot, because a dot is the thing this replaced.
*/

#pragma once

#include "VizView.h"
#include "DwellGlow.h"
#include "../EedDynamicsCore.h"

namespace echojay::viz
{

class TransferCurveView : public VizView,
                          private juce::Timer
{
public:
    TransferCurveView();
    ~TransferCurveView() override;

    // Anything at or below this drops the instantaneous whisper — the value a
    // device passes when it is bypassed or has never seen a sample.
    static constexpr float kNoLevel = -200.0f;

    // Nothing to draw for the two optional overlays below.
    static constexpr float kNoHysteresis = 0.0f;
    static constexpr float kNoCeiling    = 1000.0f;

    // ---- the analytic part (no processor change) --------------------------
    // Exactly the four numbers the gain computer takes, in the schema's units.
    void setCurve (float thresholdDb, float ratio, float kneeDb, float rangeDb,
                   echojay::DynamicsMode mode);

    // Makeup lifts the whole output axis, which is the difference between "this
    // is pulling 6 dB off" and "this is pulling 6 dB off and handing it back".
    void setMakeupDb (float db);

    // How far down the axes run. -60 suits a compressor; a gate wants further.
    void setFloorDb (float db);

    // How far UP they run. kAutoAxisTop derives it from the makeup, which is
    // right for every face whose interesting behaviour happens below 0 dBFS.
    //
    // A LIMITER is the exception, and it is not a cosmetic one: with a -0.3 dB
    // ceiling and an axis stopping at 0, the entire brick wall is the top 0.3 dB
    // of the plot — a picture of a limiter in which the limiting is invisible.
    // Its input routinely goes above 0 dBFS (that is what it is for), so its
    // axis has to as well.
    static constexpr float kAutoAxisTop = -1000.0f;
    void setAxisTopDb (float db);

    // ---- overlays two of the six faces need -------------------------------
    // A GATE's hysteresis, in dB below the threshold. The gate opens at the
    // threshold and closes only at threshold-hysteresis; between the two it
    // holds whatever state it is already in, so there is no single curve through
    // that span. Drawn as a BAND rather than folded into the curve, because a
    // curve that picked one of the two edges would be claiming the gate is
    // deterministic there when the whole point of hysteresis is that it is not.
    void setHysteresisDb (float db);

    // A LIMITER's ceiling, on the OUTPUT axis. The curve already flattens there
    // (an infinite ratio does that on its own), but the flat top of a curve and
    // a promise that nothing gets past it are different statements, and the
    // ceiling is the one the device is actually making. kNoCeiling for none.
    void setCeilingLineDb (float db);

    // ---- the dwell glow ----------------------------------------------------
    // The core's published histogram, and whether it should be believed right
    // now (false for a bypassed or band-bypassed device, which fades the glow
    // out over the easing time constant rather than snapping it off).
    //
    // A POINTER, polled by this view's OWN 60 Hz timer rather than pushed by
    // the editor's 20 Hz one, and that is the point: the editor's timer is
    // there to sync knobs, and easing toward a target that only moves 20 times
    // a second reintroduces exactly the stepping the histogram was built to
    // remove. The tap outlives the view (it lives in the processor), and the
    // editor re-passes it every refresh, so a stale pointer is not reachable.
    //
    // nullptr detaches entirely and stops the timer.
    void setDwellSource (const echojay::dyn::DwellTap* tap, bool active);

    // The detector's current level, dB. Now only a soft local lift in the glow;
    // kNoLevel drops it. NOT a dot — see the header comment.
    void setInputLevelDb (float db);

    // The reduction readout in the corner. Not part of the glow: it is the one
    // number the picture cannot express as a shape.
    void setGainReductionDb (float db);

    // ---- one of several, for the 4-band's row of four ---------------------
    // Recedes this curve so a sibling can be the one being edited. DELIBERATELY
    // NOT setDimmed: dimmed means "this device is bypassed and is not doing
    // what the picture shows", and it correctly takes the glow away. An
    // unselected band IS still processing, so its glow has to stay — four live
    // plots with one in front is the entire point of showing four.
    void setSelected (bool s);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;
    void resized() override;

private:
    void timerCallback() override;

    // ---- what is still THIS view's own about the glow ----------------------
    // The instantaneous whisper: how wide in dB, and how much it can lift the
    // local intensity. Small on purpose — it is a hint, not a marker.
    static constexpr float kWhisperSigmaDb = 2.0f;
    static constexpr float kWhisperGain    = 0.18f;

    // 0..1 heat for an input level. The histogram's own answer plus the
    // whisper — everything else about how that looks lives in DwellGlow, so
    // this view and the de-esser's cannot end up with two different glows.
    float heatAt (float inDb) const noexcept;

    // Fills the glow's columns along the curve and renders it.
    void renderGlow (juce::Graphics& g, juce::Rectangle<float> plot, float a);

    // Output dB for an input dB, INCLUDING makeup — the curve as heard.
    float outputDb (float inDb) const noexcept;

    // The top of both axes: the explicit override when one is set, otherwise
    // rounded up from the makeup. One function so paint and rebuild cannot
    // disagree about where the top of the plot is.
    float axisTopDb() const noexcept;

    // Every colour in the plot is multiplied by this. Folds together the two
    // independent reasons to fade — bypassed, and not-the-selected-band — so
    // they compose instead of one overwriting the other.
    float plotAlpha() const noexcept;

    void rebuildPath();

    echojay::GainCurve curve_;
    float makeupDb_    = 0.0f;
    float floorDb_     = -60.0f;
    float inDb_        = kNoLevel;
    float grDb_        = 0.0f;
    float hysteresisDb_ = kNoHysteresis;
    float ceilingDb_    = kNoCeiling;
    float axisTopDb_    = kAutoAxisTop;

    // True unless a device explicitly says otherwise, so the five single-curve
    // faces never have to think about it.
    bool  selected_     = true;

    // Rebuilt on a curve or size change only. Repainting is cheap; recomputing
    // 200 gain-computer evaluations at 60 Hz for a curve that has not moved is
    // not, and six dynamics editors can be open at once.
    //
    // The curve is kept as POINTS as well as a path: the path draws the cold
    // diagram in one stroke, and the points are what the glow's per-pixel walk
    // reads its y out of.
    juce::Path curvePath_, fillPath_;
    std::vector<juce::Point<float>> curvePts_;
    std::vector<float>              curveInDb_;

    // ---- the dwell glow ----------------------------------------------------
    DwellGlow glow_;
    double    lastTickMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransferCurveView)
};

} // namespace echojay::viz
