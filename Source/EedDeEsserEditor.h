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

    TWO VIEWS, because this device has two questions and they are not the same
    one. The other five Dynamics faces ask only "how much", and a transfer curve
    answers that. A de-esser also asks WHERE, and that is the setting it lives or
    dies on — freq_hz a few hundred Hz too low ducks the vowel instead of the
    "s", which sounds like a bad mix rather than a wrong number, and is invisible
    on a dial and on a transfer curve alike.

    So the top strip carries both:

        [ TRANSFER (how much) | BAND (where) ]

    The transfer curve is the narrower of the two and is the first to go when the
    rack gives the editor less width, because the band view is this device's
    signature — a de-esser without a frequency axis is a compressor with an
    unexplained filter in it.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedDeEsserProcessor.h"
#include "viz/DeEsserBandView.h"
#include "viz/TransferCurveView.h"

class EedDeEsserEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedDeEsserEditor (EedDeEsserProcessor& p);

protected:
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;

    int  topContentHeight() const override;
    void layoutTopContent (juce::Rectangle<int> area) override;

    void refreshExtras() override;

private:
    void refreshSwitchText();

    EedDeEsserProcessor& deEsser_;

    juce::TextButton modeBtn_, listenBtn_;

    echojay::viz::TransferCurveView curve_;
    echojay::viz::DeEsserBandView   band_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedDeEsserEditor)
};
