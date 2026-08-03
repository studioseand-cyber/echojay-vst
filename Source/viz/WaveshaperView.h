/*
    WaveshaperView.h  —  the saturation transfer curve, with the signal's level
    riding it (VISUALS_PLAN.md, Harmonic signature visualisation).

    Analytic, like the dynamics curve, and for the same reason: the shape is a
    pure function of the drive and the curve choice, both of which the editor
    already reads. What it takes is the SHAPING FUNCTION ITSELF, as a
    std::function<float(float)>, so a Harmonic device hands it
    echojay::harmonic::shape() bound to its own curve and drive. The picture is
    then the DSP's own transfer function rather than a lookalike, and a change
    to the shapers redraws with no edit here.

    Why the level dot matters more here than anywhere else: every saturation
    curve is nearly straight near zero (that is the "unity slope at zero" the
    harmonic core is built around), so a device driven at -30 dBFS is sitting on
    the linear part and doing nothing audible no matter what the DRIVE knob
    says. The dot is what makes that visible instead of mysterious.
*/

#pragma once

#include "VizView.h"

#include <functional>

namespace echojay::viz
{

class WaveshaperView : public VizView
{
public:
    WaveshaperView();

    // -1..+1 in, -1..+1 out. Must be cheap: it is evaluated ~160 times per
    // rebuild. Setting it always rebuilds — a std::function cannot be compared,
    // so a caller sets it when the curve or drive CHANGES, not every frame.
    void setTransfer (std::function<float(float)> fn);

    // The signal's current level, 0..1 linear (a peak or short-window RMS off a
    // FloatTap). Negative hides the dot.
    void setInputLevel (float linear);

    // Shown top-right — "TUBE", "TAPE", the device's own word for its curve.
    void setCurveName (const juce::String& n);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;
    void resized() override;

private:
    void rebuildPath();

    std::function<float(float)> transfer_;
    juce::String                curveName_;
    float                       level_ = -1.0f;
    juce::Path                  curvePath_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveshaperView)
};

} // namespace echojay::viz
