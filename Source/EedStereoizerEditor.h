/*
    EedStereoizerEditor.h  —  the editor for "EchoJay Stereoizer".

    Four dials, a goniometer and a layout; everything else is inherited from
    DeviceEditorBase. The dials are driven THROUGH the schema (setParamValue /
    getParamValue), so a knob turn and an AI move take the identical path.

    THE GONIOMETER (VISUALS_PLAN.md Phase V0) is the same ring-tap path Stereo
    Width proved: the editor reads EedStereoizerProcessor::scopeTap() on the
    timer it already runs and hands the frame to echojay::viz::Goniometer. The
    view is read-only — nothing about the dialable contract changes.

    It earns its place more here than on Stereo Width. Width scales side content
    that already exists, so the number at least predicts the direction. This
    device MANUFACTURES side content with a Haas delay, and how much image 15 ms
    actually buys depends entirely on the source: a mono synth spreads wide, an
    already-wide pad barely moves. The picture is the only honest answer, and it
    is why the tap is taken post-engine.

    EXPECT THE CORRELATION BAR TO GO NEGATIVE HERE, and do not read that as a
    fault. Goniometer.h says of Stereo Width that a mono-safe device "cannot
    produce -1 by design"; that is fair for scaling side content that already
    exists, but it does not carry over to this device. Correlation compares side
    ENERGY against mid energy, while mono-safety is a statement about the SUM.
    A Haas widener manufactures side content, so past roughly 15 ms and 130% a
    mono source measures negative — the registry test pins it at about -0.36 for
    20 ms at 150% — while L + R remains bit-for-bit the input's fold-down. Both
    facts are true at once: the image is genuinely phasey, and nothing cancels
    when it is summed. The bar is reporting the first, and it should.
*/

#pragma once

#include "DeviceEditorBase.h"
#include "EedStereoizerProcessor.h"
#include "viz/Goniometer.h"

#include <array>

class EedStereoizerEditor : public DeviceEditorBase,
                            private juce::Timer
{
public:
    explicit EedStereoizerEditor (EedStereoizerProcessor& p);
    ~EedStereoizerEditor() override;

protected:
    void layoutContent (juce::Rectangle<int> content) override;

private:
    void timerCallback() override;
    void syncFromProcessor();
    void refreshScope();

    // One frame handed to the goniometer. Sized ONCE, here, and reused for the
    // life of the editor — the same discipline SurgicalEqEditor's FFT scratch
    // follows, for the same reason: nothing on a repaint path allocates.
    static constexpr int kScopeFrame = 1024;

    EedStereoizerProcessor& proc_;

    echojay::device::EchoJayDeviceKnob widthKnob_, haasKnob_, monoKnob_, mixKnob_;
    echojay::viz::Goniometer           scope_;

    std::array<float, (size_t) kScopeFrame> scopeL_ {}, scopeR_ {};

    // The mono-maker frequency currently written into the scope caption. Kept so
    // the caption string is only rebuilt when the crossover actually moves,
    // rather than allocated once per timer tick.
    int captionHz_ = -1;

    bool suppressCallbacks_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedStereoizerEditor)
};
