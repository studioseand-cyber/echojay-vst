/*
    EedAutoPanEditor.h  —  the editor for "EchoJay Auto Pan".

    A list of params on EedModEditorBase; see EedTremoloEditor.h for the pattern.
*/

#pragma once

#include "EedAutoPanProcessor.h"
#include "EedModEditorBase.h"

class EedAutoPanEditor : public EedModEditorBase
{
public:
    explicit EedAutoPanEditor (EedAutoPanProcessor& p);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedAutoPanEditor)
};
