/*
    EedCompressorEditor.h  —  the editor for "EchoJay Compressor".

    Seven dials, a gain-reduction meter and a transfer curve. The first two are
    exactly the shape EedDynamicsFaceEditor provides, so those stay a knob table
    and a constructor: everything that makes it look like EchoJay (logo, title,
    BYPASS, palette, filmstrip dials, inline hosting) comes from
    DeviceEditorBase, and the schema wiring from the face editor. Both are
    inherited, not re-authored.

    THE CURVE (VISUALS_PLAN.md Phase V0). This device is where both
    visualisation data paths are proved end to end:

      * ANALYTIC — the curve is drawn from threshold / ratio / knee, which the
        editor already reads through getParamValue, using the processor's OWN
        echojay::GainCurve. No processor change, no tap, and no possibility of
        the picture disagreeing with the DSP.
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
    int  topContentHeight() const override;
    void layoutTopContent (juce::Rectangle<int> area) override;
    void refreshExtras() override;

private:
    EedCompressorProcessor&        proc_;
    echojay::viz::TransferCurveView curve_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedCompressorEditor)
};
