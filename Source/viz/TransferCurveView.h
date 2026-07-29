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

    The optional live dot is the ONE float tap (VISUALS_PLAN.md, "One float tap"):
    the detector's current level in dB, polled on the editor's existing timer,
    the same benign racy single-float read as DynamicsCore::gainReductionDb.
    Without it the curve is a diagram; with it you watch the signal walk into the
    knee, which is what makes an AI's threshold move legible.
*/

#pragma once

#include "VizView.h"
#include "../EedDynamicsCore.h"

namespace echojay::viz
{

class TransferCurveView : public VizView
{
public:
    TransferCurveView();

    // Anything at or below this hides the live dot — the value a device passes
    // when it is bypassed or has never seen a sample.
    static constexpr float kNoLevel = -200.0f;

    // ---- the analytic part (no processor change) --------------------------
    // Exactly the four numbers the gain computer takes, in the schema's units.
    void setCurve (float thresholdDb, float ratio, float kneeDb, float rangeDb,
                   echojay::DynamicsMode mode);

    // Makeup lifts the whole output axis, which is the difference between "this
    // is pulling 6 dB off" and "this is pulling 6 dB off and handing it back".
    void setMakeupDb (float db);

    // How far down the axes run. -60 suits a compressor; a gate wants further.
    void setFloorDb (float db);

    // ---- the one-float-tap part -------------------------------------------
    // The detector's current level, dB. kNoLevel hides the dot.
    void setInputLevelDb (float db);

    // Purely for the readout beside the dot; the dot itself rides the static
    // curve so it reads as "where on the curve am I", not "where did the
    // ballistics get to".
    void setGainReductionDb (float db);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;
    void resized() override;

private:
    // Output dB for an input dB, INCLUDING makeup — the curve as heard.
    float outputDb (float inDb) const noexcept;

    void rebuildPath();

    echojay::GainCurve curve_;
    float makeupDb_ = 0.0f;
    float floorDb_  = -60.0f;
    float inDb_     = kNoLevel;
    float grDb_     = 0.0f;

    // Rebuilt on a curve or size change only. Repainting is cheap; recomputing
    // 200 gain-computer evaluations at 20 Hz for a curve that has not moved is
    // not, and six dynamics editors can be open at once.
    juce::Path curvePath_, fillPath_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransferCurveView)
};

} // namespace echojay::viz
