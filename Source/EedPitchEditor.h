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
#include <fstream>

#include "DeviceEditorBase.h"
#include "EedPitchProcessor.h"

class EedPitchEditor : public DeviceEditorBase,
                       private juce::Timer
{
public:
    explicit EedPitchEditor (EedPitchProcessor& p);
    ~EedPitchEditor() override;

    // Every param id this editor exposes as a HAND control. The UI-coverage
    // audit (tools/pitch_mode_test) walks the schema against this list: a
    // param the model can set and the user cannot see is invisible state,
    // and it will eventually surprise someone - measured, the SCALE combo
    // spent an unknown time squeezed to a 10-pixel sliver and a user
    // reported the panel "shows KEY only". Keep this list adjacent to the
    // truth it describes: when a control is added or removed in the .cpp,
    // this list moves in the same commit, and the audit's exemption ledger
    // names every param that remains UI-less and why.
    static const std::vector<const char*>& handControlledParams();

protected:
    void layoutContent (juce::Rectangle<int> content) override;
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;
    void paintContent (juce::Graphics& g) override;

private:
    void timerCallback() override;
    std::ofstream traceFile_;   // retune-trace CSV (3 Sep investigation)
    void syncFromProcessor();

    void paintNotePanel  (juce::Graphics& g, juce::Rectangle<int> area,
                          const echojay::PitchReading& r);
    void paintNumbers    (juce::Graphics& g, juce::Rectangle<int> area,
                          const echojay::PitchReading& r);
    void paintGuardPanel (juce::Graphics& g, juce::Rectangle<int> area,
                          const echojay::PitchReading& r);

    EedPitchProcessor& proc_;

    juce::ComboBox   voiceBox_, trackBox_, formantBox_, keyBox_, scaleBox_, modeBox_;
    juce::TextButton correctBtn_ { "CORRECT" };
    juce::TextButton vibBtn_     { "IGN VIB" };
    juce::TextButton keyAutoBtn_ { "AUTO" };
    // The reference control (29 Aug 2026): Sean was stuck on a grid he could
    // neither see the origin of nor change. The knob edits the MANUAL field
    // (turning it takes manual control); the AUTO button returns the mode;
    // provenance lives in the attribution line.
    juce::TextButton refAutoBtn_ { "AUTO" };

    // low_latency is a WORKFLOW choice (tracking vs mixing), so it reads as a
    // mode rather than a checkbox, and carries the number it costs.
    juce::TextButton latencyBtn_;
    juce::Rectangle<int> latencyBounds_, keyAttrBounds_;
    void paintKeyAttribution (juce::Graphics& g, juce::Rectangle<int> area);
    void paintLatencyMode (juce::Graphics& g, juce::Rectangle<int> area);
    void refreshLatencyButton();
    int  currentLatencyMs (bool lowLatency) const;
    juce::TextButton resetBtn_ { "RESET" };
    echojay::device::EchoJayDeviceKnob targetKnob_, retuneKnob_, flexKnob_, humanKnob_,
                                       depthKnob_, refKnob_;

    juce::Rectangle<int> notePanel_, numbersPanel_, guardPanel_, ribbonBounds_;

    bool suppressCallbacks_ = false;
    // Voice-fit readout (1 Sep 2026 ruling): running log-f0 evidence and
    // the suggested voice type - the fourth hidden-state-made-visible.
    double fitLogHz_ = 0.0; int fitN_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPitchEditor)
};
