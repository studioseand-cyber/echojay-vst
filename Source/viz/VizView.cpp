/*
    VizView.cpp  —  see VizView.h.
*/

#include "VizView.h"

namespace echojay::viz
{

namespace
{
    constexpr float kCorner   = 3.0f;
    constexpr float kInset    = 4.0f;
    constexpr float kCaptionH = 11.0f;
}

VizView::VizView()
{
    // A visualisation is a readout, not a control. Letting it swallow clicks
    // would put a dead zone over the top of a device editor.
    setInterceptsMouseClicks (false, false);
}

void VizView::setCaption (const juce::String& c)
{
    if (c == caption_) return;
    caption_ = c;
    repaint();
}

void VizView::setDimmed (bool d)
{
    if (d == dimmed_) return;
    dimmed_ = d;
    repaint();
}

void VizView::setFramed (bool f)
{
    if (f == framed_) return;
    framed_ = f;
    repaint();
}

juce::Rectangle<float> VizView::plotArea() const
{
    auto plot = getLocalBounds().toFloat().reduced (kInset);

    if (caption_.isNotEmpty() && plot.getHeight() > kCaptionH * 2.0f)
        plot.removeFromTop (kCaptionH);

    return plot;
}

bool VizView::hasPlot() const
{
    const auto p = plotArea();
    return p.getWidth() >= kMinPlot && p.getHeight() >= kMinPlot;
}

void VizView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (r.getWidth() < 2.0f || r.getHeight() < 2.0f) return;

    if (framed_)
    {
        g.setColour (Colours::bg3);
        g.fillRoundedRectangle (r, kCorner);
        g.setColour (Colours::border2);
        g.drawRoundedRectangle (r.reduced (0.5f), kCorner, 1.0f);
    }

    if (caption_.isNotEmpty())
    {
        auto cap = r.reduced (kInset).withHeight (kCaptionH);
        if (r.getHeight() > kCaptionH * 2.0f + kInset * 2.0f)
        {
            g.setColour (Colours::text3.withAlpha (dimmed_ ? 0.35f : 0.75f));
            g.setFont (uiFont (8.0f, true));
            g.drawText (caption_, cap, juce::Justification::centredLeft);
        }
    }

    const auto plot = plotArea();

    // The guard the whole library relies on: a collapsed rack slot, and the
    // harness's deliberate 1x1 paint, stop HERE rather than inside nine
    // different subclasses' coordinate maths.
    if (plot.getWidth() < kMinPlot || plot.getHeight() < kMinPlot) return;

    // Clipped, so a subclass that computes a point slightly outside its plot
    // (a live dot at the very top of its range) cannot draw over the frame.
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (plot.getSmallestIntegerContainer());

    paintPlot (g, plot);
}

} // namespace echojay::viz
