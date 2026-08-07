/*
    EedKeyDetectorEditor.h  —  the Key Detector's face (KEY_DETECTOR_SPEC §6).

    A note wheel: 12 segments, one per pitch class, brightness from the live
    chroma through the SAME heat treatment as the Dynamics family's dwell glow
    (DwellGlow::heatColour + the same easing time constant), so the analysis
    devices visibly belong to the same instrument as the dynamics ones. The
    detected key sits large in the middle, confidence as a meter, alternates
    small, ANALYSE / HOLD / RESET, and the dial row.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedKeyDetectorProcessor.h"
#include "viz/DwellGlow.h"

class EedKeyDetectorEditor : public DeviceEditorBase,
                             private juce::Timer
{
public:
    explicit EedKeyDetectorEditor (EedKeyDetectorProcessor& p);
    ~EedKeyDetectorEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;
    void paintContent (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void syncFromProcessor();
    void paintWheel (juce::Graphics& g, juce::Rectangle<float> area);
    void paintReadout (juce::Graphics& g, juce::Rectangle<int> area);

    EedKeyDetectorProcessor& proc_;

    // dials
    echojay::device::EchoJayDeviceKnob windowKnob_, sensKnob_, tuningKnob_,
                                       lowKnob_, highKnob_;
    // actions / switches
    juce::TextButton analyseBtn_ { "ANALYSE" };
    juce::TextButton holdBtn_    { "HOLD" };
    juce::TextButton resetBtn_   { "RESET" };
    juce::TextButton contBtn_    { "CONT" };
    juce::TextButton autoTuneBtn_{ "AUTO TUNE" };
    juce::TextButton hpssBtn_    { "HPSS" };
    juce::ComboBox   modeLockBox_;

    juce::Rectangle<int> wheelBounds_, readoutBounds_;

    // The wheel's easing state — same idea (and time constant) as
    // DwellGlow::tick, per pitch class instead of per dB bin.
    std::array<float, 12> chromaShown_ {};
    double lastTickMs_ = 0.0;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedKeyDetectorEditor)
};
