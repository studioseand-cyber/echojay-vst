/*
    EedCompressorEditor.h  —  the editor for "EchoJay Compressor".

    Seven dials and a gain-reduction meter, which is exactly the shape
    EedDynamicsFaceEditor provides — so this class is a knob table and a
    constructor. Everything that makes it look like EchoJay (logo, title, BYPASS,
    palette, filmstrip dials, inline hosting) comes from DeviceEditorBase, and
    the schema wiring comes from the face editor. Both are inherited, not
    re-authored.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedCompressorProcessor.h"

class EedCompressorEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedCompressorEditor (EedCompressorProcessor& p);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedCompressorEditor)
};
