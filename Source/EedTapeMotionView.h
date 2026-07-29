/*
    EedTapeMotionView.h  —  the transport's wobble, as a needle on a slim strip
    (VISUALS_PLAN.md Phase V1 Harmonic).

    Tape-specific, so it lives here rather than in Source/viz: nothing else in
    the suite has a transport, and a shared library should hold what is actually
    shared. It still subclasses viz::VizView, so it wears the same frame,
    caption and bypass dim as every other picture in the editor.

    WHY IT EARNS ITS PIXELS. Wow and flutter are the two controls on this device
    whose effect is inaudible on most material — on a sustained pad they are
    obvious, on a drum loop they are almost nothing, and a user who sets them on
    a drum loop cannot tell whether the knob is doing anything at all. The needle
    is the difference between "this control is broken" and "this control is
    working, on material that does not show it".

    It reads TapeEngine::transportOffset(), which is the position the modulated
    delay line is genuinely reading at — not a second copy of the LFOs running
    in the editor. A re-synthesised needle would drift out of phase with the
    audio within seconds and be worse than none, because it would look right.

    Cheap by construction: one float per timer tick, and the trail is a small
    ring of past positions, so the whole view costs a repaint of a 20-pixel strip.
*/

#pragma once

#include "viz/VizView.h"

#include <array>

class EedTapeMotionView : public echojay::viz::VizView
{
public:
    EedTapeMotionView();

    // -1..+1 around the transport's centre delay. Values outside are clamped
    // rather than rejected, so a future deeper modulation cannot draw outside
    // the frame.
    void setOffset (float normalised);

protected:
    void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override;

private:
    // ~1.6 s of history at the editor's 15 Hz timer, which is two and a half
    // cycles of the 0.6 Hz wow — enough to read the drift as a shape rather
    // than as a twitch.
    static constexpr int kTrail = 24;

    std::array<float, kTrail> trail_ {};
    int   head_    = 0;
    int   filled_  = 0;
    float current_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTapeMotionView)
};
