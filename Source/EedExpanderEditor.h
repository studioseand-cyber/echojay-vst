/*
    EedExpanderEditor.h  —  the editor for "EchoJay Expander".

    Five dials, a gain-reduction meter and a transfer curve on
    EedDynamicsFaceEditor.

    This is the face where the curve earns its space most quietly. The
    expander's two defining numbers — ratio and range — shape the same part of
    the sound, and neither is legible from a dial: 6:1 capped at 12 dB of range
    and 2:1 capped at 12 dB are identical above the threshold and completely
    different below it. The plot shows the slope AND where it stops, in one
    shape, which is the whole device.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedExpanderProcessor.h"
#include "viz/TransferCurveView.h"

class EedExpanderEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedExpanderEditor (EedExpanderProcessor& p);

protected:
    int  topContentHeight() const override;
    void layoutTopContent (juce::Rectangle<int> area) override;
    void refreshExtras() override;

private:
    EedExpanderProcessor&           proc_;
    echojay::viz::TransferCurveView curve_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedExpanderEditor)
};
