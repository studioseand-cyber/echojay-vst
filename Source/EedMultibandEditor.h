/*
    EedMultibandEditor.h  —  the editor for "EchoJay 4-Band Compressor".

    The one Dynamics face that is NOT EedDynamicsFaceEditor. Four bands times
    seven knobs is twenty-eight controls, and laying all of them out at once
    would need a window several times the size the chain rack gives an inline
    editor — the sizing contract in DeviceEditorBase is not a suggestion.

    So it is the standard multiband shape instead:

      * three CROSSOVER dials across the top, because they define what the four
        bands even are,
      * a row of four BAND buttons, each showing that band's live gain reduction
        in its own small meter, so you can see all four working while editing one,
      * the SELECTED band's six dials underneath, plus its bypass.

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

    static constexpr int kNumBands   = EedMultibandProcessor::kNumBands;
    static constexpr int kBandKnobs  = 6;    // the dials; bypass is a button

    EedMultibandProcessor& proc_;

    echojay::device::EchoJayDeviceKnob crossoverKnobs_[3];
    echojay::device::EchoJayDeviceKnob bandKnobs_[kBandKnobs];

    juce::TextButton bandButtons_[kNumBands];
    juce::TextButton bandBypassBtn_;

    // Owned so they can be re-pointed at another band's meters on selection.
    juce::OwnedArray<echojay::device::GrMeter> bandMeters_;

    // The KnobSpec table for the selected band. Rebuilt on selection because the
    // ids carry the band number, and the ids are what bind a dial to the schema.
    echojay::device::KnobSpec bandSpecs_[kBandKnobs];
    juce::String              bandSpecIds_[kBandKnobs];   // backs the char* above

    int  selectedBand_ = 0;
    bool suppressCallbacks_ = false;

    juce::Rectangle<int> bandRowBounds_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedMultibandEditor)
};
