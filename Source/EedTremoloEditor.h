/*
    EedTremoloEditor.h  —  the editor for "EchoJay Tremolo".

    The point of EedModEditorBase: this class is a LIST of params plus a picture.
    The EchoJay identity (logo, title, bypass, palette, filmstrip dials, inline
    hosting) comes from DeviceEditorBase, and the schema-driven dials, the
    RATE/SYNC/DIVISION interlock and the AI-move polling come from
    EedModEditorBase.
*/

#pragma once

#include "EedModEditorBase.h"
#include "EedTremoloProcessor.h"
#include "viz/LfoScopeView.h"

class EedTremoloEditor : public EedModEditorBase
{
public:
    explicit EedTremoloEditor (EedTremoloProcessor& p);

protected:
    int   topContentHeight() const override;
    void  layoutTopContent (juce::Rectangle<int> area) override;
    void  refreshExtras() override;
    float lfoPhase() const override;

private:
    EedTremoloProcessor& proc_;

    echojay::viz::LfoScopeView scope_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTremoloEditor)
};
