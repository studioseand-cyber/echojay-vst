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
#include <vector>
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

    // THE SET (UI_SIMPLIFICATION.md rounds 41-44): FRONT = key (+AUTO badge),
    // scale, retune, flex, humanize, KEEP VIBRATO, ignore-vibrato, reference
    // (+AUTO badge), voice type, depth (until the re-mapped dial ships).
    // ADVANCED = mode, tracking, latency, correct, seam attack, mix, output,
    // formant shift (which drives formant_mode), the vibrato generator, and
    // the readouts. INTERNAL (no control) = formant_mode as a switch,
    // transpose, target_hz, reset_stats, ref_manual_by_user.
    // THE DIALABLE PRINCIPLE: every front control is a dial or an explicit
    // choice; AUTO is a badge on the control it governs, and turning the
    // control overrides it.
    juce::ComboBox   voiceBox_, trackBox_, keyBox_, scaleBox_, modeBox_, vibShapeBox_;
    juce::TextButton correctBtn_ { "CORRECT" };
    juce::TextButton vibBtn_     { "IGN VIB" };
    juce::TextButton keepVibBtn_ { "KEEP VIBRATO" };   // natural_vibrato as the two-state control it is (round 44); the Natural Vibrato name is reserved
    juce::TextButton advBtn_     { "ADVANCED" };       // shows the advanced panel; UI state only
    bool advanced_ = false;
public:
    // For the offline snapshot harness (tools/pitch_mode_test EJ_EDITOR_SNAP):
    // the panel a screenshot is taken of. Same path the ADVANCED button takes.
    void showAdvanced (bool on) { advBtn_.setToggleState (on, juce::dontSendNotification); advanced_ = on; resized(); repaint(); }
    void syncNow() { syncFromProcessor(); }   // what the 30 Hz timer does in a host
private:
    juce::TextButton keyAutoBtn_ { "AUTO" };           // the badge on KEY/SCALE
    // The reference control (29 Aug 2026): Sean was stuck on a grid he could
    // neither see the origin of nor change. The knob edits the MANUAL field
    // (turning it takes manual control); the AUTO button returns the mode;
    // provenance lives in the attribution line.
    juce::TextButton refAutoBtn_ { "AUTO" };

    // low_latency is a WORKFLOW choice (tracking vs mixing), so it reads as a
    // mode rather than a checkbox, and carries the number it costs.
    juce::TextButton latencyBtn_;
    juce::Rectangle<int> latencyBounds_, keyAttrBounds_;
    juce::Rectangle<int> offCurveBounds_;   // empty since round 49 (the strip is gone; the dial reads "(off)")
    juce::Rectangle<int> numbersBox_;       // round 49: the bounded numbers box at the top right of READOUTS   // round 46: the strip that explains an off-curve retune_speed_ms/depth
    // Round 47 (Sean's screenshot): every advanced control carries its own
    // caption, and the panel is three framed groups. Captions and frames are
    // laid out with the controls and painted from these lists, so a label can
    // never fall between another control's knob and its readout.
    struct Caption { juce::Rectangle<int> r; juce::String text; };
    struct Group   { juce::Rectangle<int> r; juce::String title; };
    std::vector<Caption> advCaptions_;
    std::vector<Group>   advGroups_;
    void paintKeyAttribution (juce::Graphics& g, juce::Rectangle<int> area);
    void paintLatencyMode (juce::Graphics& g, juce::Rectangle<int> area);
    void refreshLatencyButton();
    int  currentLatencyMs (bool lowLatency) const;
    echojay::device::EchoJayDeviceKnob retuneKnob_, flexKnob_, humanKnob_, depthKnob_, refKnob_,
                                       seamKnob_, mixKnob_, outKnob_, fshiftKnob_,
                                       vibDepthKnob_, vibRateKnob_, vibOnsetKnob_;

    juce::Rectangle<int> notePanel_, numbersPanel_, guardPanel_, ribbonBounds_;

    bool suppressCallbacks_ = false;
    // Voice-fit readout (1 Sep 2026 ruling): running log-f0 evidence and
    // the suggested voice type - the fourth hidden-state-made-visible.
    double fitLogHz_ = 0.0; int fitN_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPitchEditor)
};
