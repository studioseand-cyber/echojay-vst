/*
    EedPhaserEditor.h  —  the editor for "EchoJay Phaser".

    A list of params on EedModEditorBase; see EedTremoloEditor.h for the pattern.
*/

#pragma once

#include "EedModEditorBase.h"
#include "EedPhaserProcessor.h"

class EedPhaserEditor : public EedModEditorBase
{
public:
    explicit EedPhaserEditor (EedPhaserProcessor& p);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPhaserEditor)
};
