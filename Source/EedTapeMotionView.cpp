/*
    EedTapeMotionView.cpp  —  see EedTapeMotionView.h.
*/

#include "EedTapeMotionView.h"

using namespace echojay::viz;

// Qualified alias, not `using namespace`: unqualified `Colours` is ambiguous
// against juce::Colours, which JuceHeader.h pulls into scope. Same convention as
// DeviceEditorBase and SurgicalEqEditor.
using C = echojay::viz::Colours;

EedTapeMotionView::EedTapeMotionView()
{
    setCaption ("TRANSPORT");
    trail_.fill (0.0f);
}

void EedTapeMotionView::setOffset (float normalised)
{
    const float v = juce::jlimit (-1.0f, 1.0f,
                                  std::isfinite (normalised) ? normalised : 0.0f);

    // The trail advances every tick even when the needle is still: a stopped
    // needle with a frozen trail and a stopped needle with a trail draining
    // away look different, and the second one is the truth (the wobble is off,
    // it is not merely paused).
    trail_[(std::size_t) head_] = v;
    head_ = (head_ + 1) % kTrail;
    if (filled_ < kTrail) ++filled_;

    // Repaint gate: the needle at rest must not cost a frame. The threshold is
    // well under a pixel of travel on any strip the rack will give us.
    const bool stillDraining = filled_ < kTrail;
    if (! stillDraining && ! moved (v, current_, 0.002f)) return;

    current_ = v;
    repaint();
}

void EedTapeMotionView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a  = dimAlpha();
    const float cy = plot.getCentreY();
    const float cx = plot.getCentreX();
    const float halfW = plot.getWidth() * 0.5f;

    // ---- the rail, and the centre the transport sits at when it is still ---
    g.setColour (C::border2.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), cy - 0.25f, plot.getWidth(), 0.5f);

    g.setColour (C::text3.withMultipliedAlpha (0.5f * a));
    g.fillRect (cx - 0.25f, plot.getY() + 1.0f, 0.5f, plot.getHeight() - 2.0f);

    // ---- the trail ---------------------------------------------------------
    // Oldest first, fading in: the shape of the last second and a half of drift.
    // Wow and flutter are slow, and a single dot cannot show a direction.
    for (int i = 0; i < filled_; ++i)
    {
        // head_ points at the NEXT slot to write, so the oldest sample is there.
        const int  slot = (head_ + i) % kTrail;
        const float age = (float) i / (float) juce::jmax (1, filled_ - 1);   // 0 old, 1 new
        const float x   = cx + trail_[(std::size_t) slot] * halfW;

        g.setColour (C::blue2.withAlpha (0.05f + 0.35f * age * age * a));
        g.fillEllipse (x - 1.5f, cy - 1.5f, 3.0f, 3.0f);
    }

    // ---- the needle --------------------------------------------------------
    const float x = cx + current_ * halfW;

    if (! isDimmed())
    {
        g.setColour (C::amber.withAlpha (0.20f));
        g.fillEllipse (x - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    }

    g.setColour (C::amber.withMultipliedAlpha (a));
    g.fillEllipse (x - 2.5f, cy - 2.5f, 5.0f, 5.0f);
}
