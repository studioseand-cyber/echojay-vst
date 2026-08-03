/*
    EedLimiterEditor.h  —  the editor for "EchoJay Limiter".

    Three dials, a gain-reduction meter, a transfer curve, and one line of text
    the other faces do not need: the latency the lookahead is currently costing.
    A device that silently delays the track is a device people distrust; showing
    the number turns it into an informed choice.

    THE CEILING IS DRAWN AS A HARD LINE, over the curve rather than under it.
    The curve flattening at the ceiling is a CONSEQUENCE of the infinite ratio;
    the line is the PROMISE — nothing gets above this — and the promise is what
    the device is selling. Everything above it is shaded out, because an empty
    plot above a flat curve reads as headroom that is still available, which is
    the exact opposite of what a brick wall means.

    The curve is drawn in Limit mode with a HARD knee, matching the DSP:
    EedLimiterProcessor sets knee 0 deliberately, because a soft knee would
    start reducing below the ceiling, and that is a compressor.

    THE MODE SELECTOR HIDES WHAT IT DISABLES. `clip` runs an instantaneous gain
    and no delay, so RELEASE and LOOKAHEAD are both doing nothing — and a dial
    that turns, reads back and changes nothing is worse than a dial that is not
    there. Both come back the moment the mode does. They remain schema params
    throughout, so an AI move can set them while clip is selected and they will
    be waiting when it is not.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedLimiterProcessor.h"
#include "viz/TransferCurveView.h"

class EedLimiterEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedLimiterEditor (EedLimiterProcessor& p);

protected:
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;

    int  topContentHeight() const override;
    void layoutTopContent (juce::Rectangle<int> area) override;

    int  extraContentHeight() const override { return 14; }
    void layoutExtraContent (juce::Rectangle<int> area) override;

    bool knobVisible (int index) const override;
    void refreshExtras() override;

private:
    EedLimiterProcessor&            limiter_;
    echojay::viz::TransferCurveView curve_;
    juce::Label                     latencyLabel_;

    juce::ComboBox   modeBox_;
    juce::TextButton truePeakBtn_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterEditor)
};
