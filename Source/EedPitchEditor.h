/*
    EedPitchEditor.h  —  the P0 face of "EchoJay Pitch": a detection DEBUG
    READOUT, nothing more (PITCH_CORRECTION_SPEC.md §9, P0).

    Three panels painted at poll rate: the detected note with its deviation on
    a +/-50 cent tuner bar, the raw numbers (f0, confidence, input RMS), and
    the octave-guard log (fires, rate over voiced hops) with a RESET. The
    voice_type selector — the one P0 control that matters — sits in the
    header. The pitch ribbon is P5; this face is scaffolding the corrector
    phases will replace, so it stays deliberately plain.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedPitchProcessor.h"

class EedPitchEditor : public DeviceEditorBase,
                       private juce::Timer
{
public:
    explicit EedPitchEditor (EedPitchProcessor& p);
    ~EedPitchEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;
    void paintContent (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void syncFromProcessor();

    void paintNotePanel  (juce::Graphics& g, juce::Rectangle<int> area,
                          const echojay::PitchReading& r);
    void paintNumbers    (juce::Graphics& g, juce::Rectangle<int> area,
                          const echojay::PitchReading& r);
    void paintGuardPanel (juce::Graphics& g, juce::Rectangle<int> area,
                          const echojay::PitchReading& r);

    EedPitchProcessor& proc_;

    juce::ComboBox   voiceBox_, trackBox_, formantBox_;
    juce::TextButton resetBtn_ { "RESET" };
    echojay::device::EchoJayDeviceKnob targetKnob_;

    juce::Rectangle<int> notePanel_, numbersPanel_, guardPanel_;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPitchEditor)
};
