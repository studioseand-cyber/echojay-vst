/*
    EedDelayEditor.h  —  the editor for "EchoJay Delay".

    Eight dials, two toggles and a note-length selector, on DeviceEditorBase. The
    EchoJay identity — logo, title, bypass, palette, filmstrip knobs, inline
    hosting — is inherited, not re-authored.

    Every control is driven THROUGH the schema (setParamValue / getParamValue)
    rather than by calling the engine directly, so a knob turn and an AI move
    take the identical path and cannot disagree about clamping.

    Two bits of behaviour beyond "place N dials":

    * SYNC swaps which time control is live. With sync on the TIME dial is
      dimmed and the division selector drives the delay; with it off the reverse.
      Both stay visible: hiding one makes the layout jump, and a control that
      moves when you toggle something else is how mis-clicks happen.

    * The header hint shows the time ACTUALLY in force, derived from the engine
      rather than recomputed here — so when sync is on it reads the note length
      and the milliseconds that note works out to at the host's current tempo.

    The visualisation is echojay::viz::DelayTapsView (VISUALS_PLAN.md, Time), and
    it is ANALYTIC — no tap, no ring, no processor change. Where the echoes land
    and how loud each one is are a pure function of time, feedback and mix, all
    of which the editor already reads through the schema.

    It is fed the EFFECTIVE time, not the TIME dial: with sync on, the dial is
    ignored by the audio path, so drawing it would put a picture of one delay
    above a device playing another.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedDelayProcessor.h"
#include "viz/DelayTapsView.h"

class EedDelayEditor : public DeviceEditorBase,
                       private juce::Timer
{
public:
    explicit EedDelayEditor (EedDelayProcessor& p);
    ~EedDelayEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();
    void pushKnob (const char* id, const echojay::device::EchoJayDeviceKnob& k);
    void refreshSyncState();
    void refreshHint();
    void refreshTaps();

    EedDelayProcessor& proc_;

    echojay::viz::DelayTapsView taps_;

    echojay::device::EchoJayDeviceKnob timeKnob_, feedbackKnob_, mixKnob_,
                                       hpKnob_, lpKnob_,
                                       offsetKnob_, modRateKnob_, modDepthKnob_;

    juce::TextButton syncBtn_     { "SYNC" };
    juce::TextButton pingPongBtn_ { "PING-PONG" };
    juce::ComboBox   divisionBox_;
    juce::Label      divisionLabel_;

    juce::String lastHint_;
    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedDelayEditor)
};
