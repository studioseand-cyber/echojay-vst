/*
    SweepView.cpp  —  see SweepView.h.
*/

#include "SweepView.h"

namespace echojay::viz
{

namespace
{
    // Notch width, in octaves either side. Wide enough to be visible at any
    // rack width, narrow enough that four of them do not merge into one dip.
    constexpr float kNotchOctaves = 0.28f;

    constexpr int kResolution = 140;

    const float kTicks[] { 100.0f, 1000.0f, 10000.0f };

    juce::String tickText (float f)
    {
        return f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k"
                            : juce::String ((int) f);
    }
}

SweepView::SweepView()
{
    setCaption ("SWEEP");
}

void SweepView::setNotches (const float* freqsHz, int n, float depthDb)
{
    const int count = juce::jlimit (0, (int) kMaxNotches, n);

    bool changed = (count != numNotches_) || moved (depthDb, depthDb_, 0.1f);

    for (int i = 0; i < count && freqsHz != nullptr; ++i)
    {
        const float f = juce::jlimit (kMinFreq, kMaxFreq, freqsHz[i]);

        // The gate is on the LOG frequency, because that is what the axis
        // shows: 20 Hz of movement at 8 kHz is invisible, and at 60 Hz it is a
        // third of the plot.
        if (freqs_[i] <= 0.0f || std::abs (std::log (f / freqs_[i])) > 0.01f)
            changed = true;

        freqs_[i] = f;
    }

    numNotches_ = count;
    depthDb_    = depthDb;

    if (changed) repaint();
}

void SweepView::setSweep (float centreHz, int numStages, float depthDb)
{
    // A cascade of allpass stages summed with the dry signal notches at odd
    // multiples of the centre — 1x, 3x, 5x ... — which is why a phaser's
    // notches spread apart as they climb rather than staying evenly spaced.
    const int n = juce::jlimit (1, (int) kMaxNotches, numStages / 2);

    float f[kMaxNotches];
    for (int i = 0; i < n; ++i)
        f[i] = centreHz * (float) (2 * i + 1);

    setNotches (f, n, depthDb);
}

float SweepView::magnitudeDbAt (float freqHz) const noexcept
{
    float db = 0.0f;

    for (int i = 0; i < numNotches_; ++i)
    {
        if (freqs_[i] <= 0.0f) continue;

        // Octaves away from this notch, as a Gaussian well.
        const float oct = std::log2 (freqHz / freqs_[i]);
        const float t   = oct / kNotchOctaves;
        db += depthDb_ * std::exp (-t * t);
    }

    return db;
}

void SweepView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a = dimAlpha();

    const float logMin = std::log (kMinFreq), logMax = std::log (kMaxFreq);
    auto xForFreq = [&] (float f)
    {
        return plot.getX() + plot.getWidth()
               * (std::log (juce::jlimit (kMinFreq, kMaxFreq, f)) - logMin)
               / (logMax - logMin);
    };

    // The vertical scale is fixed at +-18 dB, the EQ's own default range, so a
    // phaser's dip and an EQ cut of the same depth look the same size.
    constexpr float kRangeDb = 18.0f;
    auto yForDb = [&] (float db)
    {
        return plot.getCentreY()
               - plot.getHeight() * 0.5f * juce::jlimit (-1.0f, 1.0f, db / kRangeDb);
    };

    // ---- grid --------------------------------------------------------------
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.fillRect (plot.getX(), yForDb (0.0f) - 0.25f, plot.getWidth(), 0.5f);

    g.setColour (Colours::border.withMultipliedAlpha (a));
    g.setFont (uiFont (7.0f));
    for (float f : kTicks)
    {
        const float x = xForFreq (f);
        g.setColour (Colours::border.withMultipliedAlpha (a));
        g.fillRect (x, plot.getY(), 0.5f, plot.getHeight());

        if (plot.getHeight() > 34.0f)
        {
            g.setColour (Colours::text3.withMultipliedAlpha (0.7f * a));
            g.drawText (tickText (f), (int) x - 16, (int) plot.getBottom() - 9, 32, 9,
                        juce::Justification::centred);
        }
    }

    if (numNotches_ <= 0) return;

    // ---- the response ------------------------------------------------------
    juce::Path curve, fill;
    for (int i = 0; i < kResolution; ++i)
    {
        const float t = (float) i / (float) (kResolution - 1);
        const float f = std::exp (logMin + t * (logMax - logMin));
        const juce::Point<float> p { xForFreq (f), yForDb (magnitudeDbAt (f)) };

        if (i == 0) { curve.startNewSubPath (p); fill.startNewSubPath (p.x, yForDb (0.0f)); fill.lineTo (p); }
        else        { curve.lineTo (p); fill.lineTo (p); }
    }
    fill.lineTo (plot.getRight(), yForDb (0.0f));
    fill.closeSubPath();

    g.setColour (Colours::purple.withAlpha (0.14f * a));
    g.fillPath (fill);

    g.setColour (Colours::blue2.withMultipliedAlpha (a));
    g.strokePath (curve, juce::PathStrokeType (1.75f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // ---- where the notches actually are ------------------------------------
    // The curve alone reads as "a wobble"; the markers are what make the
    // movement legible as notches climbing and falling together.
    if (isDimmed()) return;

    for (int i = 0; i < numNotches_; ++i)
    {
        const float x = xForFreq (freqs_[i]);
        const float y = yForDb (magnitudeDbAt (freqs_[i]));
        g.setColour (Colours::amber.withAlpha (0.75f));
        g.fillEllipse (x - 2.0f, y - 2.0f, 4.0f, 4.0f);
    }
}

} // namespace echojay::viz
