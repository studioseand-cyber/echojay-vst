/*
    EedDeEsserEditor.h  —  the editor for "EchoJay De-Esser".

    Five dials and a gain-reduction meter from EedDynamicsFaceEditor, plus the
    two switches the device needs. Those go in the HEADER, inboard of BYPASS,
    where DeviceEditorBase already puts a device's global toggles (it is where
    the EQ's analyzer switches live) — they are modes, not values, and putting
    them in the dial row would read as two knobs that happen to be buttons.

    LISTEN is a monitoring mode that makes the device output something other than
    its result, so the editor says so in the header hint while it is on. A device
    left in listen and forgotten is a mix that sounds broken for no visible
    reason.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedDeEsserProcessor.h"

class EedDeEsserEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedDeEsserEditor (EedDeEsserProcessor& p);

protected:
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;
    void refreshExtras() override;

private:
    void pushSwitches();
    void refreshSwitchText();

    EedDeEsserProcessor& deEsser_;

    juce::TextButton modeBtn_, listenBtn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedDeEsserEditor)
};
