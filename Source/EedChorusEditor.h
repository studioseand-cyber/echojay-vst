/*
    EedChorusEditor.h  —  the editor for "EchoJay Chorus".

    A list of params on EedModEditorBase; see EedTremoloEditor.h for the pattern.
*/

#pragma once

#include "EedChorusProcessor.h"
#include "EedModEditorBase.h"

class EedChorusEditor : public EedModEditorBase
{
public:
    explicit EedChorusEditor (EedChorusProcessor& p);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedChorusEditor)
};
