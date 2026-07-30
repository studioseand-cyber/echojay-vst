/*
    EedGateEditor.h  —  the editor for "EchoJay Gate".

    Six dials, a gain-reduction meter and a transfer curve on
    EedDynamicsFaceEditor. The meter reads a gate's reduction, which is what
    makes hysteresis and hold legible: with the meter you can watch the gate
    hold open through a decay instead of guessing whether the hold time is doing
    anything.

    THE CURVE IS A STEP, AND THE STEP HAS TWO EDGES. A gate is not a steep
    expander, and this picture is where that stops being a pedantic distinction:
    the plot is unity above the threshold and flat at -range below it, with the
    HYSTERESIS BAND drawn between the open edge (solid) and the close edge
    (dashed). Inside that band the gate holds whichever state it is already in,
    so there is deliberately no curve through it — a line there would claim a
    determinism the device does not have, and hysteresis is the one gate
    parameter nobody can hear themselves setting.

    Getting that right needed a DSP fix rather than a drawing decision:
    GainCurve's Gate branch used to run the EXPANDER slope, on a `ratio` this
    device never sets. It was dead code until something plotted it. See
    EedDynamicsCore.h.

    AND THE SAME PICTURE DRAWS THE DUCKER. `mode` flips which side of the
    threshold is attenuated, and the curve is drawn in whichever DynamicsMode the
    core is actually running — so the step turns over and the hysteresis band
    stays exactly where it was, which is correct: the band is "the signal is over
    the line, allowing for hysteresis" in both modes, and only the consequence
    differs.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedGateProcessor.h"
#include "viz/TransferCurveView.h"

class EedGateEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedGateEditor (EedGateProcessor& p);

protected:
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;

    int  topContentHeight() const override;
    void layoutTopContent (juce::Rectangle<int> area) override;
    void refreshExtras() override;

private:
    EedGateProcessor&               proc_;
    echojay::viz::TransferCurveView curve_;

    juce::ComboBox modeBox_;
    bool           suppressCallbacks_ = false;
    juce::String   lastHint_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGateEditor)
};
