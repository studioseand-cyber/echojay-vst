/*
    EedExpanderEditor.h  —  the editor for "EchoJay Expander".

    Five dials and a gain-reduction meter on EedDynamicsFaceEditor.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedExpanderProcessor.h"

class EedExpanderEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedExpanderEditor (EedExpanderProcessor& p);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedExpanderEditor)
};
