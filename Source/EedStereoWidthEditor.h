/*
    EedStereoWidthEditor.h  —  the editor for "EchoJay Stereo Width".

    Three dials, a goniometer and a layout. The EchoJay identity — logo, title,
    bypass, palette, filmstrip knobs, inline hosting — is inherited from
    DeviceEditorBase, not re-authored.

    The dials are driven THROUGH the schema (setParamValue / getParamValue)
    rather than by calling the engine directly, so a knob turn and an AI move
    take the identical path and cannot disagree about clamping.

    THE GONIOMETER (VISUALS_PLAN.md Phase V0) proves the ring-tap path: the
    editor reads EedStereoWidthProcessor::scopeTap() on the timer it already
    runs and hands the frame to echojay::viz::Goniometer, which draws the
    Lissajous and the correlation. Nothing about the dialable contract changes —
    the view is read-only, the way every visualisation in the plan is.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedStereoWidthProcessor.h"
#include "viz/Goniometer.h"

#include <array>

class EedStereoWidthEditor : public DeviceEditorBase,
                             private juce::Timer
{
public:
    explicit EedStereoWidthEditor (EedStereoWidthProcessor& p);
    ~EedStereoWidthEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;
    void layoutHeaderLeading (juce::Rectangle<int>& bar) override;

private:
    void timerCallback() override;
    void syncFromProcessor();
    void refreshScope();
    void refreshModeState();

    bool multibandActive() const;

    // One frame handed to the goniometer. Sized ONCE, here, and reused for the
    // life of the editor — the same discipline SurgicalEqEditor's FFT scratch
    // follows, for the same reason: nothing on a repaint path allocates.
    static constexpr int kScopeFrame = 1024;

    EedStereoWidthProcessor& proc_;

    echojay::device::EchoJayDeviceKnob widthKnob_, bassMonoKnob_, trimKnob_;

    // The depth pass. The MODE decides which of these are on the panel at all:
    // `full` shows the single WIDTH dial, `multiband` swaps it for the three
    // band widths and their crossovers (rotation/bass-mono/trim stay put).
    echojay::device::EchoJayDeviceKnob rotationKnob_,
                                       widthLowKnob_, widthMidKnob_, widthHighKnob_,
                                       xoverLowKnob_, xoverHighKnob_;
    juce::ComboBox                     modeBox_;

    echojay::viz::Goniometer           scope_;

    std::array<float, (size_t) kScopeFrame> scopeL_ {}, scopeR_ {};

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedStereoWidthEditor)
};
