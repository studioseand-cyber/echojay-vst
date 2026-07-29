/*
    EedTremoloEditor.h  —  the editor for "EchoJay Tremolo".

    The point of EedModEditorBase: this class is a LIST of params. The EchoJay
    identity (logo, title, bypass, palette, filmstrip dials, inline hosting) comes
    from DeviceEditorBase, and the schema-driven dials, the RATE/SYNC/DIVISION
    interlock and the AI-move polling come from EedModEditorBase.
*/

#pragma once

#include "EedModEditorBase.h"
#include "EedTremoloProcessor.h"

class EedTremoloEditor : public EedModEditorBase
{
public:
    explicit EedTremoloEditor (EedTremoloProcessor& p);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTremoloEditor)
};
