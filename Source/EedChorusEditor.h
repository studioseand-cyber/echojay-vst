/*
    EedChorusEditor.h  —  the editor for "EchoJay Chorus".

    A list of params on EedModEditorBase; see EedTremoloEditor.h for the pattern.

    Two views, side by side, because a chorus is two things at once: an LFO
    (the scope) and the comb filter that LFO is sweeping (the SweepView). The
    scope shows the drift; the comb shows what the drift is doing to the tone,
    which is the half a user cannot get to from the dials.
*/

#pragma once

#include "EedChorusProcessor.h"
#include "EedModEditorBase.h"
#include "viz/LfoScopeView.h"
#include "viz/SweepView.h"

class EedChorusEditor : public EedModEditorBase
{
public:
    explicit EedChorusEditor (EedChorusProcessor& p);

protected:
    int   topContentHeight() const override;
    void  layoutTopContent (juce::Rectangle<int> area) override;
    void  refreshExtras() override;
    float lfoPhase() const override;

    // Dimension has no sweep, so the dials that shape one come off the panel.
    bool  controlVisible (const char* id) const override;

private:
    EedChorusProcessor& proc_;

    echojay::viz::LfoScopeView scope_;
    echojay::viz::SweepView    comb_;
    juce::String               lastHint_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedChorusEditor)
};
