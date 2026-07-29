/*
    EedPhaserEditor.h  —  the editor for "EchoJay Phaser".

    A list of params on EedModEditorBase; see EedTremoloEditor.h for the pattern.

    The SweepView is the device. A phaser's whole identity is where its notches
    are and that they are climbing; STAGES and CENTER FREQ are numbers a user
    guesses at until they can see the notches those numbers put on the axis.
*/

#pragma once

#include "EedModEditorBase.h"
#include "EedPhaserProcessor.h"
#include "viz/LfoScopeView.h"
#include "viz/SweepView.h"

class EedPhaserEditor : public EedModEditorBase
{
public:
    explicit EedPhaserEditor (EedPhaserProcessor& p);

protected:
    int   topContentHeight() const override;
    void  layoutTopContent (juce::Rectangle<int> area) override;
    void  refreshExtras() override;
    float lfoPhase() const override;

private:
    EedPhaserProcessor& proc_;

    echojay::viz::LfoScopeView scope_;
    echojay::viz::SweepView    sweep_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPhaserEditor)
};
