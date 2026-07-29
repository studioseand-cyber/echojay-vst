/*
    TransferCurveView.cpp  —  see TransferCurveView.h.
*/

#include "TransferCurveView.h"

namespace echojay::viz
{

namespace
{
    // A 12 dB grid: fine enough to read a threshold off, coarse enough that a
    // 120-pixel-tall plot in a shrunk rack slot is not a hatch pattern.
    constexpr float kGridStepDb = 12.0f;

    // Points across the plot. The gain computer is a handful of flops, so this
    // is cheap — and it is evaluated on a curve REBUILD, not on a repaint.
    constexpr int kResolution = 160;

    float ceilingFor (float makeupDb) noexcept
    {
        // Round the makeup up to the grid so the top gridline is a labelled
        // value rather than "+7". 0 dB stays the top when there is no makeup,
        // which is the usual case and the one worth optimising the picture for.
        if (makeupDb <= 0.0f) return 0.0f;
        return std::ceil (makeupDb / kGridStepDb) * kGridStepDb;
    }
}

TransferCurveView::TransferCurveView()
{
    setCaption ("TRANSFER");
}

void TransferCurveView::setCurve (float thresholdDb, float ratio, float kneeDb,
                                  float rangeDb, echojay::DynamicsMode mode)
{
    // The repaint gate. A knob sends its value continuously while dragged and
    // the editor's timer re-pushes it at 20 Hz whether it moved or not; without
    // this, every dynamics editor rebuilds a 160-point path five times a second
    // for a curve that is standing still.
    if (! moved (thresholdDb, curve_.thresholdDb, 0.01f)
        && ! moved (ratio,    curve_.ratio,       0.001f)
        && ! moved (kneeDb,   curve_.kneeDb,      0.01f)
        && ! moved (rangeDb,  curve_.rangeDb,     0.01f)
        && mode == curve_.mode)
        return;

    curve_.thresholdDb = thresholdDb;
    curve_.ratio       = std::max (ratio, 1.0f);
    curve_.kneeDb      = std::max (kneeDb, 0.0f);
    curve_.rangeDb     = std::max (rangeDb, 0.0f);
    curve_.mode        = mode;

    rebuildPath();
    repaint();
}

void TransferCurveView::setMakeupDb (float db)
{
    if (! moved (db, makeupDb_, 0.01f)) return;
    makeupDb_ = db;
    rebuildPath();
    repaint();
}

void TransferCurveView::setFloorDb (float db)
{
    const float f = std::min (db, -12.0f);
    if (! moved (f, floorDb_, 0.01f)) return;
    floorDb_ = f;
    rebuildPath();
    repaint();
}

void TransferCurveView::setInputLevelDb (float db)
{
    // A quarter of a dB is under a pixel on any plot this size, so anything
    // smaller is a repaint nobody can see. Sliding past kNoLevel in either
    // direction always repaints — that is the dot appearing or vanishing.
    const bool wasVisible = inDb_ > kNoLevel;
    const bool isVisible  = db    > kNoLevel;

    if (wasVisible == isVisible && ! moved (db, inDb_, 0.25f)) return;

    inDb_ = db;
    repaint();
}

void TransferCurveView::setGainReductionDb (float db)
{
    if (! moved (db, grDb_, 0.1f)) return;
    grDb_ = db;
    repaint();
}

float TransferCurveView::outputDb (float inDb) const noexcept
{
    // The DSP's own gain computer, not a redrawn approximation of it.
    return inDb + curve_.reductionDb (inDb) + makeupDb_;
}

void TransferCurveView::resized()
{
    rebuildPath();
}

void TransferCurveView::rebuildPath()
{
    curvePath_.clear();
    fillPath_.clear();

    if (! hasPlot()) return;

    const auto  plot   = plotArea();
    const float ceilDb = ceilingFor (makeupDb_);
    const float span   = ceilDb - floorDb_;
    if (span <= 0.0f) return;

    auto xForDb = [&] (float db)
    {
        return plot.getX() + plot.getWidth() * (db - floorDb_) / span;
    };
    auto yForDb = [&] (float db)
    {
        return plot.getBottom() - plot.getHeight() * (db - floorDb_) / span;
    };

    for (int i = 0; i < kResolution; ++i)
    {
        const float t    = (float) i / (float) (kResolution - 1);
        const float inDb = floorDb_ + t * (ceilDb - floorDb_);

        // Clamped rather than left to run off: a gate's range can drive the
        // output far below the axis floor, and a path with points thousands of
        // units outside the clip region is real work for the renderer.
        const float outDb = juce::jlimit (floorDb_ - 6.0f, ceilDb + 6.0f, outputDb (inDb));

        const juce::Point<float> p { xForDb (inDb), yForDb (outDb) };

        if (i == 0) { curvePath_.startNewSubPath (p); fillPath_.startNewSubPath (p); }
        else        { curvePath_.lineTo (p);          fillPath_.lineTo (p); }
    }

    // The fill closes back along the UNITY diagonal, so the shaded wedge is
    // exactly "what this device is taking off (or adding)" rather than a
    // decorative area under the curve.
    for (int i = kResolution - 1; i >= 0; --i)
    {
        const float t    = (float) i / (float) (kResolution - 1);
        const float inDb = floorDb_ + t * (ceilDb - floorDb_);
        fillPath_.lineTo (xForDb (inDb), yForDb (inDb));
    }
    fillPath_.closeSubPath();
}

void TransferCurveView::paintPlot (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const float a      = dimAlpha();
    const float ceilDb = ceilingFor (makeupDb_);
    const float span   = ceilDb - floorDb_;
    if (span <= 0.0f) return;

    auto xForDb = [&] (float db)
    {
        return plot.getX() + plot.getWidth() * (db - floorDb_) / span;
    };
    auto yForDb = [&] (float db)
    {
        return plot.getBottom() - plot.getHeight() * (db - floorDb_) / span;
    };

    // ---- grid ------------------------------------------------------------
    g.setFont (uiFont (7.5f));
    for (float db = ceilDb; db >= floorDb_; db -= kGridStepDb)
    {
        const float x = xForDb (db);
        const float y = yForDb (db);

        g.setColour (Colours::border.withMultipliedAlpha (a));
        g.fillRect (plot.getX(), y, plot.getWidth(), 0.5f);
        g.fillRect (x, plot.getY(), 0.5f, plot.getHeight());

        // Label the output axis only. Both axes are the same scale — saying so
        // twice costs the plot a third of its width in text.
        if (db < ceilDb - 0.5f && db > floorDb_ + 0.5f && plot.getWidth() > 70.0f)
        {
            g.setColour (Colours::text3.withMultipliedAlpha (0.75f * a));
            g.drawText (juce::String ((int) db),
                        (int) plot.getX() + 2, (int) y - 6, 26, 12,
                        juce::Justification::centredLeft);
        }
    }

    // ---- unity ------------------------------------------------------------
    // The reference the whole picture is read against: where the output would
    // be if the device were doing nothing.
    {
        juce::Path unity, dashed;
        unity.startNewSubPath (xForDb (floorDb_), yForDb (floorDb_));
        unity.lineTo (xForDb (ceilDb), yForDb (ceilDb));

        // Destination and source are separate paths on purpose:
        // createDashedStroke clears its destination first, so passing one path
        // as both empties the input before it is read.
        const float dashes[] { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, unity, dashes, 2);

        g.setColour (Colours::text3.withMultipliedAlpha (0.55f * a));
        g.fillPath (dashed);
    }

    // ---- threshold --------------------------------------------------------
    if (curve_.thresholdDb > floorDb_ && curve_.thresholdDb < ceilDb)
    {
        const float tx = xForDb (curve_.thresholdDb);
        g.setColour (Colours::amber.withMultipliedAlpha (0.35f * a));
        g.fillRect (tx, plot.getY(), 1.0f, plot.getHeight());
    }

    // ---- the curve --------------------------------------------------------
    // resized() is what normally builds these. The guard covers the one case it
    // does not reach: a view painted before it was ever laid out, which is
    // exactly what the editor-paint harness does.
    if (curvePath_.isEmpty()) rebuildPath();

    if (! fillPath_.isEmpty())
    {
        g.setColour (Colours::blue.withAlpha (0.12f * a));
        g.fillPath (fillPath_);
    }

    if (! curvePath_.isEmpty())
    {
        g.setColour (Colours::blue2.withMultipliedAlpha (a));
        g.strokePath (curvePath_, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
    }

    // ---- the live dot -----------------------------------------------------
    // This is the whole reason the view is worth its pixels: a threshold set
    // 10 dB too low looks identical on the knobs and is obvious the moment the
    // signal's dot is sitting the wrong side of the knee.
    //
    // inDb_ > floorDb_ is also what hides the dot for kNoLevel (-200 dB), which
    // is what a bypassed or never-run device reports.
    if (! isDimmed() && inDb_ > floorDb_)
    {
        const float dx = xForDb (juce::jmin (inDb_, ceilDb));
        const float dy = yForDb (juce::jlimit (floorDb_, ceilDb, outputDb (inDb_)));

        // A soft halo first so the dot stays findable over the shaded wedge.
        g.setColour (Colours::amber.withAlpha (0.20f));
        g.fillEllipse (dx - 6.0f, dy - 6.0f, 12.0f, 12.0f);
        g.setColour (Colours::amber);
        g.fillEllipse (dx - 3.0f, dy - 3.0f, 6.0f, 6.0f);

        // The number, bottom-right, where it cannot sit under the curve.
        if (plot.getWidth() > 90.0f && grDb_ < -0.05f)
        {
            g.setColour (Colours::amber.brighter (0.2f));
            g.setFont (uiFont (8.5f, true));
            g.drawText (juce::String (grDb_, 1) + " dB",
                        plot.reduced (3.0f, 1.0f),
                        juce::Justification::bottomRight);
        }
    }
}

} // namespace echojay::viz
