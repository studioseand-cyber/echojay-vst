/*
    WaveshaperView.cpp  —  see WaveshaperView.h.
*/

#include "WaveshaperView.h"

namespace echojay::viz
{

namespace
{
    constexpr int kResolution = 161;   // odd, so a point lands exactly on zero
}

WaveshaperView::WaveshaperView()
{
    setCaption ("CURVE");

    // A sane default so the view is never blank before a device wires itself
    // up: tanh is the textbook soft clipper and has the unity slope at zero
    // that every curve in EedHarmonicCore is built around.
    transfer_ = [] (float x) { return std::tanh (x); };
}

void WaveshaperView::setTransfer (std::function<float(float)> fn)
{
    transfer_ = fn ? std::move (fn) : std::function<float(float)> ([] (float x) { return x; });
    rebuildPath();
    repaint();
}

void WaveshaperView::setInputLevel (float linear)
{
    const float v = linear < 0.0f ? -1.0f : juce::jlimit (0.0f, 1.2f, linear);
    const bool wasVisible = level_ >= 0.0f, isVisible = v >= 0.0f;

    if (wasVisible == isVisible && ! moved (v, level_, 0.004f)) return;

    level_ = v;
    repaint();
}

void WaveshaperView::setCurveName (const juce::String& n)
{
    if (n == curveName_) return;
    curveName_ = n;
    repaint();
}

void WaveshaperView::resized()
{
    rebuildPath();
}

void WaveshaperView::rebuildPath()
{
    curvePath_.clear();
    if (! hasPlot() || ! transfer_) return;

    const auto plot = plotArea();

    for (int i = 0; i < kResolution; ++i)
    {
        const float x = -1.0f + 2.0f * (float) i / (float) (kResolution - 1);
        const float y = juce::jlimit (-1.2f, 1.2f, transfer_ (x));

        const juce::Point<float> p {
            plot.getCentreX() + x * plot.getWidth()  * 0.5f,
            plot.getCentreY() - y * plot.getHeight() * 0.5f
        };

        if (i == 0) curvePath_.startNewSubPath (p); else curvePath_.lineTo (p);
    }
}

void WaveshaperView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a = dimAlpha();
    const auto  c = plot.getCentre();

    // ---- graticule + the unity diagonal -----------------------------------
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), c.y - 0.25f, plot.getWidth(), 0.5f);
    g.fillRect (c.x - 0.25f, plot.getY(), 0.5f, plot.getHeight());

    {
        juce::Path unity, dashed;
        unity.startNewSubPath (plot.getX(), plot.getBottom());
        unity.lineTo (plot.getRight(), plot.getY());

        const float dashes[] { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, unity, dashes, 2);

        g.setColour (Colours::text3.withMultipliedAlpha (0.45f * a));
        g.fillPath (dashed);
    }

    if (curvePath_.isEmpty()) rebuildPath();

    g.setColour (Colours::blue2.withMultipliedAlpha (a));
    g.strokePath (curvePath_, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

    // ---- the level, riding the curve --------------------------------------
    // Drawn on BOTH sides: a waveshaper is symmetric about zero (or
    // deliberately is not, for the biased tube curve), and showing only the
    // positive half hides an asymmetry that is the entire character of the
    // device.
    if (! isDimmed() && level_ >= 0.0f && transfer_)
    {
        const float xs[] { level_, -level_ };

        for (float x : xs)
        {
            const float y = juce::jlimit (-1.2f, 1.2f, transfer_ (x));
            const float px = c.x + x * plot.getWidth()  * 0.5f;
            const float py = c.y - y * plot.getHeight() * 0.5f;

            g.setColour (Colours::amber.withAlpha (0.20f));
            g.fillEllipse (px - 5.0f, py - 5.0f, 10.0f, 10.0f);
            g.setColour (Colours::amber);
            g.fillEllipse (px - 2.5f, py - 2.5f, 5.0f, 5.0f);
        }
    }

    if (curveName_.isNotEmpty() && plot.getWidth() > 60.0f)
    {
        g.setColour (Colours::text3.withMultipliedAlpha (0.8f * a));
        g.setFont (uiFont (7.5f, true));
        g.drawText (curveName_, plot.reduced (2.0f, 1.0f), juce::Justification::topRight);
    }
}

} // namespace echojay::viz
