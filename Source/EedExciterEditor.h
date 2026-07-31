/*
    EedExciterEditor.h  —  the editor for "EchoJay Exciter".

    Five dials, a mode selector, and the Harmonic cluster's signature pair
    (VISUALS_PLAN.md) — but both of them read the HIGH BAND rather than the
    whole signal, because that is the only part this device touches.

      * the curve is the one applied to the band above the split, including the
        amount crossfade, so at amount 0 it is drawn as the straight line it
        genuinely is;
      * the level dot rides that band's level, not the input's — a bass note
        with the split at 6 kHz barely reaches the curve, and the dot sitting
        near the origin is the honest picture of that;
      * the bars show what the device GENERATED (wet - dry), measured against
        the input's fundamental, so they read as "how much harmonic content was
        added" and fall to nothing when AMOUNT is down.

    Every control is driven THROUGH the schema (setParamValue / getParamValue).
    The visuals are READ-ONLY.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedExciterProcessor.h"
#include "viz/HarmonicBars.h"
#include "viz/WaveshaperView.h"

#include <vector>

class EedExciterEditor : public DeviceEditorBase,
                         private juce::Timer
{
public:
    explicit EedExciterEditor (EedExciterProcessor& p);
    ~EedExciterEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();

    // Only when the mode or the amount has moved — see the note on the
    // equivalent in EedSaturationEditor.
    void refreshTransfer();
    void refreshBars();

    EedExciterProcessor& proc_;

    echojay::device::EchoJayDeviceKnob freqKnob_, amountKnob_, focusKnob_, mixKnob_, outKnob_;
    juce::ComboBox                     modeBox_;

    echojay::viz::WaveshaperView shaper_;
    echojay::viz::HarmonicBars   bars_;

    // Two frames: the dry input and the generated content, sample aligned.
    // Sized ONCE here and reused, so the timer never allocates.
    std::vector<float> dryFrame_, genFrame_;

    int   lastMode_    = -1;
    float lastAmount_  = -1.0f;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedExciterEditor)
};
