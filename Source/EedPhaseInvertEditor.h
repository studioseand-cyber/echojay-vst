/*
    EedPhaseInvertEditor.h  —  the editor for "EchoJay Phase Invert".

    Two toggles on the shared look. Proves DeviceEditorBase works for a device
    whose controls are buttons rather than dials — the EQ and Gain both use
    filmstrip rotaries, and a base that only fitted knobs would not survive
    Wave 1 (a Gate's LISTEN, a Comp's auto-release).

    VISUALISATION: a correlation dot on a -1..+1 track, and nothing else.
    VISUALS_PLAN.md says this device "stays minimal (a small correlation dot at
    most)", so it deliberately does NOT get a viz/ panel like every other device.
    There is no curve, spectrum or envelope in a sign flip, and framing a single
    number as though there were would be decoration pretending to be information.

    It is drawn straight into paintContent rather than as a VizView for the same
    reason: one dot and three ticks do not need a component, and the visual quiet
    of an unframed strip is the point — this device should look like the smallest
    thing in the rack, because it is.

    What the dot IS worth showing: polarity's whole hazard is mono fold-down, and
    correlation is the number that predicts it. Flip one side of a correlated
    pair and the dot swings to -1, which is the cancellation, visible before
    someone else's mono playback finds it.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedPhaseInvertProcessor.h"

class EedPhaseInvertEditor : public DeviceEditorBase,
                             private juce::Timer
{
public:
    explicit EedPhaseInvertEditor (EedPhaseInvertProcessor& p);
    ~EedPhaseInvertEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;
    void paintContent (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void syncFromProcessor();
    void refreshCorrelation();

    EedPhaseInvertProcessor& proc_;

    // Where paintContent draws the correlation strip, decided in layoutContent.
    // Empty when the slot is too short to fit it, which is how the strip drops
    // out rather than overlapping the buttons.
    juce::Rectangle<int> corrBounds_;

    // What is currently ON SCREEN, so the timer can repaint only when the dot
    // has actually moved. An idle editor costs nothing.
    float shownCorr_ = 2.0f;

    juce::TextButton leftBtn_  { "INVERT L" };
    juce::TextButton rightBtn_ { "INVERT R" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPhaseInvertEditor)
};
