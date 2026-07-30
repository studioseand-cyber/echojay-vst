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

    float autoTopFor (float makeupDb) noexcept
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

TransferCurveView::~TransferCurveView() = default;

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

void TransferCurveView::setHysteresisDb (float db)
{
    // No path rebuild: the band is an overlay drawn from two x coordinates, not
    // part of the curve geometry.
    if (! moved (db, hysteresisDb_, 0.01f)) return;
    hysteresisDb_ = db;
    repaint();
}

void TransferCurveView::setCeilingLineDb (float db)
{
    if (! moved (db, ceilingDb_, 0.01f)) return;
    ceilingDb_ = db;
    repaint();
}

void TransferCurveView::setAxisTopDb (float db)
{
    if (! moved (db, axisTopDb_, 0.01f)) return;
    axisTopDb_ = db;
    rebuildPath();
    repaint();
}

float TransferCurveView::axisTopDb() const noexcept
{
    return axisTopDb_ > kAutoAxisTop ? axisTopDb_ : autoTopFor (makeupDb_);
}

void TransferCurveView::setSelected (bool sel)
{
    if (sel == selected_) return;
    selected_ = sel;
    repaint();
}

float TransferCurveView::plotAlpha() const noexcept
{
    // 0.5 is far enough back that the selected plot is unmistakably the front
    // one, and near enough that a sibling pulling 9 dB is still legible from
    // across the panel — which is the reason four are drawn at all.
    constexpr float kRecede = 0.5f;
    return dimAlpha() * (selected_ ? 1.0f : kRecede);
}

void TransferCurveView::setDwellSource (const echojay::dyn::DwellTap* tap, bool active)
{
    const bool wantTimer = glow_.setSource (tap, active);

    if (tap == nullptr)
    {
        stopTimer();
        repaint();          // a detached source leaves nothing to fade from
        return;
    }

    // Restarted from here rather than run unconditionally, so a device sitting
    // bypassed in a rack costs nothing at all once its glow has faded out. The
    // editor re-passes the tap 20 times a second, so un-bypassing picks the
    // timer straight back up.
    if (wantTimer && ! isTimerRunning())
    {
        lastTickMs_ = 0.0;
        startTimerHz (DwellGlow::kGlowHz);
    }
}

void TransferCurveView::setInputLevelDb (float db)
{
    // No repaint here any more. A quarter-dB move used to be worth a frame
    // because it moved a dot; now it is a gaussian smaller than the bloom
    // around it, and the glow timer is already redrawing at 60 Hz whenever
    // there is anything to see.
    inDb_ = db;
}

void TransferCurveView::setGainReductionDb (float db)
{
    if (! moved (db, grDb_, 0.1f)) return;
    grDb_ = db;
    repaint();
}

// ---------------------------------------------------------------------------
// The glow's frame. Everything about HOW the heat eases and looks is DwellGlow's;
// this only measures the elapsed time and decides whether to keep running.
// ---------------------------------------------------------------------------
void TransferCurveView::timerCallback()
{
    // dt MEASURED, not assumed. The whole reason the glow eases with a time
    // constant instead of a fixed multiply is to survive the frames the message
    // thread does not give us; assuming 1/60 here would throw that away.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float  dt  = lastTickMs_ > 0.0 ? (float) ((now - lastTickMs_) * 0.001)
                                         : 1.0f / (float) DwellGlow::kGlowHz;
    lastTickMs_ = now;

    if (glow_.tick (dt)) repaint();

    // Faded out AND told it is bypassed: nothing will change again until the
    // editor says otherwise, and it says so 20 times a second.
    if (! glow_.isAlive() && ! glow_.isActive()) stopTimer();
}

float TransferCurveView::heatAt (float inDb) const noexcept
{
    // x IS a level on this plot, so the question is the direct one: how much of
    // its time does the signal spend here.
    float v = glow_.intensityAt (inDb);

    // The whisper. A gaussian, not a marker, and added AFTER the contrast so it
    // is not flattened away with the tails: it reads as the curve being
    // slightly hotter just here rather than as a thing travelling along it.
    if (inDb_ > kNoLevel)
    {
        const float d = (inDb - inDb_) / kWhisperSigmaDb;
        v += kWhisperGain * std::exp (-0.5f * d * d);
    }

    return juce::jlimit (0.0f, 1.0f, v);
}

// ---------------------------------------------------------------------------
// The glow's columns. DwellGlow composites and blits; what this supplies is the
// only part that is this view's own — where the curve is at each pixel column,
// and which INPUT LEVEL that column stands for.
// ---------------------------------------------------------------------------
void TransferCurveView::renderGlow (juce::Graphics& g, juce::Rectangle<float> plot, float a)
{
    const int n = (int) curvePts_.size();
    if (n < 2) return;

    const int w = glow_.prepareColumns (plot);
    if (w < 2) return;

    const float originX = std::floor (plot.getX());
    const float originY = std::floor (plot.getY());

    auto& colY = glow_.columnY();
    auto& colE = glow_.columnIntensity();

    for (int ix = 0; ix < w; ++ix)
    {
        // Where this column sits along the curve, in the same parameterisation
        // rebuildPath used — so the field cannot drift off the line it lights.
        const float t   = (originX + (float) ix + 0.5f - plot.getX()) / plot.getWidth();
        const float pos = juce::jlimit (0.0f, 1.0f, t) * (float) (n - 1);

        const int   i0   = juce::jlimit (0, n - 2, (int) pos);
        const float frac = pos - (float) i0;

        const auto& p0 = curvePts_[(std::size_t) i0];
        const auto& p1 = curvePts_[(std::size_t) i0 + 1];

        colY[(std::size_t) ix] = (p0.y + (p1.y - p0.y) * frac) - originY;

        const float inDb = curveInDb_[(std::size_t) i0]
                         + (curveInDb_[(std::size_t) i0 + 1]
                          - curveInDb_[(std::size_t) i0]) * frac;

        colE[(std::size_t) ix] = heatAt (inDb);
    }

    glow_.render (g, a);
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
    curvePts_.clear();
    curveInDb_.clear();
    curvePath_.clear();
    fillPath_.clear();

    if (! hasPlot()) return;

    const auto  plot   = plotArea();
    const float ceilDb = axisTopDb();
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

    curvePts_.reserve ((std::size_t) kResolution);
    curveInDb_.reserve ((std::size_t) kResolution);

    for (int i = 0; i < kResolution; ++i)
    {
        const float t    = (float) i / (float) (kResolution - 1);
        const float inDb = floorDb_ + t * (ceilDb - floorDb_);

        // Clamped rather than left to run off: a gate's range can drive the
        // output far below the axis floor, and a path with points thousands of
        // units outside the clip region is real work for the renderer.
        const float outDb = juce::jlimit (floorDb_ - 6.0f, ceilDb + 6.0f, outputDb (inDb));

        const juce::Point<float> p { xForDb (inDb), yForDb (outDb) };

        // The input dB is kept alongside the point because that is what the
        // glow is indexed by. Recovering it from x at paint time would work and
        // would be a second copy of the axis mapping waiting to disagree.
        curvePts_.push_back (p);
        curveInDb_.push_back (inDb);

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
    const float a      = plotAlpha();
    const float ceilDb = axisTopDb();
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

    // ---- hysteresis band (gate) -------------------------------------------
    // Drawn UNDER the threshold line and the curve, so it reads as a region of
    // the plot rather than as another line competing with them.
    if (hysteresisDb_ > kNoHysteresis)
    {
        const float openDb  = curve_.thresholdDb;
        const float closeDb = curve_.thresholdDb - hysteresisDb_;

        const float x0 = xForDb (juce::jmax (closeDb, floorDb_));
        const float x1 = xForDb (juce::jmin (openDb,  ceilDb));

        if (x1 > x0)
        {
            g.setColour (Colours::amber.withAlpha (0.10f * a));
            g.fillRect (x0, plot.getY(), x1 - x0, plot.getHeight());

            // The close edge, dashed against the threshold's solid line: the
            // gate opens on the solid one and closes on the dashed one, and the
            // gap between them is the chatter this device does not do.
            if (closeDb > floorDb_)
            {
                juce::Path edge, dashed;
                edge.startNewSubPath (x0, plot.getY());
                edge.lineTo (x0, plot.getBottom());

                const float dashes[] { 2.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, edge, dashes, 2);

                g.setColour (Colours::amber.withMultipliedAlpha (0.45f * a));
                g.fillPath (dashed);
            }
        }
    }

    // ---- threshold --------------------------------------------------------
    if (curve_.thresholdDb > floorDb_ && curve_.thresholdDb < ceilDb)
    {
        const float tx = xForDb (curve_.thresholdDb);
        g.setColour (Colours::amber.withMultipliedAlpha (0.35f * a));
        g.fillRect (tx, plot.getY(), 1.0f, plot.getHeight());
    }

    // ---- the curve, as a dwell heatmap ------------------------------------
    // resized() is what normally builds these. The guard covers the one case it
    // does not reach: a view painted before it was ever laid out, which is
    // exactly what the editor-paint harness does.
    if (curvePts_.empty()) rebuildPath();

    if (! fillPath_.isEmpty())
    {
        g.setColour (Colours::blue.withAlpha (0.12f * a));
        g.fillPath (fillPath_);
    }

    // The DIAGRAM first: the curve's shape, cold, in one stroke. Drawn whether
    // or not anything is playing, which is what makes an empty histogram — a
    // bypassed device, a stopped transport, the editor-paint harness — a plain
    // teal curve rather than an empty box.
    if (! curvePath_.isEmpty())
    {
        g.setColour (Colours::blue2.withMultipliedAlpha (DwellGlow::kColdAlpha * a));
        g.strokePath (curvePath_, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
    }

    // Then the HEAT over it, as a per-pixel soft field.
    renderGlow (g, plot, a);

    // ---- ceiling (limiter) ------------------------------------------------
    // OVER the curve, not under it: the curve flattening at the ceiling is a
    // consequence, and this is the guarantee. Everything above the line is a
    // region the output cannot enter, so it is shaded out rather than left as
    // empty plot the eye reads as available headroom.
    if (ceilingDb_ < kNoCeiling && ceilingDb_ > floorDb_ && ceilingDb_ < ceilDb)
    {
        const float cy = yForDb (ceilingDb_);

        g.setColour (Colours::red.withAlpha (0.07f * a));
        g.fillRect (plot.getX(), plot.getY(), plot.getWidth(), cy - plot.getY());

        g.setColour (Colours::red.withMultipliedAlpha (0.75f * a));
        g.fillRect (plot.getX(), cy - 0.5f, plot.getWidth(), 1.5f);

        if (plot.getWidth() > 70.0f)
        {
            g.setFont (uiFont (7.5f, true));
            g.drawText (juce::String (ceilingDb_, 1),
                        (int) plot.getRight() - 32, (int) cy - 11, 30, 10,
                        juce::Justification::centredRight);
        }
    }

    // ---- the reduction readout --------------------------------------------
    // The one thing the glow cannot say. The glow answers "where does the
    // signal live", which is the question the picture exists for; how many dB
    // that is costing right now is a number, and a number is what a number is
    // for.
    //
    // Gated on isDimmed(), NOT on `a`: an unselected band is receded but still
    // processing, and it still has a figure to report. Only a bypassed device
    // has nothing.
    if (! isDimmed() && plot.getWidth() > 90.0f && grDb_ < -0.05f)
    {
        g.setColour (Colours::amber.brighter (0.2f).withMultipliedAlpha (a));
        g.setFont (uiFont (8.5f, true));
        g.drawText (juce::String (grDb_, 1) + " dB",
                    plot.reduced (3.0f, 1.0f),
                    juce::Justification::bottomRight);
    }
}

} // namespace echojay::viz
