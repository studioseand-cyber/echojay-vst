/*
    DelayTapsView.h  —  a delay's echoes as decaying taps on a time axis
    (VISUALS_PLAN.md, Time: "Delay: DelayTapsView (analytic)").

    Fully analytic — no tap of any kind. Time, feedback and mix completely
    determine where the echoes land and how loud each one is:

        tap k lands at k * time, at mix * feedback^(k-1)

    which is the entire picture. What it buys is the thing a delay's knobs are
    worst at communicating: how LONG the repeats go on for. 70% feedback and 85%
    feedback look like a small knob move and are the difference between four
    audible repeats and twenty, and that is exactly the mistake an AI move can
    make invisibly.

    PING-PONG is drawn as taps alternating above and below the axis, because
    ping-pong is not a level change — it is which SIDE each repeat comes from,
    and there is no other honest way to show it.
*/

#pragma once

#include "VizView.h"

namespace echojay::viz
{

class DelayTapsView : public VizView
{
public:
    DelayTapsView();

    // Every number in the schema's own units: ms, percent, percent.
    void setTaps (float timeMs, float feedbackPct, float mixPct, bool pingPong);

    // How much time the axis shows. Defaults to auto: the window follows the
    // delay time so a 20 ms slap and a 2 s echo are both readable.
    void setWindowMs (float ms);      // <= 0 restores auto

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    float windowMs() const noexcept;

    static constexpr int kMaxTaps = 32;

    float timeMs_    = 350.0f;
    float feedback_  = 0.35f;    // 0..1
    float mix_       = 0.35f;    // 0..1
    bool  pingPong_  = false;
    float windowSet_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayTapsView)
};

} // namespace echojay::viz
