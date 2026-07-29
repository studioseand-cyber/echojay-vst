/*
    EedReverbEditor.h  —  the editor for "EchoJay Reverb".

    Nine dials on DeviceEditorBase, in two rows: the four that define the SPACE
    (size, decay, predelay, mix) on top, the five that shape what happens inside
    it underneath. The EchoJay identity — logo, title, bypass, palette, filmstrip
    knobs, inline hosting — is inherited, not re-authored.

    Every dial is driven THROUGH the schema (setParamValue / getParamValue)
    rather than by calling the engine directly, so a knob turn and an AI move
    take the identical path and cannot disagree about clamping.

    The header hint reports the room in metres and the decay the tail ACTUALLY
    has — which is shorter than the decay knob at high damping, because the
    damping filter sits inside the feedback loop and shortens what it filters.
    Showing the asked-for number there would be a readout the ear disagrees with.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedReverbProcessor.h"

class EedReverbEditor : public DeviceEditorBase,
                        private juce::Timer
{
public:
    explicit EedReverbEditor (EedReverbProcessor& p);
    ~EedReverbEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();
    void pushKnob (const char* id, const echojay::device::EchoJayDeviceKnob& k);
    void refreshHint();

    EedReverbProcessor& proc_;

    echojay::device::EchoJayDeviceKnob sizeKnob_, decayKnob_, predelayKnob_, mixKnob_,
                                       dampingKnob_, lowCutKnob_, widthKnob_,
                                       earlyLateKnob_, modDepthKnob_;

    juce::String lastHint_;
    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedReverbEditor)
};
