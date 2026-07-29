/*
    EedLimiterEditor.h  —  the editor for "EchoJay Limiter".

    Three dials, a gain-reduction meter, and one line of text the other faces do
    not need: the latency the lookahead is currently costing. A device that
    silently delays the track is a device people distrust; showing the number
    turns it into an informed choice.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedLimiterProcessor.h"

class EedLimiterEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedLimiterEditor (EedLimiterProcessor& p);

protected:
    int  extraContentHeight() const override { return 14; }
    void layoutExtraContent (juce::Rectangle<int> area) override;
    void refreshExtras() override;

private:
    EedLimiterProcessor& limiter_;
    juce::Label          latencyLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterEditor)
};
