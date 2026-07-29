/*
    Goniometer.cpp  —  see Goniometer.h.
*/

#include "Goniometer.h"

namespace echojay::viz
{

namespace
{
    constexpr float kCorrBarH  = 12.0f;   // the correlation strip under the scope
    constexpr float kSilenceSq = 1.0e-9f; // sum-of-squares under this is silence
}

Goniometer::Goniometer()
{
    setCaption ("STEREO FIELD");
    points_.reserve ((size_t) kMaxPoints);
}

void Goniometer::setCorrelation (float c)
{
    const float v = juce::jlimit (-1.0f, 1.0f, c);
    if (! moved (v, corr_, 0.005f)) return;
    corr_ = v;
    repaint();
}

void Goniometer::setShowCorrelation (bool s)
{
    if (s == showCorr_) return;
    showCorr_ = s;
    repaint();
}

void Goniometer::setSamples (const float* left, const float* right, int numSamples)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    // Correlation over the WHOLE frame, not over the decimated points: the
    // number is a measurement and should not depend on how many pixels the
    // scope happens to have.
    double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = left[i], r = right[i];
        sumLR += l * r;
        sumLL += l * l;
        sumRR += r * r;
    }

    const bool nowSilent = (sumLL + sumRR) < kSilenceSq;

    // Silence twice running is not a new picture. This is the repaint gate that
    // makes an open editor on a stopped transport cost nothing.
    if (nowSilent && silent_)
        return;

    if (! nowSilent)
    {
        const double denom = std::sqrt (sumLL * sumRR);
        corr_ = denom > 1.0e-12 ? (float) juce::jlimit (-1.0, 1.0, sumLR / denom)
                                : 1.0f;    // one channel silent: nothing to disagree
    }
    else
    {
        corr_ = 1.0f;                      // silence folds down perfectly
    }

    // The bar chases the measurement rather than snapping to it: correlation on
    // real material jumps around inside a single note, and an unsmoothed bar is
    // a flicker with no value attached to it. Same reasoning as the GR meter's
    // peak hold.
    corrShown_ += (corr_ - corrShown_) * 0.25f;

    // Decimate to the display's budget. Stride, not average: averaging adjacent
    // samples would pull the Lissajous toward its own centre and make every
    // source look narrower than it is.
    const int stride = juce::jmax (1, numSamples / kMaxPoints);

    points_.clear();
    for (int i = 0; i < numSamples; i += stride)
    {
        const float mid  = 0.5f * (left[i] + right[i]);
        const float side = 0.5f * (left[i] - right[i]);

        // The 45-degree rotation is exactly this pair, and the sqrt(2) puts a
        // hard-panned channel (|L|=1, R=0) on the diagonal at unit radius
        // rather than at 0.707 — so the L and R guide lines mean full scale.
        points_.emplace_back (side * 1.41421356f, mid * 1.41421356f);
    }

    silent_ = nowSilent;
    repaint();
}

void Goniometer::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a = dimAlpha();

    juce::Rectangle<float> corrArea;
    if (showCorr_ && plot.getHeight() > kCorrBarH * 3.0f)
    {
        corrArea = plot.removeFromBottom (kCorrBarH);
        plot.removeFromBottom (3.0f);
    }

    // The scope is SQUARE. A stretched vectorscope reports a width the signal
    // does not have, which is the one thing this view exists to get right.
    const float side  = juce::jmin (plot.getWidth(), plot.getHeight());
    const auto  scope = plot.withSizeKeepingCentre (side, side);
    const auto  c     = scope.getCentre();
    const float rad   = side * 0.5f;

    // ---- graticule ---------------------------------------------------------
    g.setColour (Colours::border.withMultipliedAlpha (a));
    g.drawEllipse (scope.reduced (0.5f), 0.5f);
    g.drawEllipse (scope.withSizeKeepingCentre (side * 0.5f, side * 0.5f), 0.5f);

    // M vertical, S horizontal.
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.fillRect (c.x - 0.25f, scope.getY(), 0.5f, side);
    g.fillRect (scope.getX(), c.y - 0.25f, side, 0.5f);

    // The L and R diagonals, labelled. Without them the picture has no
    // orientation and "it leans left" cannot be said about it.
    {
        const float d = rad * 0.7071f;
        g.setColour (Colours::border2.withMultipliedAlpha (0.8f * a));
        g.drawLine (c.x - d, c.y - d, c.x + d, c.y + d, 0.5f);
        g.drawLine (c.x + d, c.y - d, c.x - d, c.y + d, 0.5f);

        if (side > 60.0f)
        {
            g.setColour (Colours::text3.withMultipliedAlpha (0.7f * a));
            g.setFont (uiFont (7.5f, true));
            g.drawText ("L", (int) (c.x - d) - 12, (int) (c.y - d) - 10, 12, 10,
                        juce::Justification::centredRight);
            g.drawText ("R", (int) (c.x + d) + 1, (int) (c.y - d) - 10, 12, 10,
                        juce::Justification::centredLeft);
            g.drawText ("M", (int) c.x + 3, (int) scope.getY(), 12, 10,
                        juce::Justification::centredLeft);
        }
    }

    // ---- the trace ---------------------------------------------------------
    if (! isDimmed() && ! points_.empty())
    {
        // Individual dots with low alpha, not a joined path: the density IS the
        // information — where the energy sits — and joining consecutive samples
        // draws lines through the middle that no signal ever occupied.
        g.setColour (Colours::blue2.withAlpha (0.45f));

        for (const auto& p : points_)
        {
            const float x = c.x + juce::jlimit (-1.2f, 1.2f, p.x) * rad;
            const float y = c.y - juce::jlimit (-1.2f, 1.2f, p.y) * rad;
            g.fillRect (x - 0.75f, y - 0.75f, 1.5f, 1.5f);
        }
    }

    // ---- correlation -------------------------------------------------------
    if (corrArea.isEmpty()) return;

    g.setColour (Colours::bg2.withMultipliedAlpha (a));
    g.fillRoundedRectangle (corrArea, 1.5f);
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.drawRoundedRectangle (corrArea.reduced (0.5f), 1.5f, 0.5f);

    const float mid = corrArea.getCentreX();

    // Centre tick = 0 = uncorrelated. The bar grows from here in both
    // directions, so "which way is it leaning" is the first thing read.
    g.setColour (Colours::border2.withMultipliedAlpha (a));
    g.fillRect (mid - 0.25f, corrArea.getY() + 1.0f, 0.5f, corrArea.getHeight() - 2.0f);

    if (! isDimmed())
    {
        const float v   = juce::jlimit (-1.0f, 1.0f, corrShown_);
        const float len = std::abs (v) * (corrArea.getWidth() * 0.5f - 2.0f);

        // Green in phase, red out of phase, amber through the middle: the same
        // "amber means watch this" the GR meter and the EQ's dynamic bar use.
        const auto col = v >= 0.0f ? Colours::amber.interpolatedWith (Colours::green, v)
                                   : Colours::amber.interpolatedWith (Colours::red, -v);

        g.setColour (col.withAlpha (0.85f));
        g.fillRect (v >= 0.0f ? mid : mid - len,
                    corrArea.getY() + 2.0f, len, corrArea.getHeight() - 4.0f);

        if (corrArea.getWidth() > 70.0f)
        {
            g.setColour (Colours::text2);
            g.setFont (uiFont (8.0f, true));
            g.drawText (juce::String (v, 2), corrArea.reduced (4.0f, 0.0f),
                        juce::Justification::centredRight);
        }
    }
}

} // namespace echojay::viz
