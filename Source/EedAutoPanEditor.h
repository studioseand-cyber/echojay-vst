/*
    EedAutoPanEditor.h  —  the editor for "EchoJay Auto Pan".

    A list of params on EedModEditorBase; see EedTremoloEditor.h for the pattern.

    Two views rather than one, because a pan has two questions and the waveform
    only answers the first. The scope says WHAT and HOW FAST; the position strip
    says WHERE the two channels are right now and how far apart STEREO PHASE has
    pushed them — which is the parameter nothing else on the panel makes visible.
*/

#pragma once

#include "EedAutoPanProcessor.h"
#include "EedModEditorBase.h"
#include "viz/LfoScopeView.h"
#include "viz/PanPositionView.h"

class EedAutoPanEditor : public EedModEditorBase
{
public:
    explicit EedAutoPanEditor (EedAutoPanProcessor& p);

protected:
    int   topContentHeight() const override;
    void  layoutTopContent (juce::Rectangle<int> area) override;
    void  refreshExtras() override;
    float lfoPhase() const override;

private:
    EedAutoPanProcessor& proc_;

    echojay::viz::LfoScopeView    scope_;
    echojay::viz::PanPositionView position_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedAutoPanEditor)
};
