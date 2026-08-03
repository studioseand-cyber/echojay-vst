/*
    EedCompressorEditor.h  —  the editor for "EchoJay Compressor".

    Eleven dials, a gain-reduction meter and a transfer curve. Most of that is
    exactly the shape EedDynamicsFaceEditor provides, so it stays a knob table
    and a constructor: everything that makes it look like EchoJay (logo, title,
    BYPASS, palette, filmstrip dials, inline hosting) comes from
    DeviceEditorBase, and the schema wiring from the face editor. Both are
    inherited, not re-authored.

    THE MODE SELECTOR GOES IN THE HEADER, with the detector and auto-release
    switches, where DeviceEditorBase already puts a device's global toggles (it
    is where the EQ's analyzer switches and the de-esser's LISTEN live). They are
    MODES, not values: a character selector in the dial row would read as a
    twelfth knob, and the one thing this control has to communicate is that it
    changes what the other eleven do.

    The header hint then reports what the mode made of the dialled times — "punch
    - attack 2.5 ms, release 84 ms" — because the character SCALES them, and a
    dial reading 10 ms on a device running 2.5 is the kind of small lie that
    makes people stop trusting a readout.

    THE CURVE (VISUALS_PLAN.md Phase V0). This device is where both
    visualisation data paths are proved end to end:

      * ANALYTIC — the curve is drawn from threshold / ratio / knee, which the
        editor already reads through getParamValue, using the processor's OWN
        echojay::GainCurve. No processor change, no tap, and no possibility of
        the picture disagreeing with the DSP. It is drawn from the EFFECTIVE
        knee, not the dialled one, for the reason above.
      * THE DWELL GLOW — what lights that curve is
        DynamicsCore::detectorLevelDb(), polled on the timer the face editor
        already runs, on the same benign racy contract as the GR meter.

    Together they are the difference between a compressor you dial by faith and
    one where a threshold set 10 dB too low is visible before it is audible.
*/

#pragma once

#include "EedDynamicsFaceEditor.h"
#include "EedCompressorProcessor.h"
#include "viz/TransferCurveView.h"

class EedCompressorEditor : public EedDynamicsFaceEditor
{
public:
    explicit EedCompressorEditor (EedCompressorProcessor& p);

protected:
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;

    int  topContentHeight() const override;
    void layoutTopContent (juce::Rectangle<int> area) override;
    void refreshExtras() override;

private:
    void refreshHint();

    EedCompressorProcessor&         proc_;
    echojay::viz::TransferCurveView curve_;

    juce::ComboBox   modeBox_, detectorBox_;
    juce::TextButton autoRelBtn_;

    // The hint's own repaint gate — see refreshHint().
    juce::String lastHint_;

    // Guards the controls' callbacks while refreshExtras writes into them, so
    // following the processor cannot be mistaken for a user edit.
    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedCompressorEditor)
};
