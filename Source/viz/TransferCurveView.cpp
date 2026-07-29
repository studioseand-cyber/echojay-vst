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

    // The glow's frame rate. 60 rather than the editor's 20 because the whole
    // claim of this visual is that it FLOWS, and 20 Hz is the rate at which a
    // moving gradient becomes a sequence of gradients.
    constexpr int kGlowHz = 60;

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
    dwellTap_    = tap;
    dwellActive_ = active;

    if (tap == nullptr)
    {
        stopTimer();

        // Not a fade: there is no longer anything to fade FROM. A detached
        // source means the editor is going away or was never wired, and a glow
        // left frozen on screen would be a claim about a signal nobody is
        // watching any more.
        target_.fill (0.0f);
        displayed_.fill (0.0f);
        smoothed_.fill (0.0f);
        runMax_ = 0.0f;

        if (wasAlive_) { wasAlive_ = false; repaint(); }
        return;
    }

    // Restarted from here rather than run unconditionally, so a device sitting
    // bypassed in a rack costs nothing at all once its glow has faded out. The
    // editor re-passes the tap 20 times a second, so un-bypassing picks the
    // timer straight back up.
    if (active && ! isTimerRunning())
    {
        lastTickMs_ = 0.0;
        startTimerHz (kGlowHz);
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
// The easing timer — layers 2 and 3 of the smoothness (see the header).
// ---------------------------------------------------------------------------
void TransferCurveView::timerCallback()
{
    // dt MEASURED, not assumed. The whole reason this eases with a time
    // constant instead of a fixed multiply is to survive the frames the message
    // thread does not give us; assuming 1/60 here would throw that away.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float  dt  = lastTickMs_ > 0.0 ? (float) ((now - lastTickMs_) * 0.001)
                                         : 1.0f / (float) kGlowHz;
    lastTickMs_ = now;

    // Clamped at both ends: a zero dt stalls the ease, and a 3-second gap
    // (editor hidden, host stalled, machine asleep) would otherwise snap the
    // glow to its target in one visible jump.
    const float dtc = juce::jlimit (0.001f, 0.25f, dt);

    // ---- 1: the published target -----------------------------------------
    if (dwellTap_ != nullptr && dwellActive_)
    {
        float fresh[kDwellBins];

        // A torn read KEEPS the previous target. One frame of standing still is
        // invisible; a frame of anything else is a flicker, which is the one
        // thing this visual is not allowed to do.
        if (dwellTap_->read (fresh))
            std::copy (fresh, fresh + kDwellBins, target_.begin());
    }
    else
    {
        target_.fill (0.0f);
    }

    // ---- 2: frame-rate-independent easing --------------------------------
    const float k = 1.0f - std::exp (-dtc / kEaseTauSec);

    float peak = 0.0f, motion = 0.0f;

    for (int i = 0; i < kDwellBins; ++i)
    {
        const float d = target_[(std::size_t) i] - displayed_[(std::size_t) i];
        displayed_[(std::size_t) i] += d * k;

        motion = std::max (motion, std::abs (d));
        peak   = std::max (peak, displayed_[(std::size_t) i]);
    }

    // ---- the self-scaling reference ---------------------------------------
    {
        const float tau = peak > runMax_ ? kMaxRiseTauSec : kMaxFallTauSec;
        runMax_ += (peak - runMax_) * (1.0f - std::exp (-dtc / tau));
    }

    // ---- 3: spatial smoothing across bins ---------------------------------
    // Two passes of a binomial 5-tap. One pass still leaves the 0.84 dB bin
    // grid faintly readable as steps along the curve where the histogram is
    // steep; two does not, and 128 bins twice is nothing.
    {
        std::array<float, (std::size_t) kDwellBins> scratch;

        auto blur = [] (const std::array<float, (std::size_t) kDwellBins>& src,
                        std::array<float, (std::size_t) kDwellBins>& dst)
        {
            for (int i = 0; i < kDwellBins; ++i)
            {
                // Edge-clamped rather than wrapped or zeroed: bin 0 and bin 127
                // are the ends of a dB axis, not neighbours, and darkening them
                // would draw a fade the signal does not have.
                auto at = [&src] (int j) noexcept
                {
                    return src[(std::size_t) juce::jlimit (0, kDwellBins - 1, j)];
                };

                dst[(std::size_t) i] = (1.0f * at (i - 2) + 4.0f * at (i - 1)
                                      + 6.0f * at (i)
                                      + 4.0f * at (i + 1) + 1.0f * at (i + 2)) * (1.0f / 16.0f);
            }
        };

        blur (displayed_, scratch);
        blur (scratch,    smoothed_);
    }

    // ---- repaint only while there is something to see ---------------------
    const bool alive = peak > kQuiet || motion > kQuiet;

    if (alive || wasAlive_) repaint();     // the trailing frame clears the glow
    wasAlive_ = alive;

    // Faded out AND told it is bypassed: nothing will change again until the
    // editor says otherwise, and it says so 20 times a second.
    if (! alive && ! dwellActive_) stopTimer();
}

float TransferCurveView::dwellIntensity (float inDb) const noexcept
{
    float v = 0.0f;

    if (runMax_ > kQuiet)
    {
        // Interpolated between BIN CENTRES, not snapped to a bin. The bins are
        // already smaller than a pixel on most of these plots, but snapping
        // reintroduces a staircase at exactly the sizes where the plot is
        // biggest and the stepping would be most obvious.
        const float span = echojay::dyn::kDwellTopDb - echojay::dyn::kDwellFloorDb;
        const float f    = (inDb - echojay::dyn::kDwellFloorDb) / span * (float) kDwellBins - 0.5f;

        const int   i0   = (int) std::floor (f);
        const float frac = f - (float) i0;

        auto at = [this] (int j) noexcept
        {
            return smoothed_[(std::size_t) juce::jlimit (0, kDwellBins - 1, j)];
        };

        v = (at (i0) + (at (i0 + 1) - at (i0)) * frac) / runMax_;

        // Normalised against the running max, then gamma-lifted. Linear
        // intensity puts almost the whole plot in the bottom eighth of the
        // ramp — musically the histogram is a spike on a wide quiet base, and
        // the quiet base is most of what there is to look at.
        v = std::sqrt (juce::jlimit (0.0f, 1.0f, v));
    }

    // The whisper. A gaussian, not a marker: at 2 dB sigma it is wider than
    // the bloom it sits inside, so it reads as the curve being slightly hotter
    // just here rather than as a thing travelling along it.
    if (inDb_ > kNoLevel)
    {
        const float d = (inDb - inDb_) / kWhisperSigmaDb;
        v += kWhisperGain * std::exp (-0.5f * d * d);
    }

    return juce::jlimit (0.0f, 1.0f, v);
}

juce::Colour TransferCurveView::dwellColour (float v)
{
    // Cold teal -> amber -> white-hot. The cold end is the palette's own blue,
    // so a silent plot is the same colour the curve has always been and the
    // glow reads as that curve heating up rather than as a second graphic.
    if (v <= 0.5f)
        return Colours::blue.interpolatedWith (Colours::amber, v * 2.0f);

    return Colours::amber.interpolatedWith (juce::Colours::white, (v - 0.5f) * 1.7f);
}

void TransferCurveView::strokeHeat (juce::Graphics& g, int segments, float width,
                                    float baseAlpha, float glowAlpha, float a) const
{
    const int n = (int) curvePts_.size();
    if (n < 2 || segments < 1) return;

    const juce::PathStrokeType stroke (width, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded);
    juce::Path seg;

    for (int s = 0; s < segments; ++s)
    {
        const int i0 = (int) ((std::int64_t) s       * (n - 1) / segments);
        const int i1 = (int) ((std::int64_t) (s + 1) * (n - 1) / segments);
        if (i1 <= i0) continue;

        // Sampled at the segment's MIDPOINT so the colour is centred on the
        // span it paints; sampling at an end walks the whole ramp half a
        // segment off, which shows as the glow lagging the curve where it bends.
        const float v = dwellIntensity (0.5f * (curveInDb_[(std::size_t) i0]
                                              + curveInDb_[(std::size_t) i1]));

        const float alpha = (baseAlpha + glowAlpha * v) * a;

        // Where there is no dwell the bloom layers carry no base alpha, so this
        // is what makes a silent plot cost nothing beyond its core line.
        if (alpha <= 0.004f) continue;

        // Short, LOCAL paths on purpose. Collecting every segment of one colour
        // into a single path and stroking it once sounds cheaper and measured
        // half again as expensive: the runs of a colour are scattered along the
        // curve, so that path's bounding box is the whole plot, and the
        // rasteriser then does full-width work for each of them.
        seg.clear();
        seg.startNewSubPath (curvePts_[(std::size_t) i0]);
        for (int i = i0 + 1; i <= i1; ++i)
            seg.lineTo (curvePts_[(std::size_t) i]);

        g.setColour (dwellColour (v).withAlpha (juce::jmin (1.0f, alpha)));
        g.strokePath (seg, stroke);
    }
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

        if (i == 0) fillPath_.startNewSubPath (p);
        else        fillPath_.lineTo (p);
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

    // Four passes, widest and faintest first, so they sum into a soft bloom
    // around a bright core. JUCE's software renderer has no additive blend
    // mode, so this is the layered-stroke form of one — and layering is what
    // gives the falloff its shape anyway: a genuinely additive single stroke
    // would be uniformly bright out to its edge.
    //
    // The three bloom layers carry NO base alpha, so where there is no dwell
    // they cost nothing and draw nothing; the core layer carries most of its
    // alpha as base, because the curve is a diagram before it is a meter and it
    // has to be readable with the transport stopped.
    //
    // Segment counts are tied to the plot's width so a wide editor gets a finer
    // gradient and a shrunk rack slot does not pay for detail it cannot show —
    // and each layer gets its OWN count, in inverse proportion to its width. A
    // 12 px stroke overlaps its neighbours several times over and blends itself
    // smooth, so subdividing it finely buys nothing visible and costs the most
    // pixels of any layer; the 2 px core is the one whose colour steps would
    // actually be legible.
    {
        const float w = plot.getWidth();
        auto segs = [w] (float per, int lo, int hi)
        {
            return juce::jlimit (lo, hi, (int) (w / per));
        };

        strokeHeat (g, segs (18.0f,  8, 20), 12.0f, 0.0f,  0.085f, a);
        strokeHeat (g, segs (11.0f, 12, 32),  6.5f, 0.0f,  0.13f,  a);
        strokeHeat (g, segs ( 7.0f, 16, 48),  3.5f, 0.0f,  0.20f,  a);
        strokeHeat (g, segs ( 3.5f, 24, 96),  2.0f, 0.55f, 0.45f,  a);
    }

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
