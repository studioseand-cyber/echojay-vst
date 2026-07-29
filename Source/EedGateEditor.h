/*
    EedGateEditor.h  —  the editor for "EchoJay Gate".

    Six dials and a gain-reduction meter on EedDynamicsFaceEditor. The meter
    reads a gate's reduction, which is what makes hysteresis and hold legible:
    with the meter you can watch the gate hold open through a decay instead of
    guessing whether the hold time is doing anything.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedGateProcessor.h"

class EedGateEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedGateEditor (EedGateProcessor& p);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGateEditor)
};
