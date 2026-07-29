/*
    PanPositionView.cpp  —  see PanPositionView.h.
*/

#include "PanPositionView.h"

namespace echojay::viz
{

namespace
{
    // Below this the two dots overlap into one blob, so the strip says "both
    // channels are together" with a single marker instead of a smear.
    constexpr float kTogetherEpsilon = 0.02f;
}

PanPositionView::PanPositionView()
{
    setCaption ("POSITION");
}

void PanPositionView::setPositions (float panL, float panR)
{
    const float l = juce::jlimit (-1.0f, 1.0f, panL);
    const float r = juce::jlimit (-1.0f, 1.0f, panR);

    // 1/200th of the field is under a pixel on any strip a rack slot holds.
    if (! moved (l, panL_, 0.005f) && ! moved (r, panR_, 0.005f)) return;

    panL_ = l;
    panR_ = r;
    repaint();
}

void PanPositionView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a    = dimAlpha();
    const float midY = plot.getCentreY();

    auto xFor = [&] (float pan)
    {
        return plot.getCentreX() + plot.getWidth() * 0.5f * juce::jlimit (-1.0f, 1.0f, pan);
    };

    // ---- the field ---------------------------------------------------------
    g.setColour (Colours::border.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), midY - 0.5f, plot.getWidth(), 1.0f);

    // Centre, and the two ends. Marked because "how far off centre is it" is
    // the whole reading, and a bare line gives nothing to judge it against.
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    for (float p : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
    {
        const float h = (p == 0.0f) ? plot.getHeight() * 0.5f : plot.getHeight() * 0.25f;
        g.fillRect (xFor (p) - 0.25f, midY - h * 0.5f, 0.5f, h);
    }

    if (plot.getWidth() > 90.0f)
    {
        g.setColour (Colours::text3.withMultipliedAlpha (0.7f * a));
        g.setFont (uiFont (7.0f, true));
        g.drawText ("L", plot.withWidth (10.0f), juce::Justification::centredLeft);
        g.drawText ("R", plot.withTrimmedLeft (plot.getWidth() - 10.0f),
                    juce::Justification::centredRight);
    }

    if (isDimmed()) return;

    // ---- where the channels are -------------------------------------------
    const bool together = std::abs (panL_ - panR_) < kTogetherEpsilon;

    auto dot = [&] (float pan, juce::Colour c, float radius)
    {
        const float x = xFor (pan);
        g.setColour (c.withAlpha (0.20f));
        g.fillEllipse (x - radius * 2.0f, midY - radius * 2.0f, radius * 4.0f, radius * 4.0f);
        g.setColour (c);
        g.fillEllipse (x - radius, midY - radius, radius * 2.0f, radius * 2.0f);
    };

    if (together)
    {
        // One image, moving as a whole — which is what an auto-pan at 0 degrees
        // of stereo phase actually does.
        dot (panL_, Colours::blue2, 3.0f);
    }
    else
    {
        // The span between them IS the effect at any non-zero stereo phase, so
        // it is drawn as a bar rather than left to be inferred from two dots.
        const float x0 = juce::jmin (xFor (panL_), xFor (panR_));
        const float x1 = juce::jmax (xFor (panL_), xFor (panR_));
        g.setColour (Colours::blue2.withAlpha (0.18f));
        g.fillRect (x0, midY - 1.5f, x1 - x0, 3.0f);

        dot (panL_, Colours::blue2,  2.5f);
        dot (panR_, Colours::purple, 2.5f);
    }
}

} // namespace echojay::viz
