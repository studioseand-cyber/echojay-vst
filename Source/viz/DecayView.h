/*
    DecayView.h  —  a reverb's energy decay, early reflections and all
    (VISUALS_PLAN.md, Time: "Reverb: DecayView (analytic)").

    Analytic. RT60 is `decay_s` straight off the schema, the pre-delay is the
    gap before anything happens, and the early/late split is the one thing a
    reverb's knobs never show: SIZE moves where the early reflections land,
    DECAY moves how long the tail runs, and on knobs alone those two are the
    same gesture. Drawn, they are obviously different pictures.

    The envelope is the textbook one: -60 dB over RT60 seconds, so
    amplitude(t) = 10^(-3t/RT60). Damping tilts it — a damped tail loses its
    high end first and therefore its ENERGY faster — drawn as a second, faster
    envelope under the first rather than as a colour, because "the top end goes
    first" is a shape, not a shade.
*/

#pragma once

#include "VizView.h"

namespace echojay::viz
{

class DecayView : public VizView
{
public:
    DecayView();

    // Schema units throughout: seconds, ms, percent, percent.
    void setDecay (float decaySeconds, float predelayMs,
                   float dampingPct, float sizePct);

    // Scales the whole envelope, so a reverb at 10% wet does not draw itself as
    // loud as one at 100%.
    void setMixPercent (float pct);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    float windowSeconds() const noexcept;

    float decayS_    = 2.0f;
    float predelayS_ = 0.0f;
    float damping_   = 0.5f;    // 0..1
    float size_      = 0.5f;    // 0..1
    float mix_       = 1.0f;    // 0..1

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DecayView)
};

} // namespace echojay::viz
