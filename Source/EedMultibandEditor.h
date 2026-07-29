/*
    EedMultibandEditor.h  —  the editor for "EchoJay 4-Band Compressor".

    The one Dynamics face that is NOT EedDynamicsFaceEditor. Four bands times
    seven knobs is twenty-eight controls, and laying all of them out at once
    would need a window several times the size the chain rack gives an inline
    editor — the sizing contract in DeviceEditorBase is not a suggestion.

    So it is the standard multiband shape instead:

      * FOUR MINI TRANSFER CURVES across the top, one per band,
      * three CROSSOVER dials, because they define what the four bands even are,
      * a row of four BAND buttons, each showing that band's live gain reduction
        in its own small meter, so you can see all four working while editing one,
      * the SELECTED band's six dials underneath, plus its bypass.

    THE FOUR CURVES ARE FOUR, not one. This device is four independent
    compressors, and the failure it is easiest to ship is four bands set to
    numbers that look reasonable one at a time and fight each other together —
    band 2 pulling 9 dB while band 3 does nothing, which reads on the dials as a
    perfectly ordinary pair of settings. Four curves side by side, each with its
    own live dot, is the view that makes that visible at a glance, and it is why
    they are not folded into one plot with a band selector.

    A ROW, not a vertical stack. Four plots stacked in the height an inline rack
    slot has would be about 28 px each, which is not a curve — it is a smudge
    with a dot on it. Across, they sit directly over the four band buttons they
    belong to, which is also what makes the mapping obvious without labelling it
    twice.

    The selected band's curve is at full brightness and the other three are
    dimmed but still live, which is the whole of "tracking the selected band":
    you edit one and keep watching four.

    The band selector is a VIEW, not a parameter. It is not in the schema and is
    not saved: which band a human happens to be looking at is not part of what
    the device sounds like, and advertising it would invite the model to "select"
    a band instead of setting it. Every knob it reveals is dialable by id whether
    or not that band is on screen.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedDynamicsEditorSupport.h"
#include "viz/EedGrMeter.h"
#include "viz/TransferCurveView.h"
#include "EedMultibandProcessor.h"

class EedMultibandEditor : public DeviceEditorBase,
                           private juce::Timer
{
public:
    explicit EedMultibandEditor (EedMultibandProcessor& p);
    ~EedMultibandEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;
    void paintContent (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void selectBand (int band);
    void rebindBandKnobs();
    void syncAll();
    void refreshCurves();

    static constexpr int kNumBands   = EedMultibandProcessor::kNumBands;
    static constexpr int kBandKnobs  = 6;    // the dials; bypass is a button

    EedMultibandProcessor& proc_;

    echojay::device::EchoJayDeviceKnob crossoverKnobs_[3];
    echojay::device::EchoJayDeviceKnob bandKnobs_[kBandKnobs];

    juce::TextButton bandButtons_[kNumBands];
    juce::TextButton bandBypassBtn_;

    // Owned so they can be re-pointed at another band's meters on selection.
    juce::OwnedArray<echojay::device::GrMeter> bandMeters_;

    // One per band, always live. OwnedArray for the same reason the knobs are:
    // a VizView is non-copyable and non-movable by design.
    juce::OwnedArray<echojay::viz::TransferCurveView> bandCurves_;

    // The KnobSpec table for the selected band. Rebuilt on selection because the
    // ids carry the band number, and the ids are what bind a dial to the schema.
    echojay::device::KnobSpec bandSpecs_[kBandKnobs];
    juce::String              bandSpecIds_[kBandKnobs];   // backs the char* above

    int  selectedBand_ = 0;
    bool suppressCallbacks_ = false;

    juce::Rectangle<int> bandRowBounds_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedMultibandEditor)
};
