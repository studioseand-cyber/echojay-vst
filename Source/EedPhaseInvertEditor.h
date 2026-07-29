/*
    EedPhaseInvertEditor.h  —  the editor for "EchoJay Phase Invert".

    Two toggles on the shared look. Proves DeviceEditorBase works for a device
    whose controls are buttons rather than dials — the EQ and Gain both use
    filmstrip rotaries, and a base that only fitted knobs would not survive
    Wave 1 (a Gate's LISTEN, a Comp's auto-release).
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

private:
    void timerCallback() override;
    void syncFromProcessor();

    EedPhaseInvertProcessor& proc_;

    juce::TextButton leftBtn_  { "INVERT L" };
    juce::TextButton rightBtn_ { "INVERT R" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPhaseInvertEditor)
};
